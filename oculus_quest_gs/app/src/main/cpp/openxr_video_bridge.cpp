#include "openxr_video_bridge.h"

#include <GLES3/gl3.h>

#include <atomic>
#include <deque>
#include <mutex>

namespace gs::openxr
{
namespace
{
std::mutex g_input_mutex;
std::deque<int> g_pending_imgui_keys;

std::mutex g_share_mutex;
void* g_shared_egl_context = nullptr;

std::mutex g_renderer_tex_mutex;
unsigned int g_renderer_tex = 0;
int g_renderer_tex_w = 0;
int g_renderer_tex_h = 0;

// Guards g_renderer_fence for the whole duration of the glWaitSync/glDeleteSync calls, so a
// fence can never be deleted out from under a consumer that is about to wait on it. Both
// operations are cheap: glWaitSync only enqueues a server-side wait and does not block the
// CPU, and glFenceSync only inserts a command.
std::mutex g_fence_mutex;
GLsync g_renderer_fence = nullptr;

std::atomic<bool> g_renderer_context_shared{false};
}

//===================================================================================
//===================================================================================
// Queues an ImGui key for the renderer to consume on its next frame.
void publishImGuiKey(int imgui_key)
{
    std::lock_guard<std::mutex> lock(g_input_mutex);
    // Cap the queue at a small bound so a stuck consumer cannot grow it without limit.
    if (g_pending_imgui_keys.size() >= 64)
    {
        g_pending_imgui_keys.pop_front();
    }
    g_pending_imgui_keys.push_back(imgui_key);
}

//===================================================================================
//===================================================================================
// Pops the oldest pending ImGui key. Returns false when the queue is empty.
bool consumeImGuiKey(int& out_imgui_key)
{
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (g_pending_imgui_keys.empty())
    {
        return false;
    }
    out_imgui_key = g_pending_imgui_keys.front();
    g_pending_imgui_keys.pop_front();
    return true;
}

//===================================================================================
//===================================================================================
// Stores the OpenXR thread's EGLContext so the renderer's EGL context can be
// created in the same share group (textures visible across threads).
void setSharedEglContext(void* egl_context)
{
    std::lock_guard<std::mutex> lock(g_share_mutex);
    g_shared_egl_context = egl_context;
}

void* getSharedEglContext()
{
    std::lock_guard<std::mutex> lock(g_share_mutex);
    return g_shared_egl_context;
}

//===================================================================================
//===================================================================================
// Renderer publishes its offscreen color texture each time it's reallocated; the
// OpenXR thread samples this texture into its head-locked quad swapchain.
void publishRendererTexture(unsigned int gl_texture, int width, int height)
{
    std::lock_guard<std::mutex> lock(g_renderer_tex_mutex);
    g_renderer_tex = gl_texture;
    g_renderer_tex_w = width;
    g_renderer_tex_h = height;
}

bool getRendererTexture(unsigned int& gl_texture, int& width, int& height)
{
    std::lock_guard<std::mutex> lock(g_renderer_tex_mutex);
    if (g_renderer_tex == 0 || g_renderer_tex_w <= 0 || g_renderer_tex_h <= 0)
    {
        return false;
    }
    gl_texture = g_renderer_tex;
    width = g_renderer_tex_w;
    height = g_renderer_tex_h;
    return true;
}

//===================================================================================
//===================================================================================
// Renderer thread: inserts a fence after the frame's draw commands. The OpenXR thread
// waits on it before sampling the shared texture.
void publishRendererFence()
{
    const GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (fence == nullptr)
    {
        return;
    }
    // Required: a sync object is not guaranteed to be observable by other contexts in the
    // share group until the commands preceding it have been flushed.
    glFlush();

    GLsync previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_fence_mutex);
        previous = g_renderer_fence;
        g_renderer_fence = fence;
    }
    if (previous != nullptr)
    {
        glDeleteSync(previous);
    }
}

//===================================================================================
//===================================================================================
// OpenXR thread: makes this context's subsequent sampling wait for the renderer's writes.
// Server-side wait — it does not stall the CPU or the frame loop.
void waitForRendererFence()
{
    std::lock_guard<std::mutex> lock(g_fence_mutex);
    if (g_renderer_fence == nullptr)
    {
        return;
    }
    glWaitSync(g_renderer_fence, 0, GL_TIMEOUT_IGNORED);
}

//===================================================================================
//===================================================================================
// Drops the pending fence. Called on teardown while a context is still current.
void resetRendererFence()
{
    GLsync fence = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_fence_mutex);
        fence = g_renderer_fence;
        g_renderer_fence = nullptr;
    }
    if (fence != nullptr)
    {
        glDeleteSync(fence);
    }
}

//===================================================================================
//===================================================================================
void setRendererContextShared(bool shared)
{
    g_renderer_context_shared.store(shared);
}

bool isRendererContextShared()
{
    return g_renderer_context_shared.load();
}

} // namespace gs::openxr

//===================================================================================
//===================================================================================
// Weak hook called by the renderer's surface backend to grab the OpenXR thread's
// EGLContext for share-group construction. Returns nullptr if not yet set.
extern "C" void* gsGetSharedEglContext()
{
    return gs::openxr::getSharedEglContext();
}

//===================================================================================
//===================================================================================
// Weak hook called by the shared renderer to publish its offscreen color texture
// to the OpenXR thread. Avoids a CPU-side glReadPixels round-trip.
extern "C" void gsPublishRendererTexture(unsigned int gl_texture, int width, int height)
{
    gs::openxr::publishRendererTexture(gl_texture, width, height);
}

//===================================================================================
//===================================================================================
// Weak hook called by the shared renderer each frame to drain controller-derived
// keys produced by the OpenXR thread. Returns 0 (ImGuiKey_None) when empty.
extern "C" int gsTryConsumeXrImGuiKey()
{
    int key = 0;
    return gs::openxr::consumeImGuiKey(key) ? key : 0;
}
