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

class AndroidVideoRenderer
{
public:
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

    AndroidVideoRenderer();
    ~AndroidVideoRenderer();

    void setSurface(ANativeWindow* window);
    void clearSurface();
    void submitFrame(const uint8_t* rgba, size_t size, int width, int height, int stride);
    void setScreenMode(int screen_mode);
    void setFontAtlasPng(const uint8_t* png_data, size_t size);
    void setMenuFontTtf(const uint8_t* ttf_data, size_t size);
    void setOverlayState(const std::vector<OverlayChip>& chips, const OverlayMenuState& menu_state);

private:
    struct GlyphTexture
    {
        struct Glyph
        {
            int x0 = 0;
            int y0 = 0;
            int x1 = 0;
            int y1 = 0;
            float xoff = 0.0f;
            float yoff = 0.0f;
            float xadvance = 0.0f;
        };

        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        int first_char = 32;
        int glyph_count = 0;
        float baked_pixel_height = 32.0f;
        float min_yoff = 0.0f;
        float max_ybottom = 0.0f;
        std::vector<Glyph> glyphs;
    };

    void run();
    void applyPendingSurfaceLocked();
    bool initEglLocked();
    void destroyEglLocked();
    void drawFrameLocked();
    void uploadFrameLocked();
    void ensureTextureLocked();
    bool uploadFontTextureLocked();
    void drawOverlayLocked();
    void drawHudLocked();
    void drawMenuLocked();
    void drawRectLocked(float x, float y, float width, float height, const std::array<float, 4>& color);
    void drawTextLocked(float x, float y, float char_height, const std::string& text, const std::array<float, 4>& color);
    void drawTextLineLocked(float x,
                            float y,
                            float max_width,
                            float box_height,
                            float char_height,
                            const std::string& text,
                            const std::array<float, 4>& color);
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
    bool m_font_dirty = false;

    void* m_egl_display = nullptr;
    void* m_egl_surface = nullptr;
    void* m_egl_context = nullptr;
    unsigned int m_program = 0;
    unsigned int m_texture = 0;
    unsigned int m_white_texture = 0;
    unsigned int m_font_texture = 0;
    int m_surface_width = 0;
    int m_surface_height = 0;
    int m_uploaded_width = 0;
    int m_uploaded_height = 0;
    bool m_has_uploaded_frame = false;
    std::vector<uint8_t> m_pending_font_png;
    std::vector<uint8_t> m_pending_font_ttf;
    GlyphTexture m_font;
    std::vector<OverlayChip> m_overlay_chips;
    OverlayMenuState m_overlay_menu;
};
