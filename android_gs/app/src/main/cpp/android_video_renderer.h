#pragma once

#include "android_jni_shared.h"

#include <android/native_window.h>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "core/stats_panel_shared.h"

enum class AndroidPerfMode
{
    Baseline,
    SkipDecodeSynthetic,
    SkipUpload,
};

inline constexpr AndroidPerfMode kAndroidPerfMode = AndroidPerfMode::Baseline;

class AndroidVideoRenderer
{
public:
    enum class PixelFormat
    {
        RGB24,
        RGB565,
    };

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

    AndroidVideoRenderer();
    ~AndroidVideoRenderer();

    void setSurface(ANativeWindow* window);
    void clearSurface();
    void submitFrame(const uint8_t* pixels,
                     size_t size,
                     int width,
                     int height,
                     int stride,
                     uint32_t frame_id,
                     PixelFormat pixel_format = PixelFormat::RGB24);
    void submitFrame(jobject direct_buffer_global_ref,
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
    void setScreenMode(int screen_mode);
    void setOverlayState(const std::vector<OverlayChip>& chips,
                         const OverlayMenuState& menu_state,
                         const OverlayStatsState& stats_state,
                         const OverlayPacketDebugState& packet_debug_state);
    MenuAction dispatchTap(float x, float y);
    RendererStats consumeStats();

private:
    struct PendingFrame
    {
        std::vector<uint8_t> pixels;
        jobject direct_buffer_global_ref = nullptr;
        const uint8_t* direct_pixels = nullptr;
        size_t direct_size = 0;
        int width = 0;
        int height = 0;
        int stride = 0;
        uint32_t frame_id = 0;
        PixelFormat pixel_format = PixelFormat::RGB24;
    };

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
    void releaseFrameRefLocked(PendingFrame& frame);

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    bool m_exit = false;

    ANativeWindow* m_window = nullptr;
    ANativeWindow* m_pending_window = nullptr;
    bool m_surface_dirty = false;

    PendingFrame m_pending_frame;
    PendingFrame m_locked_frame;
    bool m_has_pending_frame = false;
    int m_frame_width = 0;
    int m_frame_height = 0;
    int m_frame_stride = 0;
    std::atomic<bool> m_frame_dirty = false;

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
    PixelFormat m_uploaded_pixel_format = PixelFormat::RGB24;
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
