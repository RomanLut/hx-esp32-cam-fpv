#include "gs_video_renderer.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "Log.h"
#include "core/osd_menu_controller.h"
#include "core/osd_menu_imgui_shared.h"
#include "core/stats_panel_shared.h"
#include "gs_playback_manager.h"
#include "gs_recordings_storage.h"
#include "gs_runtime_input.h"
#include "gs_runtime_state.h"
#include "gs_shared_state.h"
#include "gs_runtime_menu_ui.h"
#include "gs_top_overlay_shared.h"
#include "gs_camera_calibration_shared.h"
#include "gs_video_stabilization_shared.h"
#include "gs_video_layout_shared.h"
#include "gs_video_shader_renderer.h"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "utils/lodepng.h"
#include "../mcp/gs_mcp_server.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "../../components/common/Clock.h"

namespace
{

constexpr float kLinuxMenuFontGlobalScale = 2.0f;
constexpr float kTouchNavLabelScale = 0.75f;

bool shouldReplacePendingFrame(uint32_t new_frame_id, uint32_t old_frame_id)
{
    return new_frame_id > old_frame_id ||
           (old_frame_id >= 10u && new_frame_id < old_frame_id - 10u);
}

bool pointInRect(float x, float y, const GsVideoRenderer::Rect& rect)
{
    return rect.width > 0.0f &&
           rect.height > 0.0f &&
           x >= rect.x &&
           x <= rect.x + rect.width &&
           y >= rect.y &&
           y <= rect.y + rect.height;
}

void logGlError(const char* stage)
{
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR)
    {
        LOGE("{} glError=0x{:04x}", stage, static_cast<unsigned int>(error));
    }
}

// Weak hooks defined by the Quest build's OpenXR bridge; absent on android_gs
// and Linux. Their presence also acts as the "Quest immersive build" sentinel.
//
// gsTryConsumeXrImGuiKey: returns the next pending ImGui key produced by the
// OpenXR controller-input thread (or 0 when the queue is empty).
// gsPublishRendererTexture: receives the renderer's offscreen color texture id
// so the OpenXR thread can sample it directly into the head-locked swapchain.
extern "C" int gsTryConsumeXrImGuiKey() __attribute__((weak));
extern "C" void gsPublishRendererTexture(unsigned int gl_texture, int width, int height) __attribute__((weak));

} // namespace

//===================================================================================
//===================================================================================
// Constructor for GsVideoRenderer. Initializes the video rendering system.
GsVideoRenderer::GsVideoRenderer()
    : m_thread(&GsVideoRenderer::run, this)
{
}

//===================================================================================
//===================================================================================
// Destructor for GsVideoRenderer. Cleans up rendering resources.
GsVideoRenderer::~GsVideoRenderer()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_exit = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
    {
        m_thread.join();
    }

    releaseFrameRefLocked(m_locked_frame);
    releaseFrameRefLocked(m_pending_frame);
}

//===================================================================================
//===================================================================================
// Sets the rendering surface handle for the video renderer.
void GsVideoRenderer::setSurface(void* surface_handle)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_surface_backend.setSurface(surface_handle);
    m_surface_dirty = true;
    m_cv.notify_all();
}

//===================================================================================
//===================================================================================
// Clears the current rendering surface.
void GsVideoRenderer::clearSurface()
{
    setSurface(nullptr);
}

//===================================================================================
//===================================================================================
// Submits a video frame for rendering using raw pixel data.
void GsVideoRenderer::submitFrame(const uint8_t* pixels,
                                       size_t size,
                                       int width,
                                       int height,
                                       int stride,
                                       uint32_t frame_id,
                                       PixelFormat pixel_format,
                                       const gs::render::VideoPostprocessingParams& postprocessing_params)
{
    const int min_stride = pixel_format == PixelFormat::RGB565
        ? width * 2
        : (pixel_format == PixelFormat::RGBA8888 ? width * 4 : width * 3);
    if (pixels == nullptr || size == 0 || width <= 0 || height <= 0 || stride < min_stride)
    {
        return;
    }

    PendingFrame frame;
    frame.pixels.assign(pixels, pixels + size);
    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.frame_id = frame_id;
    frame.pixel_format = pixel_format;
    frame.postprocessing_params = postprocessing_params;
    submitPendingFrame(std::move(frame));
}

//===================================================================================
//===================================================================================
// Submits a video frame for rendering with external frame reference management.
void GsVideoRenderer::submitFrame(std::shared_ptr<void> external_frame_ref,
                                       const uint8_t* pixels,
                                       size_t size,
                                       int width,
                                       int height,
                                       int stride,
                                       uint32_t frame_id,
                                       PixelFormat pixel_format,
                                       bool stabilization_prepared,
                                       const gs::render::VideoPostprocessingParams& postprocessing_params)
{
    const int min_stride = pixel_format == PixelFormat::RGB565
        ? width * 2
        : (pixel_format == PixelFormat::RGBA8888 ? width * 4 : width * 3);
    if (!external_frame_ref ||
        pixels == nullptr ||
        size == 0 ||
        width <= 0 ||
        height <= 0 ||
        stride < min_stride)
    {
        return;
    }

    PendingFrame frame;
    frame.external_frame_ref = std::move(external_frame_ref);
    frame.external_pixels = pixels;
    frame.external_size = size;
    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.frame_id = frame_id;
    frame.pixel_format = pixel_format;
    frame.stabilization_prepared = stabilization_prepared;
    frame.postprocessing_params = postprocessing_params;
    submitPendingFrame(std::move(frame));
}

//===================================================================================
//===================================================================================
// Queues a validated frame for the render thread, replacing older pending frames.
void GsVideoRenderer::submitPendingFrame(PendingFrame&& frame)
{
    const GsVisionImageFormat calibration_format = frame.pixel_format == PixelFormat::RGB565
        ? GS_VISION_IMAGE_FORMAT_RGB565
        : (frame.pixel_format == PixelFormat::RGBA8888
            ? GS_VISION_IMAGE_FORMAT_RGBA8
            : GS_VISION_IMAGE_FORMAT_RGB8);
    gs::calibration::captureReadyFrame(frame.external_pixels != nullptr ? frame.external_pixels : frame.pixels.data(),
                                       frame.external_pixels != nullptr ? frame.external_size : frame.pixels.size(),
                                       frame.width,
                                       frame.height,
                                       frame.stride,
                                       calibration_format);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_has_pending_frame)
        {
            if (!shouldReplacePendingFrame(frame.frame_id, m_pending_frame.frame_id))
            {
                m_discarded_pending_count.fetch_add(1);
                releaseFrameRefLocked(frame);
                return;
            }
            m_discarded_pending_count.fetch_add(1);
        }
        releaseFrameRefLocked(m_pending_frame);
        m_pending_frame = std::move(frame);
        m_has_pending_frame = true;
        m_frame_dirty.store(true);
    }
    m_cv.notify_all();
}

//===================================================================================
//===================================================================================
// Sets the screen mode for video rendering.
void GsVideoRenderer::setScreenMode(int screen_mode)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_screen_mode = std::clamp(screen_mode, 0, 2);
    m_mode_dirty = true;
    m_cv.notify_all();
}

//===================================================================================
//===================================================================================
// Enables or disables vertical sync for rendering. On Quest (OpenXR bridge
// present) the SurfaceView is never visible, so we always force vsync off —
// otherwise eglSwapBuffers blocks on SurfaceFlinger throttling and pins the
// renderer thread to a few FPS.
void GsVideoRenderer::setVsync(bool enabled)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (gsPublishRendererTexture != nullptr)
    {
        enabled = false;
    }
    m_vsync = enabled;
    m_surface_backend.setVsync(enabled);
}

//===================================================================================
//===================================================================================
// Enables or disables VR mode for video rendering.
void GsVideoRenderer::setVrMode(bool enabled)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (gsPublishRendererTexture != nullptr)
    {
        // OpenXR layer is already presented to both eyes; force single-eye render.
        enabled = false;
    }
    m_vr_mode = enabled;
    m_mode_dirty = true;
    m_cv.notify_all();
}

//===================================================================================
//===================================================================================
// Updates the flight OSD data for display.
void GsVideoRenderer::updateFlightOsd(const uint8_t* data, uint16_t size)
{
    if (data == nullptr || size == 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    s_flightOSD.update(data, size);
    m_overlay_dirty = true;
    m_cv.notify_all();
}

//===================================================================================
//===================================================================================
// Clears the flight OSD display.
void GsVideoRenderer::clearFlightOsd()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    s_flightOSD.clear();
    m_overlay_dirty = true;
    m_cv.notify_all();
}

//===================================================================================
//===================================================================================
// Sets the font for flight OSD display.
void GsVideoRenderer::setFlightOsdFont(const std::string& font_name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    s_flightOSD.setFontName(font_name);
    m_overlay_dirty = true;
    m_cv.notify_all();
}


//===================================================================================
//===================================================================================
// Stores the top overlay payload separately from the frame UI state.
//===================================================================================
//===================================================================================
// Sets the overlay input data for display.
void GsVideoRenderer::setOverlayInput(const gs::imgui::TopOverlayData& overlay_input)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_overlay_input = overlay_input;
    m_overlay_dirty = true;
    m_cv.notify_all();
}

//===================================================================================
//===================================================================================
// Stores the shared frame UI state for the renderer thread.
//===================================================================================
//===================================================================================
// Sets the frame UI state for overlay rendering.
void GsVideoRenderer::setFrameUiState(const RuntimeFrameUiState& frame_ui_state)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frame_ui_state = frame_ui_state;
    m_screen_mode = std::clamp(static_cast<int>(frame_ui_state.screen_mode), 0, 2);
    // On Quest (OpenXR bridge present) the SurfaceView is hidden behind the
    // head-locked layer; vsync on its EGL surface only blocks the renderer.
    m_vsync = (gsPublishRendererTexture != nullptr) ? false : frame_ui_state.vsync;
    // OpenXR composition presents the same quad to both eyes natively, so the
    // renderer should output a single-eye view (otherwise the framebuffer is
    // 2x wider, doubling glReadPixels cost and stretching content on the quad).
    m_vr_mode = (gsPublishRendererTexture != nullptr) ? false : frame_ui_state.vr_mode;
    m_zoom = frame_ui_state.screen_zoom;
    m_vr_separation = frame_ui_state.vr_separation;
    m_menu_footer = frame_ui_state.menu_footer;
    m_surface_backend.setVsync(m_vsync);
    m_overlay_dirty = true;
    m_cv.notify_all();
}

//===================================================================================
//===================================================================================
// Sets the statistics snapshot for overlay display.
void GsVideoRenderer::setOverlayStatsSnapshot(const gs::stats::FullscreenStatsSnapshot& snapshot)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_overlay_stats_snapshot = snapshot;
}

//===================================================================================
//===================================================================================
// Binds the menu controller and configuration for OSD menu handling.
void GsVideoRenderer::setMenuBinding(gs::menu::OSDMenuController* menu_controller,
                                          Ground2Air_Config_Packet* config_packet,
                                          std::mutex* menu_mutex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_menu_controller = menu_controller;
    m_menu_config = config_packet;
    m_menu_mutex = menu_mutex;
}

//===================================================================================
//===================================================================================
// Invalidates the currently displayed video frame and discards any stale queued frame data.
void GsVideoRenderer::invalidateDisplayedFrame()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    releaseFrameRefLocked(m_locked_frame);
    m_locked_frame = {};
    releaseFrameRefLocked(m_pending_frame);
    m_pending_frame = {};
    m_has_pending_frame = false;
    m_frame_width = 0;
    m_frame_height = 0;
    m_uploaded_width = 0;
    m_uploaded_height = 0;
    m_has_uploaded_frame = false;
    m_frame_dirty.store(false);
    m_overlay_dirty = true;
    gs::stabilization::reset();
    m_cv.notify_all();
}

//===================================================================================
//===================================================================================
// Sets the footer text for the menu display.
void GsVideoRenderer::setMenuFooter(const std::string& footer)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_menu_footer = footer;
}

//===================================================================================
//===================================================================================
// Checks if the OSD menu is currently visible.
bool GsVideoRenderer::isMenuVisible()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_menu_visible;
}

//===================================================================================
//===================================================================================
// Queues a request to open the OSD menu.
void GsVideoRenderer::queueMenuOpen()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_open_menu_requested = true;
    m_open_playback_menu_requested = false;
    m_close_menu_requested = false;
    m_overlay_dirty = true;
    m_cv.notify_all();
}

//===================================================================================
//===================================================================================
// Queues a request to open the Playback menu with the previous file selection.
void GsVideoRenderer::queuePlaybackMenuOpen()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_open_playback_menu_requested = true;
    m_open_menu_requested = false;
    m_close_menu_requested = false;
    m_overlay_dirty = true;
    m_cv.notify_all();
}

//===================================================================================
//===================================================================================
// Queues a request to close the OSD menu.
void GsVideoRenderer::queueMenuClose()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_close_menu_requested = true;
    m_open_menu_requested = false;
    m_open_playback_menu_requested = false;
    m_overlay_dirty = true;
    m_cv.notify_all();
}

//===================================================================================
//===================================================================================
// Queues a key press event for menu navigation.
void GsVideoRenderer::queueKeyPress(ImGuiKey key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Android/touch navigation can report the same logical key through more than one path before the next frame.
    if (std::find(m_pending_key_presses.begin(), m_pending_key_presses.end(), key) != m_pending_key_presses.end())
    {
        return;
    }
    m_pending_key_presses.push_back(key);
    m_cv.notify_all();
}

//===================================================================================
//===================================================================================
// Dispatches a touch tap against runtime overlay controls and returns
// the semantic menu action. Runtime controls intentionally do not rely on raw
// ImGui mouse routing: VR draws the canonical left-half ImGui UI into both eyes
// after layout, so final screen coordinates must be transformed back here before
// hit testing.
GsVideoRenderer::MenuAction GsVideoRenderer::dispatchTap(float x, float y)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_imgui_context == nullptr)
    {
        return {};
    }

    // In VR mode the right half mirrors the left half; translate right-half taps
    // to their left-half equivalents so all bounds checks work for both eyes.
    // Also undo the VR separation offset so hit regions match the shifted visuals.
    if (m_vr_mode)
    {
        const float half_w = static_cast<float>(m_surface_width) * 0.5f;
        const float offset = m_vr_separation * half_w;
        if (x >= half_w)
        {
            x -= half_w;
            x += offset;  // right eye content shifted left by offset
        }
        else
        {
            x -= offset;  // left eye content shifted right by offset
        }
    }

    if (pointInRect(x, y, m_nav_up_bounds))
    {
        return {MenuActionKind::Up, -1};
    }
    if (pointInRect(x, y, m_nav_down_bounds))
    {
        return {MenuActionKind::Down, -1};
    }
    if (pointInRect(x, y, m_nav_left_bounds))
    {
        return {MenuActionKind::Back, -1};
    }
    if (pointInRect(x, y, m_nav_right_bounds))
    {
        return {MenuActionKind::Right, -1};
    }
    if (pointInRect(x, y, m_nav_center_bounds))
    {
        return {MenuActionKind::Activate, -1};
    }
    if (pointInRect(x, y, m_nav_gs_rec_bounds))
    {
        return {MenuActionKind::GsRec, -1};
    }
    if (pointInRect(x, y, m_nav_air_rec_bounds))
    {
        return {MenuActionKind::AirRec, -1};
    }

    for (size_t index = 0; index < m_menu_item_bounds.size(); ++index)
    {
        if (pointInRect(x, y, m_menu_item_bounds[index]))
        {
            return {MenuActionKind::SelectItem, static_cast<int>(index)};
        }
    }

    if (!pointInRect(x, y, m_menu_bounds))
    {
        return {MenuActionKind::Outside, -1};
    }

    return {};
}

//===================================================================================
//===================================================================================
// Consumes and returns the current renderer performance statistics.
GsVideoRenderer::RendererStats GsVideoRenderer::consumeStats()
{
    RendererStats stats;
    stats.upload_count = m_upload_count.exchange(0);
    stats.upload_total_ms = m_upload_total_ms.exchange(0);
    stats.upload_min_ms = m_upload_min_ms.exchange(99);
    stats.upload_max_ms = m_upload_max_ms.exchange(0);
    stats.discarded_pending_count = m_discarded_pending_count.exchange(0);
    stats.swap_count = m_swap_count.exchange(0);
    stats.swap_total_ms = m_swap_total_ms.exchange(0);
    stats.swap_min_ms = m_swap_min_ms.exchange(99);
    stats.swap_max_ms = m_swap_max_ms.exchange(0);
    stats.gpu_wait_last_ms = m_gpu_wait_last_ms.load();
    stats.gpu_wait_max_ms = stats.swap_count > 0 ? stats.swap_max_ms : 0;
    if (stats.upload_count == 0)
    {
        stats.upload_min_ms = 99;
    }
    if (stats.swap_count == 0)
    {
        stats.swap_min_ms = 99;
    }
    return stats;
}

//===================================================================================
//===================================================================================
// Main rendering thread function that processes frames and handles rendering.
void GsVideoRenderer::run()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_exit)
    {
        // Cap the idle wait at 50 ms so the menu/UI keeps ticking at >= 20 FPS
        // even when no video frames are arriving. Mirrors processLinuxFrameTick().
        m_cv.wait_for(lock, std::chrono::milliseconds(50), [this] {
            return m_exit ||
                   m_surface_dirty ||
                   m_frame_dirty.load() ||
                   m_mode_dirty ||
                   m_overlay_dirty ||
                   !m_pending_key_presses.empty();
        });

        if (m_exit)
        {
            break;
        }

        if (m_surface_dirty)
        {
            applyPendingSurfaceLocked();
        }

        if (!m_surface_backend.isReady())
        {
            m_frame_dirty.store(false);
            m_mode_dirty = false;
            m_overlay_dirty = false;
            continue;
        }

        if (m_frame_dirty.load())
        {
            bool have_new_frame = false;
            if (m_has_pending_frame)
            {
                releaseFrameRefLocked(m_locked_frame);
                m_locked_frame = std::move(m_pending_frame);
                m_pending_frame = {};
                m_has_pending_frame = false;
                m_frame_width = m_locked_frame.width;
                m_frame_height = m_locked_frame.height;
                have_new_frame = true;
            }

            if (have_new_frame)
            {
                uploadFrameLocked();
            }
            else
            {
                drawFrameLocked();
            }

            m_frame_dirty.store(m_has_pending_frame);
        }
        else
        {
            // Always redraw on tick (event-driven OR 50 ms timeout) so the
            // menu animates and queued key presses surface without video.
            drawFrameLocked();
        }

        m_mode_dirty = false;
        m_overlay_dirty = m_redraw_after_current_frame;
        m_redraw_after_current_frame = false;
    }

    destroyImGuiLocked();
    m_video_shader_renderer.release();
    if (m_texture != 0)
    {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }
    if (m_offscreen_fbo != 0)
    {
        glDeleteFramebuffers(1, &m_offscreen_fbo);
        m_offscreen_fbo = 0;
    }
    if (m_offscreen_tex != 0)
    {
        glDeleteTextures(1, &m_offscreen_tex);
        m_offscreen_tex = 0;
    }
    m_offscreen_w = 0;
    m_offscreen_h = 0;
    if (gsPublishRendererTexture != nullptr)
    {
        gsPublishRendererTexture(0, 0, 0);
    }
}

//===================================================================================
//===================================================================================
// Applies any pending surface changes in a thread-safe manner.
void GsVideoRenderer::applyPendingSurfaceLocked()
{
    destroyImGuiLocked();
    m_video_shader_renderer.release();
    if (m_texture != 0)
    {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }
    if (m_offscreen_fbo != 0)
    {
        glDeleteFramebuffers(1, &m_offscreen_fbo);
        m_offscreen_fbo = 0;
    }
    if (m_offscreen_tex != 0)
    {
        glDeleteTextures(1, &m_offscreen_tex);
        m_offscreen_tex = 0;
    }
    m_offscreen_w = 0;
    m_offscreen_h = 0;
    if (gsPublishRendererTexture != nullptr)
    {
        gsPublishRendererTexture(0, 0, 0);
    }
    m_surface_dirty = false;
    m_has_uploaded_frame = false;
    m_uploaded_pixel_format = PixelFormat::RGB24;
    m_uploaded_width = 0;
    m_uploaded_height = 0;
    m_surface_width = 0;
    m_surface_height = 0;

    if (m_surface_backend.applyPendingSurface(m_vsync))
    {
        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        m_surface_width = m_surface_backend.surfaceWidth();
        m_surface_height = m_surface_backend.surfaceHeight();
        // On Quest the SurfaceView's pixel size is irrelevant — we render into
        // an offscreen FBO sampled into the OpenXR head-locked quad, which is
        // a fixed 1280x720 swapchain. Forcing the renderer's working size to
        // match avoids any scaling and keeps text/overlays pixel-perfect.
        if (gsPublishRendererTexture != nullptr)
        {
            m_surface_width = 1280;
            m_surface_height = 720;
        }

        glClearColor(0.04f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        m_surface_backend.swapBuffers();
        if (!initImGuiLocked())
        {
            return;
        }
        s_flightOSD.invalidateFontAtlas();

        if (!m_locked_frame.pixels.empty() || m_locked_frame.external_pixels != nullptr)
        {
            uploadFrameLocked();
        }
        else
        {
            drawFrameLocked();
        }
    }
}

//===================================================================================
//===================================================================================
// Releases frame reference and external resources.
void GsVideoRenderer::releaseFrameRefLocked(PendingFrame& frame)
{
    frame.external_frame_ref.reset();
    frame.external_pixels = nullptr;
    frame.external_size = 0;
    frame.pixels.clear();
}

//===================================================================================
//===================================================================================
// Initializes the ImGui context for overlay rendering.
bool GsVideoRenderer::initImGuiLocked()
{
    if (m_imgui_context != nullptr)
    {
        destroyImGuiLocked();
    }

    IMGUI_CHECKVERSION();
    ImGuiContext* context = ImGui::CreateContext();
    if (context == nullptr)
    {
        return false;
    }

    m_imgui_context = context;
    ImGui::SetCurrentContext(context);
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
    io.KeyRepeatDelay = 1.0f;
    io.KeyRepeatRate = 0.1f;
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    io.DisplaySize = ImVec2(static_cast<float>(m_surface_width), static_cast<float>(m_surface_height));

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowBorderSize = 0.0f;
    style.ScrollbarSize = static_cast<float>(m_surface_width) / 80.0f;
    style.ItemInnerSpacing = ImVec2(style.ItemSpacing.x / 2.0f, style.ItemSpacing.y / 2.0f);
    io.FontGlobalScale = kLinuxMenuFontGlobalScale;

    // Android surface reattach can re-enter renderer setup while Dear ImGui's
    // GL backend still thinks it is live. Reset the backend before re-init so
    // the attach path does not trip the backend's double-init assertion.
    if (m_imgui_backend_initialized || io.BackendRendererUserData != nullptr)
    {
        ImGui_ImplOpenGL3_Shutdown();
        m_imgui_backend_initialized = false;
        io.BackendRendererUserData = nullptr;
        io.BackendRendererName = nullptr;
    }

    if (!ImGui_ImplOpenGL3_Init("#version 100"))
    {
        destroyImGuiLocked();
        return false;
    }

    m_imgui_backend_initialized = true;
    return true;
}

//===================================================================================
//===================================================================================
// Destroys the ImGui context and cleans up resources.
void GsVideoRenderer::destroyImGuiLocked()
{
    if (m_imgui_context == nullptr)
    {
        if (m_imgui_backend_initialized)
        {
            ImGui_ImplOpenGL3_Shutdown();
            m_imgui_backend_initialized = false;
        }
        return;
    }

    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_context));
    if (m_imgui_backend_initialized)
    {
        ImGui_ImplOpenGL3_Shutdown();
        m_imgui_backend_initialized = false;
    }
    ImGui::DestroyContext(static_cast<ImGuiContext*>(m_imgui_context));
    m_imgui_context = nullptr;
}

//===================================================================================
//===================================================================================
// Uploads the current frame to GPU texture memory.
void GsVideoRenderer::uploadFrameLocked()
{
    if (m_locked_frame.pixels.empty() && m_locked_frame.external_pixels == nullptr)
    {
        return;
    }

    const auto upload_begin = Clock::now();
    ensureTextureLocked();
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const void* upload_pixels = m_locked_frame.external_pixels != nullptr
        ? static_cast<const void*>(m_locked_frame.external_pixels)
        : static_cast<const void*>(m_locked_frame.pixels.data());
    const GLenum upload_type = m_locked_frame.pixel_format == PixelFormat::RGB565
        ? GL_UNSIGNED_SHORT_5_6_5
        : GL_UNSIGNED_BYTE;
    const GLenum upload_format = m_locked_frame.pixel_format == PixelFormat::RGBA8888
        ? GL_RGBA
        : GL_RGB;
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        m_frame_width,
        m_frame_height,
        upload_format,
        upload_type,
        upload_pixels);
    logGlError("glTexSubImage2D");
    const uint32_t duration_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - upload_begin).count());
    m_upload_count.fetch_add(1);
    m_upload_total_ms.fetch_add(duration_ms);
    uint32_t current_min = m_upload_min_ms.load();
    while (duration_ms < current_min && !m_upload_min_ms.compare_exchange_weak(current_min, duration_ms))
    {
    }
    uint32_t current_max = m_upload_max_ms.load();
    while (duration_ms > current_max && !m_upload_max_ms.compare_exchange_weak(current_max, duration_ms))
    {
    }
    m_has_uploaded_frame = true;
    drawFrameLocked();
}

//===================================================================================
//===================================================================================
// Ensures that a suitable OpenGL texture is available for frame rendering.
// Allocates (or re-allocates on size change) the offscreen FBO+texture used on
// Quest. The texture id is published to the bridge so the OpenXR thread can
// sample it directly into the head-locked quad swapchain.
void GsVideoRenderer::ensureOffscreenTargetLocked()
{
    if (m_surface_width <= 0 || m_surface_height <= 0)
    {
        return;
    }
    if (m_offscreen_fbo != 0 &&
        m_offscreen_w == m_surface_width &&
        m_offscreen_h == m_surface_height)
    {
        return;
    }

    if (m_offscreen_fbo != 0)
    {
        glDeleteFramebuffers(1, &m_offscreen_fbo);
        m_offscreen_fbo = 0;
    }
    if (m_offscreen_tex != 0)
    {
        glDeleteTextures(1, &m_offscreen_tex);
        m_offscreen_tex = 0;
    }

    glGenTextures(1, &m_offscreen_tex);
    glBindTexture(GL_TEXTURE_2D, m_offscreen_tex);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 m_surface_width,
                 m_surface_height,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &m_offscreen_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_offscreen_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_offscreen_tex, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        LOGE("Offscreen FBO incomplete: 0x{:04x}", static_cast<unsigned int>(status));
        glDeleteFramebuffers(1, &m_offscreen_fbo);
        glDeleteTextures(1, &m_offscreen_tex);
        m_offscreen_fbo = 0;
        m_offscreen_tex = 0;
        return;
    }

    m_offscreen_w = m_surface_width;
    m_offscreen_h = m_surface_height;

    if (gsPublishRendererTexture != nullptr)
    {
        gsPublishRendererTexture(m_offscreen_tex, m_offscreen_w, m_offscreen_h);
    }
    LOGI("renderer: offscreen FBO ready {}x{} tex={}", m_offscreen_w, m_offscreen_h, m_offscreen_tex);
}

//===================================================================================
//===================================================================================
void GsVideoRenderer::ensureTextureLocked()
{
    if (m_texture == 0 || m_frame_width <= 0 || m_frame_height <= 0)
    {
        return;
    }
    if (m_uploaded_width == m_frame_width &&
        m_uploaded_height == m_frame_height &&
        m_uploaded_pixel_format == m_locked_frame.pixel_format)
    {
        return;
    }

    glBindTexture(GL_TEXTURE_2D, m_texture);
    const GLenum texture_type = m_locked_frame.pixel_format == PixelFormat::RGB565
        ? GL_UNSIGNED_SHORT_5_6_5
        : GL_UNSIGNED_BYTE;
    const GLenum texture_internal_format = m_locked_frame.pixel_format == PixelFormat::RGB565
        ? GL_RGB565
        : (m_locked_frame.pixel_format == PixelFormat::RGBA8888 ? GL_RGBA : GL_RGB);
    const GLenum texture_format = m_locked_frame.pixel_format == PixelFormat::RGBA8888
        ? GL_RGBA
        : GL_RGB;
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        texture_internal_format,
        m_frame_width,
        m_frame_height,
        0,
        texture_format,
        texture_type,
        nullptr);
    logGlError("glTexImage2D");
    m_uploaded_width = m_frame_width;
    m_uploaded_height = m_frame_height;
    m_uploaded_pixel_format = m_locked_frame.pixel_format;
}

//===================================================================================
//===================================================================================
// Draws the OSD menu overlay.
void GsVideoRenderer::drawMenuLocked()
{
    if (m_menu_controller == nullptr || m_menu_config == nullptr)
    {
        return;
    }

    if (m_menu_mutex != nullptr)
    {
        std::lock_guard<std::mutex> menu_lock(*m_menu_mutex);
        if (m_open_menu_requested)
        {
            m_menu_controller->open();
        }
        if (m_open_playback_menu_requested)
        {
            m_menu_controller->openPlaybackMenu();
        }
        if (m_close_menu_requested)
        {
            m_menu_controller->close();
        }
        m_open_menu_requested = false;
        m_open_playback_menu_requested = false;
        m_close_menu_requested = false;
        const bool was_menu_visible = m_menu_controller->isVisible();
        m_menu_visible = was_menu_visible;
        handleImGuiKeysLocked();
        m_menu_visible = m_menu_controller->isVisible();
        if (!was_menu_visible && m_menu_visible)
        {
            // Opening from a queued ImGui key must redraw on the next frame so the
            // same key press cannot immediately activate the first visible item.
            m_redraw_after_current_frame = true;
            return;
        }
    }

    if (!m_menu_visible)
    {
        if ((s_playbackManager != nullptr && s_playbackManager->status().active) ||
            gs::calibration::isActive())
        {
            drawRuntimeTouchControlsLocked(true, true);
        }
        return;
    }

    drawMenuImGuiLocked();
}

//===================================================================================
//===================================================================================
// Handles renderer-owned ImGui key actions after platform input has reached ImGui.
void GsVideoRenderer::handleImGuiKeysLocked()
{
    if (m_menu_controller == nullptr || m_menu_config == nullptr)
    {
        return;
    }

    const OSDMenuId active_menu_id = m_menu_controller->currentMenuId();
    const bool playback_active = s_playbackManager != nullptr && s_playbackManager->status().active;
    const bool in_playback_menu = m_menu_visible &&
        (active_menu_id == OSDMenuId::Playback || active_menu_id == OSDMenuId::PlaybackDelete);

    if (!in_playback_menu && !playback_active &&
        gs::runtime::handleRecordingKeysFromImGui(*m_menu_config, "imgui_g"))
    {
        return;
    }

    if (!m_menu_visible &&
        gs::runtime::handlePlaybackKeysFromImGui(s_playbackManager,
                                                 [this]()
                                                 {
                                                     m_menu_controller->openPlaybackMenu();
                                                 },
                                                 [this]()
                                                 {
                                                     m_menu_controller->openPlaybackDeleteMenuForActivePlayback();
                                                 }))
    {
        return;
    }

    if (!m_menu_visible)
    {
        if (gs::runtime::handleResolutionCycleKeysFromImGui(*m_menu_config))
        {
            return;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Menu, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
            ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
        {
            m_menu_controller->open();
        }
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Menu, false))
    {
        m_menu_controller->close();
    }
}

//===================================================================================
//===================================================================================
// Draws the menu using ImGui rendering system.
void GsVideoRenderer::drawMenuImGuiLocked()
{
    if (m_imgui_context == nullptr || m_menu_controller == nullptr || m_menu_config == nullptr)
    {
        return;
    }

    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_context));
    RuntimeMenuUiState menu_ui = drawRuntimeTouchControlsLocked(m_menu_visible, false);
    drawRuntimeMenuOverlay(menu_ui);
    if (m_menu_mutex != nullptr)
    {
        std::lock_guard<std::mutex> menu_lock(*m_menu_mutex);
        m_menu_controller->draw(*m_menu_config);
        m_menu_visible = m_menu_controller->isVisible();
    }
    drawRuntimeMenuTouchNav(menu_ui);
}

//===================================================================================
//===================================================================================
// Draws Android-style touch DPAD and recording buttons independently of menu content.
RuntimeMenuUiState GsVideoRenderer::drawRuntimeTouchControlsLocked(bool visible, bool draw_controls)
{
    RuntimeMenuUiState menu_ui = {};
    menu_ui.visible = visible;
    menu_ui.vr_mode = m_vr_mode;
    menu_ui.touch_nav_enabled = true;
    menu_ui.surface_width = static_cast<float>(m_surface_width);
    menu_ui.surface_height = static_cast<float>(m_surface_height);
    menu_ui.gs_recording = (s_recordingsStorage != nullptr) && s_recordingsStorage->isRecording();
    menu_ui.air_recording = m_overlay_input.air_record;

    const float layout_width = m_vr_mode ? (menu_ui.surface_width * 0.5f) : menu_ui.surface_width;
    const gs::render::NavPadLayout nav =
        gs::render::buildTouchNavPadLayout(static_cast<int>(layout_width), static_cast<int>(menu_ui.surface_height));
    m_nav_up_bounds = {nav.center_x, nav.up_y, nav.size, nav.size};
    m_nav_down_bounds = {nav.center_x, nav.down_y, nav.size, nav.size};
    m_nav_left_bounds = {nav.left_x, nav.mid_y, nav.size, nav.size};
    m_nav_right_bounds = {nav.right_x, nav.mid_y, nav.size, nav.size};
    m_nav_center_bounds = {nav.center_x, nav.mid_y, nav.size, nav.size};
    m_nav_air_rec_bounds = {nav.margin, nav.mid_y, nav.size, nav.size};
    m_nav_gs_rec_bounds = {nav.margin, nav.down_y, nav.size, nav.size};
    if (draw_controls)
    {
        drawRuntimeMenuTouchNav(menu_ui);
    }

    return menu_ui;
}

//===================================================================================
//===================================================================================
// Renders one canonical left-eye ImGui frame into both VR eye regions.
void GsVideoRenderer::renderDrawDataWithVrReplication(ImDrawData* draw_data, float surface_width, float vr_separation)
{
    if (draw_data == nullptr || surface_width <= 0.0f)
    {
        ImGui_ImplOpenGL3_RenderDrawData(draw_data);
        return;
    }

    const float half_w = surface_width * 0.5f;
    const float full_w = surface_width;
    const float offset = vr_separation * half_w;

    struct SavedList
    {
        std::vector<float> x_positions;
        std::vector<ImVec4> clip_rects;
    };

    std::vector<SavedList> saved(draw_data->CmdListsCount);
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        saved[n].x_positions.resize(cmd_list->VtxBuffer.Size);
        for (int i = 0; i < cmd_list->VtxBuffer.Size; i++)
        {
            saved[n].x_positions[i] = cmd_list->VtxBuffer[i].pos.x;
        }

        saved[n].clip_rects.resize(cmd_list->CmdBuffer.Size);
        for (int i = 0; i < cmd_list->CmdBuffer.Size; i++)
        {
            saved[n].clip_rects[i] = cmd_list->CmdBuffer[i].ClipRect;
        }
    }

    const auto apply_eye = [&](float dx, float x_min, float x_max)
    {
        for (int n = 0; n < draw_data->CmdListsCount; n++)
        {
            ImDrawList* cmd_list = draw_data->CmdLists[n];
            for (int i = 0; i < cmd_list->VtxBuffer.Size; i++)
            {
                cmd_list->VtxBuffer[i].pos.x = saved[n].x_positions[i] + dx;
            }

            for (int i = 0; i < cmd_list->CmdBuffer.Size; i++)
            {
                const ImVec4& source_clip = saved[n].clip_rects[i];
                ImVec4& clip = cmd_list->CmdBuffer[i].ClipRect;
                clip.y = source_clip.y;
                clip.w = source_clip.w;

                // VR authors one canonical left-eye frame and replays it into both
                // halves. Commands already outside the canonical eye are clipped so
                // stale full-screen windows cannot leak across.
                if (source_clip.x >= half_w)
                {
                    clip.x = x_min;
                    clip.z = x_min;
                }
                else
                {
                    clip.x = std::clamp(source_clip.x + dx, x_min, x_max);
                    clip.z = std::clamp(source_clip.z + dx, x_min, x_max);
                }
            }
        }
    };

    apply_eye(+offset, 0.0f, half_w);
    ImGui_ImplOpenGL3_RenderDrawData(draw_data);
    apply_eye(half_w - offset, half_w, full_w);
    ImGui_ImplOpenGL3_RenderDrawData(draw_data);
}

//===================================================================================
//===================================================================================
// Draws the current video frame directly with the shared video shader.
void GsVideoRenderer::drawVideoShaderLocked(float quad_x,
                                            float clip_x,
                                            float viewport_width,
                                            float viewport_height)
{
    if (!m_has_uploaded_frame || m_texture == 0 || viewport_width <= 0.0f || viewport_height <= 0.0f)
    {
        return;
    }

    gs::render::VideoQuad quad =
        gs::render::buildVideoQuad(quad_x,
                                   0.0f,
                                   viewport_width,
                                   viewport_height,
                                   m_frame_width,
                                   m_frame_height,
                                   m_screen_mode);
    std::swap(quad.v0, quad.v1);

    if (m_zoom != 1.0f)
    {
        const float cx = quad.x + quad.width * 0.5f;
        const float cy = quad.y + quad.height * 0.5f;
        quad.width *= m_zoom;
        quad.height *= m_zoom;
        quad.x = cx - quad.width * 0.5f;
        quad.y = cy - quad.height * 0.5f;
    }

    const gs::render::LensCorrectionParams lens_params =
        gs::render::buildLensCorrectionParams(s_lensCorrectionState);
    gs::stabilization::StabilizationTransform stabilization_transform =
        gs::stabilization::getRenderTrajectoryTransform();
    m_video_shader_renderer.draw(m_texture,
                                 quad,
                                 clip_x,
                                 0.0f,
                                 viewport_width,
                                 viewport_height,
                                 static_cast<float>(m_surface_width),
                                 static_cast<float>(m_surface_height),
                                 m_frame_width,
                                 m_frame_height,
                                 lens_params,
                                 stabilization_transform,
                                 m_locked_frame.postprocessing_params);
}

//===================================================================================
//===================================================================================
// Draws the statistics and flight overlay elements.
void GsVideoRenderer::drawOverlayLocked()
{
    if (m_imgui_context == nullptr)
    {
        return;
    }

    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_context));
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(m_surface_width), static_cast<float>(m_surface_height));
    io.FontGlobalScale = kLinuxMenuFontGlobalScale * gs::menu::imgui::calcOsdScale(static_cast<float>(m_surface_height));
    io.DeltaTime = 1.0f / 60.0f;
    gs::mcp::drainInjectedKeysToImGui();
    if (gsTryConsumeXrImGuiKey != nullptr)
    {
        int xr_key = 0;
        while ((xr_key = gsTryConsumeXrImGuiKey()) != 0)
        {
            const ImGuiKey k = static_cast<ImGuiKey>(xr_key);
            io.AddKeyEvent(k, true);
            io.AddKeyEvent(k, false);
        }
    }
    for (const ImGuiKey key : m_pending_key_presses)
    {
        io.AddKeyEvent(key, true);
        io.AddKeyEvent(key, false);
    }
    m_pending_key_presses.clear();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    const float overlay_width = m_vr_mode ? (static_cast<float>(m_surface_width) * 0.5f) : static_cast<float>(m_surface_width);
    // Video is drawn before ImGui so lens correction can run in the video shader.
    // Overlay draw data is still authored once and replayed into both eyes in VR.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(overlay_width, static_cast<float>(m_surface_height)), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 0.0f));
    const bool overlay_window_open = ImGui::Begin("FULLSCREEN_OVERLAY",
                                                  nullptr,
                                                  ImGuiWindowFlags_NoDecoration |
                                                      ImGuiWindowFlags_NoResize |
                                                      ImGuiWindowFlags_NoBackground |
                                                      ImGuiWindowFlags_NoInputs |
                                                      ImGuiWindowFlags_NoNav |
                                                      ImGuiWindowFlags_NoFocusOnAppearing);

    if (overlay_window_open)
    {
        s_flightOSD.draw(static_cast<int>(overlay_width), m_surface_height, m_frame_width, m_frame_height, m_screen_mode);
        gs::imgui::drawTopOverlayStatus(m_overlay_input, overlay_width);
        if (m_frame_ui_state.overlay_stats_visible)
        {
            gs::stats::drawFullscreenStatsPanel(m_overlay_stats_snapshot);
        }
        drawPlaybackProgressOverlay(overlay_width, static_cast<float>(m_surface_height));
        drawMenuLocked();
        gs::stabilization::drawStabilizationRoiOverlay(0.0f,
                                                       overlay_width,
                                                       overlay_width,
                                                       static_cast<float>(m_surface_height),
                                                       m_frame_width,
                                                       m_frame_height,
                                                       m_screen_mode,
                                                       m_zoom);
        gs::calibration::drawCalibrationOverlay(overlay_width, static_cast<float>(m_surface_height));
    }

    if (m_imgui_context != nullptr)
    {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_context));
        ImGui::End();
        ImGui::PopStyleVar(5);
        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        if (m_vr_mode && draw_data != nullptr)
        {
            renderDrawDataWithVrReplication(draw_data,
                                            static_cast<float>(m_surface_width),
                                            m_vr_separation);
        }
        else
        {
            ImGui_ImplOpenGL3_RenderDrawData(draw_data);
        }
    }
}

//===================================================================================
//===================================================================================
// Draws the current video frame and overlays to the screen.
void GsVideoRenderer::drawFrameLocked()
{
    if (m_surface_width <= 0 || m_surface_height <= 0)
    {
        return;
    }

    // On Quest the SurfaceView is hidden behind the OpenXR composition layer;
    // render into an offscreen FBO whose texture is shared with the OpenXR
    // thread instead of into the SurfaceView's default framebuffer.
    const bool offscreen = (gsPublishRendererTexture != nullptr);
    if (offscreen)
    {
        ensureOffscreenTargetLocked();
        if (m_offscreen_fbo != 0)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, m_offscreen_fbo);
        }
    }

    glViewport(0, 0, m_surface_width, m_surface_height);
    glClearColor(0.04f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (m_vr_mode)
    {
        const float half_w = static_cast<float>(m_surface_width) * 0.5f;
        const float offset = m_vr_separation * half_w;
        drawVideoShaderLocked(+offset, 0.0f, half_w, static_cast<float>(m_surface_height));
        drawVideoShaderLocked(half_w - offset, half_w, half_w, static_cast<float>(m_surface_height));
    }
    else
    {
        drawVideoShaderLocked(0.0f,
                              0.0f,
                              static_cast<float>(m_surface_width),
                              static_cast<float>(m_surface_height));
    }
    drawOverlayLocked();

    if (offscreen)
    {
        // Restore default framebuffer binding and let GL flush the queue so the
        // OpenXR thread can sample the fresh contents on its next iteration.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    const auto swap_begin = Clock::now();
    if (offscreen)
    {
        // Quest immersive: nothing to swap (we rendered into the shared texture
        // directly). Just drain the GL queue so the OpenXR thread sees the
        // updated pixels on its next sample.
        glFlush();
    }
    else
    {
        m_surface_backend.swapBuffers();
    }
    const uint32_t swap_duration_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - swap_begin).count());
    m_gpu_wait_last_ms.store(swap_duration_ms);
    m_swap_count.fetch_add(1);
    m_swap_total_ms.fetch_add(swap_duration_ms);
    uint32_t swap_min = m_swap_min_ms.load();
    while (swap_duration_ms < swap_min && !m_swap_min_ms.compare_exchange_weak(swap_min, swap_duration_ms))
    {
    }
    uint32_t swap_max = m_swap_max_ms.load();
    while (swap_duration_ms > swap_max && !m_swap_max_ms.compare_exchange_weak(swap_max, swap_duration_ms))
    {
    }
}

