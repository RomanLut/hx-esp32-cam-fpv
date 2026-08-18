#pragma once

//===================================================================================
//===================================================================================
// GL/EGL presentation backend for the Android-family ground stations.
//
// One interface, two implementations — pick exactly one per app in CMakeLists:
//   android_surface_backend_window.cpp  phone/tablet: ES2 context on an
//                                       ANativeWindow surface, presents with
//                                       eglSwapBuffers.
//   android_surface_backend_openxr.cpp  Quest: ES3 context created in the OpenXR
//                                       thread's share group on a small pbuffer.
//                                       Nothing is presented here; the renderer
//                                       draws into an FBO whose color texture the
//                                       OpenXR thread samples.
//
// The two are not variants of one algorithm — every non-trivial method body
// differs — so they are deliberately separate translation units rather than one
// file behind #ifdefs. This header exists so the interface cannot drift between
// them.
class GsGlSurfaceBackend
{
public:
    GsGlSurfaceBackend();
    ~GsGlSurfaceBackend();

    // The window implementation takes ownership of an ANativeWindow reference.
    // The OpenXR implementation ignores the pointer value entirely: any non-null
    // handle means "(re-)initialize EGL on the next applyPendingSurface()" and
    // null means "tear down".
    void setSurface(void* surface_handle);
    bool applyPendingSurface(bool vsync_enabled);
    void setVsync(bool enabled);
    bool swapBuffers();
    bool isReady() const;
    int surfaceWidth() const;
    int surfaceHeight() const;

private:
    bool initEgl(bool vsync_enabled);
    void destroyEgl();

    // Currently applied ANativeWindow. Unused by the OpenXR implementation,
    // which never creates a window surface.
    void* m_window = nullptr;
    void* m_pending_window = nullptr;
    void* m_egl_display = nullptr;
    void* m_egl_surface = nullptr;
    void* m_egl_context = nullptr;
    int m_surface_width = 0;
    int m_surface_height = 0;
};
