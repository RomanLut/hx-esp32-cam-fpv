#pragma once

class GsGlSurfaceBackend
{
public:
    GsGlSurfaceBackend();
    ~GsGlSurfaceBackend();

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

    // Non-null sentinel (set by setSurface) signals that the renderer should
    // (re-)initialize EGL on the next applyPendingSurface(). On Quest the
    // surface_handle is ignored — drawing targets a shared GL texture, not
    // an Android Surface.
    void* m_pending_window = nullptr;
    void* m_egl_display = nullptr;
    void* m_egl_surface = nullptr;
    void* m_egl_context = nullptr;
    int m_surface_width = 0;
    int m_surface_height = 0;
};
