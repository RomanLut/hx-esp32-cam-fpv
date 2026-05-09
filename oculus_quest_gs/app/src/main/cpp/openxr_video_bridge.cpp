#include "openxr_video_bridge.h"

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
