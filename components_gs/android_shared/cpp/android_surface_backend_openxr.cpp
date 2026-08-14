#include "android_surface_backend.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <chrono>
#include <thread>

#include "Log.h"
#include "openxr_video_bridge.h"

// Defined in openxr_video_bridge.cpp; returns the OpenXR thread's EGLContext so
// the renderer's context can be created in the same share group. The OpenXR
// thread populates this during its own initEgl, before any frames are drawn.
extern "C" void* gsGetSharedEglContext() __attribute__((weak));

namespace
{
constexpr int k_pbuffer_width  = 16;
constexpr int k_pbuffer_height = 16;
}

//===================================================================================
//===================================================================================
GsGlSurfaceBackend::GsGlSurfaceBackend() = default;

//===================================================================================
//===================================================================================
GsGlSurfaceBackend::~GsGlSurfaceBackend()
{
    destroyEgl();
}

//===================================================================================
//===================================================================================
// On Quest the renderer never targets an Android Surface — drawing goes into an
// offscreen FBO whose color texture is shared with the OpenXR thread (see
// openxr_video_bridge.cpp). The window pointer is opaque here: any non-null
// value means "init EGL on next applyPendingSurface()" and null means "tear
// down". The actual pointer value is ignored — we never create a window
// surface, only a small pbuffer to satisfy eglMakeCurrent.
void GsGlSurfaceBackend::setSurface(void* surface_handle)
{
    m_pending_window = surface_handle;
}

//===================================================================================
//===================================================================================
bool GsGlSurfaceBackend::applyPendingSurface(bool vsync_enabled)
{
    destroyEgl();
    if (m_pending_window == nullptr)
    {
        return false;
    }
    m_pending_window = nullptr;
    return initEgl(vsync_enabled);
}

//===================================================================================
//===================================================================================
void GsGlSurfaceBackend::setVsync(bool enabled)
{
    const EGLDisplay display = static_cast<EGLDisplay>(m_egl_display);
    if (display != nullptr && display != EGL_NO_DISPLAY)
    {
        eglSwapInterval(display, enabled ? 1 : 0);
    }
}

//===================================================================================
//===================================================================================
// Pbuffer surfaces don't present anywhere; the renderer reads its FBO into the
// shared color texture each frame and the OpenXR thread samples that into the
// head-locked swapchain. We just drain the GL command queue here.
bool GsGlSurfaceBackend::swapBuffers()
{
    if (!isReady())
    {
        return false;
    }
    // Inserts the fence the OpenXR thread waits on before sampling this frame's texture,
    // and flushes. A bare glFlush does not order our writes against the other context's
    // reads — that ordering is what the fence provides.
    gs::openxr::publishRendererFence();
    return true;
}

//===================================================================================
//===================================================================================
bool GsGlSurfaceBackend::isReady() const
{
    return m_egl_display != nullptr && m_egl_surface != nullptr && m_egl_context != nullptr;
}

//===================================================================================
//===================================================================================
int GsGlSurfaceBackend::surfaceWidth() const
{
    return m_surface_width;
}

//===================================================================================
//===================================================================================
int GsGlSurfaceBackend::surfaceHeight() const
{
    return m_surface_height;
}

//===================================================================================
//===================================================================================
// Initializes EGL with a small pbuffer surface in the OpenXR thread's share
// group. Polls briefly for the OpenXR EGL context to come up since the renderer
// thread can race with OpenXR startup.
bool GsGlSurfaceBackend::initEgl(bool vsync_enabled)
{
    // NOTE: eglGetDisplay(EGL_DEFAULT_DISPLAY) returns the process-wide display, the same
    // object the OpenXR thread and the Android UI use. eglTerminate on it would mark every
    // context and surface in the process for destruction, including the OpenXR thread's,
    // so this function never calls it — it only destroys the objects it created itself.
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY)
    {
        LOGE("renderer EGL: eglGetDisplay(EGL_DEFAULT_DISPLAY) failed: 0x{:X}", eglGetError());
        return false;
    }
    if (eglInitialize(display, nullptr, nullptr) != EGL_TRUE)
    {
        LOGE("renderer EGL: eglInitialize failed: 0x{:X}", eglGetError());
        return false;
    }

    constexpr EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };

    EGLConfig config = nullptr;
    EGLint num_configs = 0;
    if (eglChooseConfig(display, config_attribs, &config, 1, &num_configs) != EGL_TRUE || num_configs != 1)
    {
        LOGE("renderer EGL: eglChooseConfig found no ES3 pbuffer config (num={}): 0x{:X}",
             num_configs, eglGetError());
        return false;
    }

    constexpr EGLint pbuffer_attribs[] = {
        EGL_WIDTH, k_pbuffer_width,
        EGL_HEIGHT, k_pbuffer_height,
        EGL_NONE
    };
    const EGLSurface surface = eglCreatePbufferSurface(display, config, pbuffer_attribs);
    if (surface == EGL_NO_SURFACE)
    {
        LOGE("renderer EGL: eglCreatePbufferSurface failed: 0x{:X}", eglGetError());
        return false;
    }

    EGLContext share_context = EGL_NO_CONTEXT;
    if (gsGetSharedEglContext != nullptr)
    {
        // OpenXR thread sets its EGLContext after its own initEgl; on cold start
        // the renderer can race ahead, so wait briefly (up to ~5 s) for it.
        constexpr int k_share_wait_steps = 500;
        int waited = 0;
        for (; waited < k_share_wait_steps && gsGetSharedEglContext() == nullptr; ++waited)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        share_context = static_cast<EGLContext>(gsGetSharedEglContext());
        if (share_context != EGL_NO_CONTEXT && waited > 0)
        {
            LOGI("renderer EGL: waited {} ms for the OpenXR share context", waited * 10);
        }
    }
    else
    {
        LOGE("renderer EGL: gsGetSharedEglContext symbol is missing - this build cannot "
             "share textures with the OpenXR thread");
    }

    if (share_context == EGL_NO_CONTEXT)
    {
        // Previously this fell through and built an unshared context. The texture id then
        // published to the OpenXR thread names a different object (or nothing) in that
        // thread's context, which produces garbage or a GPU fault instead of an error.
        LOGE("renderer EGL: FATAL no OpenXR share context available after waiting; refusing "
             "to create an unshared context because the published texture would be invalid "
             "in the OpenXR thread");
        eglDestroySurface(display, surface);
        gs::openxr::setRendererContextShared(false);
        return false;
    }

    constexpr EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    const EGLContext context = eglCreateContext(display, config, share_context, context_attribs);
    if (context == EGL_NO_CONTEXT)
    {
        LOGE("renderer EGL: eglCreateContext (shared) failed: 0x{:X}", eglGetError());
        eglDestroySurface(display, surface);
        gs::openxr::setRendererContextShared(false);
        return false;
    }

    if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE)
    {
        LOGE("renderer EGL: eglMakeCurrent failed: 0x{:X}", eglGetError());
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        gs::openxr::setRendererContextShared(false);
        return false;
    }

    gs::openxr::setRendererContextShared(true);
    LOGI("renderer EGL: context ready in the OpenXR share group");

    eglSwapInterval(display, vsync_enabled ? 1 : 0);

    // The pbuffer is just a placeholder for eglMakeCurrent; actual drawing
    // happens in a 1280x720 FBO created by the renderer. Reporting that size
    // here keeps the renderer's working dimensions consistent.
    m_surface_width = 1280;
    m_surface_height = 720;
    m_egl_display = display;
    m_egl_surface = surface;
    m_egl_context = context;
    return true;
}

//===================================================================================
//===================================================================================
void GsGlSurfaceBackend::destroyEgl()
{
    const EGLDisplay display = static_cast<EGLDisplay>(m_egl_display);
    const EGLSurface surface = static_cast<EGLSurface>(m_egl_surface);
    const EGLContext context = static_cast<EGLContext>(m_egl_context);

    if (display != nullptr && display != EGL_NO_DISPLAY)
    {
        // Drop the shared fence while this context is still current — it was created here.
        if (context != nullptr && context != EGL_NO_CONTEXT)
        {
            gs::openxr::resetRendererFence();
        }
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != nullptr && context != EGL_NO_CONTEXT)
        {
            eglDestroyContext(display, context);
        }
        if (surface != nullptr && surface != EGL_NO_SURFACE)
        {
            eglDestroySurface(display, surface);
        }
        // Deliberately no eglTerminate: the display is shared with the OpenXR thread and the
        // Android UI, and terminating it here would tear down their contexts too.
    }

    gs::openxr::setRendererContextShared(false);
    m_egl_display = nullptr;
    m_egl_surface = nullptr;
    m_egl_context = nullptr;
    m_surface_width = 0;
    m_surface_height = 0;
}
