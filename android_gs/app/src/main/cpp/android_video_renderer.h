#pragma once

#include <android/native_window.h>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/stats_panel_shared.h"

class AndroidVideoRenderer
{
public:
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

    struct MenuAction
    {
        MenuActionKind kind = MenuActionKind::None;
        int item_index = -1;
    };

    struct Rect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
    };

    struct OverlayChip
    {
        std::string text;
        bool alert = false;
    };

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

    struct OverlayStatsState
    {
        bool visible = false;
        gs::stats::FullscreenStatsSnapshot snapshot = {};
    };

    struct OverlayPacketDebugState
    {
        bool visible = false;
        std::vector<std::string> lines;
    };

    AndroidVideoRenderer();
    ~AndroidVideoRenderer();

    void setSurface(ANativeWindow* window);
    void clearSurface();
    void submitFrame(const uint8_t* rgba, size_t size, int width, int height, int stride);
    void setScreenMode(int screen_mode);
    void setOverlayState(const std::vector<OverlayChip>& chips,
                         const OverlayMenuState& menu_state,
                         const OverlayStatsState& stats_state,
                         const OverlayPacketDebugState& packet_debug_state);
    MenuAction dispatchTap(float x, float y);

private:
    void run();
    void applyPendingSurfaceLocked();
    bool initEglLocked();
    void destroyEglLocked();
    bool initImGuiLocked();
    void destroyImGuiLocked();
    void drawFrameLocked();
    void uploadFrameLocked();
    void ensureTextureLocked();
    void drawOverlayLocked();
    void drawHudLocked();
    void drawHudImGuiLocked();
    void drawStatsLocked();
    void drawPacketDebugLocked();
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

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    bool m_exit = false;

    ANativeWindow* m_window = nullptr;
    ANativeWindow* m_pending_window = nullptr;
    bool m_surface_dirty = false;

    std::vector<uint8_t> m_pending_frame;
    int m_frame_width = 0;
    int m_frame_height = 0;
    int m_frame_stride = 0;
    bool m_frame_dirty = false;

    int m_screen_mode = 1;
    bool m_mode_dirty = true;
    bool m_overlay_dirty = true;

    void* m_egl_display = nullptr;
    void* m_egl_surface = nullptr;
    void* m_egl_context = nullptr;
    unsigned int m_program = 0;
    unsigned int m_texture = 0;
    unsigned int m_white_texture = 0;
    void* m_imgui_context = nullptr;
    int m_surface_width = 0;
    int m_surface_height = 0;
    int m_uploaded_width = 0;
    int m_uploaded_height = 0;
    bool m_has_uploaded_frame = false;
    std::vector<uint8_t> m_pending_font_png;
    std::vector<OverlayChip> m_overlay_chips;
    OverlayMenuState m_overlay_menu;
    OverlayStatsState m_overlay_stats;
    OverlayPacketDebugState m_overlay_packet_debug;
    Rect m_menu_bounds;
    std::vector<Rect> m_menu_item_bounds;
    Rect m_nav_up_bounds;
    Rect m_nav_down_bounds;
    Rect m_nav_left_bounds;
    Rect m_nav_right_bounds;
    bool m_touch_pending = false;
    uint64_t m_touch_sequence = 0;
    uint64_t m_touch_processed_sequence = 0;
    float m_touch_x = 0.0f;
    float m_touch_y = 0.0f;
    MenuAction m_touch_action;
};
