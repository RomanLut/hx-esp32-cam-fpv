#pragma once

#include "flight_osd.h"
#include "android_surface_backend.h"
#include "gs_runtime_frame_ui.h"
#include "gs_top_overlay_shared.h"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "packets.h"
#include "core/stats_panel_shared.h"

namespace gs::menu
{
class OSDMenuController;
}

//===================================================================================
//===================================================================================
// Main class for rendering video frames with overlays and menus in Ground Station.
class GsVideoRenderer
{
public:
//===================================================================================
//===================================================================================
// Enumeration of supported pixel formats for video frames.
    enum class PixelFormat
    {
        RGB24,
        RGB565,
    };

//===================================================================================
//===================================================================================
// Enumeration of possible menu action types.
    enum class MenuActionKind
    {
        None,
        Outside,
        SelectItem,
        Up,
        Down,
        Back,
        Activate,
    };

//===================================================================================
//===================================================================================
// Structure representing a menu action with its kind and associated item index.
    struct MenuAction
    {
        MenuActionKind kind = MenuActionKind::None;
        int item_index = -1;
    };

//===================================================================================
//===================================================================================
// Structure representing a rectangular area with position and dimensions.
    struct Rect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
    };

//===================================================================================
//===================================================================================
// Structure holding the current state of the overlay menu display.
    struct OverlayMenuState
    {
        bool visible = false;
        std::string title;
        std::vector<std::string> items;
        std::vector<std::string> statuses;
        std::vector<std::string> status_lines;
        int selected_index = 0;
        std::string footer;
    };

//===================================================================================
//===================================================================================
// Structure containing performance statistics for the video renderer.
    struct RendererStats
    {
        uint32_t upload_count = 0;
        uint32_t upload_total_ms = 0;
        uint32_t upload_min_ms = 99;
        uint32_t upload_max_ms = 0;
        uint32_t discarded_pending_count = 0;
        uint32_t swap_count = 0;
        uint32_t swap_total_ms = 0;
        uint32_t swap_min_ms = 99;
        uint32_t swap_max_ms = 0;
    };

//===================================================================================
//===================================================================================
// Constructor for GsVideoRenderer. Initializes the video rendering system.
    GsVideoRenderer();
//===================================================================================
//===================================================================================
// Destructor for GsVideoRenderer. Cleans up rendering resources.
    ~GsVideoRenderer();

    void setSurface(void* surface_handle);
    void clearSurface();
    void submitFrame(const uint8_t* pixels,
                     size_t size,
                     int width,
                     int height,
                     int stride,
                     uint32_t frame_id,
                     PixelFormat pixel_format = PixelFormat::RGB24);
    void submitFrame(std::shared_ptr<void> external_frame_ref,
                     const uint8_t* pixels,
                     size_t size,
                     int width,
                     int height,
                     int stride,
                     uint32_t frame_id,
                     PixelFormat pixel_format = PixelFormat::RGB24);
    void submitFrame(std::vector<uint8_t>&& pixels,
                     int width,
                     int height,
                     int stride,
                     uint32_t frame_id,
                     PixelFormat pixel_format = PixelFormat::RGB24);
    void setVsync(bool enabled);
    void setVrMode(bool enabled);
    void setScreenMode(int screen_mode);
    void updateFlightOsd(const uint8_t* data, uint16_t size);
    void clearFlightOsd();
    void setFlightOsdFont(const std::string& font_name);
    void setOverlayInput(const gs::imgui::TopOverlayData& overlay_input);
    void setFrameUiState(const RuntimeFrameUiState& frame_ui_state);
    void setOverlayStatsSnapshot(const gs::stats::FullscreenStatsSnapshot& snapshot);
    void setMenuBinding(gs::menu::OSDMenuController* menu_controller,
                        Ground2Air_Config_Packet* config_packet,
                        std::mutex* menu_mutex);
    void invalidateDisplayedFrame();
    void setMenuFooter(const std::string& footer);
    bool isMenuVisible();
    void queueMenuOpen();
    void queueMenuClose();
    void queueMouseTap(float x, float y);
    void queueKeyPress(ImGuiKey key);
    MenuAction dispatchTap(float x, float y);
    RendererStats consumeStats();

private:
//===================================================================================
//===================================================================================
// Structure holding data for a frame pending rendering.
    struct PendingFrame
    {
        std::vector<uint8_t> pixels;
        std::shared_ptr<void> external_frame_ref;
        const uint8_t* external_pixels = nullptr;
        size_t external_size = 0;
        int width = 0;
        int height = 0;
        int stride = 0;
        uint32_t frame_id = 0;
        PixelFormat pixel_format = PixelFormat::RGB24;
    };

    void run();
    void applyPendingSurfaceLocked();
    bool initImGuiLocked();
    void destroyImGuiLocked();
    void drawFrameLocked();
    void uploadFrameLocked();
    void ensureTextureLocked();
    void drawOverlayLocked();
    void drawMenuLocked();
    void drawMenuImGuiLocked();
    void drawRectLocked(float x, float y, float width, float height, const std::array<float, 4>& color);
    void drawTexturedQuadLocked(float x,
                                float y,
                                float width,
                                float height,
                                float u0,
                                float v0,
                                float u1,
                                float v1,
                                unsigned int texture,
                                const std::array<float, 4>& color);
    void releaseFrameRefLocked(PendingFrame& frame);

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    bool m_exit = false;

    bool m_surface_dirty = false;
    GsGlSurfaceBackend m_surface_backend;

    PendingFrame m_pending_frame;
    PendingFrame m_locked_frame;
    bool m_has_pending_frame = false;
    int m_frame_width = 0;
    int m_frame_height = 0;
    int m_frame_stride = 0;
    std::atomic<bool> m_frame_dirty = false;

    int m_screen_mode = 1;
    bool m_vsync = true;
    bool m_vr_mode = false;
    bool m_mode_dirty = true;
    bool m_overlay_dirty = true;

    unsigned int m_program = 0;
    unsigned int m_texture = 0;
    unsigned int m_white_texture = 0;
    void* m_imgui_context = nullptr;
    int m_surface_width = 0;
    int m_surface_height = 0;
    int m_uploaded_width = 0;
    int m_uploaded_height = 0;
    PixelFormat m_uploaded_pixel_format = PixelFormat::RGB24;
    bool m_has_uploaded_frame = false;
    std::vector<uint8_t> m_pending_font_png;
    gs::imgui::TopOverlayData m_overlay_input;
    RuntimeFrameUiState m_frame_ui_state;
    gs::stats::FullscreenStatsSnapshot m_overlay_stats_snapshot;
    OverlayMenuState m_overlay_menu;
    gs::menu::OSDMenuController* m_menu_controller = nullptr;
    Ground2Air_Config_Packet* m_menu_config = nullptr;
    std::mutex* m_menu_mutex = nullptr;
    std::string m_menu_footer;
    Rect m_menu_bounds;
    std::vector<Rect> m_menu_item_bounds;
    Rect m_nav_up_bounds;
    Rect m_nav_down_bounds;
    Rect m_nav_left_bounds;
    Rect m_nav_right_bounds;
    bool m_menu_visible = false;
    bool m_open_menu_requested = false;
    bool m_close_menu_requested = false;
    bool m_touch_pending = false;
    uint64_t m_touch_sequence = 0;
    uint64_t m_touch_processed_sequence = 0;
    float m_touch_x = 0.0f;
    float m_touch_y = 0.0f;
    std::vector<ImGuiKey> m_pending_key_presses;
    MenuAction m_touch_action;

    std::atomic<uint32_t> m_upload_count = 0;
    std::atomic<uint32_t> m_upload_total_ms = 0;
    std::atomic<uint32_t> m_upload_min_ms = 99;
    std::atomic<uint32_t> m_upload_max_ms = 0;
    std::atomic<uint32_t> m_discarded_pending_count = 0;
    std::atomic<uint32_t> m_swap_count = 0;
    std::atomic<uint32_t> m_swap_total_ms = 0;
    std::atomic<uint32_t> m_swap_min_ms = 99;
    std::atomic<uint32_t> m_swap_max_ms = 0;
};
