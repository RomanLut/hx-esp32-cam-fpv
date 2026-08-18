#include "android_surface_backend.h"

#include <EGL/egl.h>
#include <android/native_window.h>

namespace
{
//===================================================================================
//===================================================================================
// Converts opaque surface handle to ANativeWindow pointer
ANativeWindow* asNativeWindow(void* surface_handle)
{
    return static_cast<ANativeWindow*>(surface_handle);
}
}

//===================================================================================
//===================================================================================
// Default constructor for GL surface backend
GsGlSurfaceBackend::GsGlSurfaceBackend() = default;

//===================================================================================
//===================================================================================
// Destructor - cleans up EGL resources and native window references
GsGlSurfaceBackend::~GsGlSurfaceBackend()
{
    destroyEgl();
    if (m_pending_window != nullptr)
    {
        ANativeWindow_release(asNativeWindow(m_pending_window));
        m_pending_window = nullptr;
    }
    if (m_window != nullptr)
    {
        ANativeWindow_release(asNativeWindow(m_window));
        m_window = nullptr;
    }
}

//===================================================================================
//===================================================================================
// Sets new surface handle to be applied on next frame
void GsGlSurfaceBackend::setSurface(void* surface_handle)
{
    if (m_pending_window != nullptr)
    {
        ANativeWindow_release(asNativeWindow(m_pending_window));
    }
    m_pending_window = surface_handle;
}

//===================================================================================
//===================================================================================
// Applies pending surface and initializes EGL context with requested vsync mode
bool GsGlSurfaceBackend::applyPendingSurface(bool vsync_enabled)
{
    destroyEgl();
    if (m_window != nullptr)
    {
        ANativeWindow_release(asNativeWindow(m_window));
        m_window = nullptr;
    }

    m_window = m_pending_window;
    m_pending_window = nullptr;

    if (m_window == nullptr)
    {
        return false;
    }

    return initEgl(vsync_enabled);
}

//===================================================================================
//===================================================================================
// Enables or disables vertical synchronization for buffer swaps
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
// Swaps front and back buffers to display rendered content
bool GsGlSurfaceBackend::swapBuffers()
{
    if (!isReady())
    {
        return false;
    }

    return eglSwapBuffers(static_cast<EGLDisplay>(m_egl_display),
                          static_cast<EGLSurface>(m_egl_surface)) == EGL_TRUE;
}

//===================================================================================
//===================================================================================
// Checks if surface backend is ready for rendering
bool GsGlSurfaceBackend::isReady() const
{
    return m_egl_display != nullptr && m_egl_surface != nullptr && m_egl_context != nullptr;
}

//===================================================================================
//===================================================================================
// Returns current surface width in pixels
int GsGlSurfaceBackend::surfaceWidth() const
{
    return m_surface_width;
}

//===================================================================================
//===================================================================================
// Returns current surface height in pixels
int GsGlSurfaceBackend::surfaceHeight() const
{
    return m_surface_height;
}

//===================================================================================
//===================================================================================
// Initializes EGL display, surface and OpenGL ES context for current window
bool GsGlSurfaceBackend::initEgl(bool vsync_enabled)
{
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY)
    {
        return false;
    }
    if (eglInitialize(display, nullptr, nullptr) != EGL_TRUE)
    {
        return false;
    }

    constexpr EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
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
        eglTerminate(display);
        return false;
    }

    const EGLSurface surface = eglCreateWindowSurface(display, config, asNativeWindow(m_window), nullptr);
    if (surface == EGL_NO_SURFACE)
    {
        eglTerminate(display);
        return false;
    }

    constexpr EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
    if (context == EGL_NO_CONTEXT)
    {
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return false;
    }

    if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE)
    {
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return false;
    }

    eglSwapInterval(display, vsync_enabled ? 1 : 0);

    m_surface_width = ANativeWindow_getWidth(asNativeWindow(m_window));
    m_surface_height = ANativeWindow_getHeight(asNativeWindow(m_window));
    m_egl_display = display;
    m_egl_surface = surface;
    m_egl_context = context;
    return true;
}

//===================================================================================
//===================================================================================
// Destroys EGL context, surface and terminates display connection
void GsGlSurfaceBackend::destroyEgl()
{
    const EGLDisplay display = static_cast<EGLDisplay>(m_egl_display);
    const EGLSurface surface = static_cast<EGLSurface>(m_egl_surface);
    const EGLContext context = static_cast<EGLContext>(m_egl_context);

    if (display != nullptr && display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    if (display != nullptr && display != EGL_NO_DISPLAY)
    {
        if (context != nullptr && context != EGL_NO_CONTEXT)
        {
            eglDestroyContext(display, context);
        }
        if (surface != nullptr && surface != EGL_NO_SURFACE)
        {
            eglDestroySurface(display, surface);
        }
        eglTerminate(display);
    }

    m_egl_display = nullptr;
    m_egl_surface = nullptr;
    m_egl_context = nullptr;
    m_surface_width = 0;
    m_surface_height = 0;
}
