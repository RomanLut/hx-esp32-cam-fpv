#include "android_video_renderer.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "imgui/imstb_truetype.h"
#include "utils/lodepng.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace
{

constexpr const char* kLogTag = "AndroidGSRenderer";
constexpr int kMenuFirstChar = 32;
constexpr int kMenuGlyphCount = 96;
constexpr float kMenuBakedPixelHeight = 13.0f;
constexpr float kLinuxMenuFontGlobalScale = 2.0f;
constexpr float kLinuxMenuRefWidth = 1280.0f;
constexpr float kLinuxMenuRefHeight = 720.0f;
constexpr float kLinuxMenuWindowWidth = 500.0f;
constexpr float kLinuxMenuWindowHeight = 600.0f;
constexpr float kLinuxMenuButtonWidth = 442.0f;
constexpr float kLinuxMenuButtonHeight = 35.0f;
constexpr float kLinuxMenuItemInset = 29.0f;
constexpr float kLinuxMenuCenterYOffset = 120.0f;
constexpr float kLinuxMenuGapLarge = 20.0f;
constexpr float kLinuxMenuGapSmall = 8.0f;
constexpr float kAndroidNavButtonSize = 72.0f;
constexpr float kAndroidNavGap = 10.0f;
constexpr float kAndroidNavMargin = 18.0f;
constexpr float kAndroidNavLabelScale = 0.75f;

float linuxMenuScaleForSurface(int surface_width, int surface_height)
{
    if (surface_width <= 0 || surface_height <= 0)
    {
        return 1.0f;
    }

    const float width_scale = static_cast<float>(surface_width) / kLinuxMenuRefWidth;
    const float height_scale = static_cast<float>(surface_height) / kLinuxMenuRefHeight;
    return std::min(width_scale, height_scale);
}

struct NavPadLayout
{
    float size = 0.0f;
    float gap = 0.0f;
    float margin = 0.0f;
    float left_x = 0.0f;
    float right_x = 0.0f;
    float center_x = 0.0f;
    float up_y = 0.0f;
    float mid_y = 0.0f;
    float down_y = 0.0f;
};

NavPadLayout buildNavPadLayout(int surface_width, int surface_height)
{
    NavPadLayout layout;
    const float ui_scale = std::min(static_cast<float>(surface_width) / 1280.0f,
                                    static_cast<float>(surface_height) / 720.0f);
    const float control_scale = std::max(0.85f, ui_scale);
    layout.size = kAndroidNavButtonSize * control_scale;
    layout.gap = kAndroidNavGap * control_scale;
    layout.margin = kAndroidNavMargin * control_scale;
    layout.right_x = static_cast<float>(surface_width) - layout.margin - layout.size;
    layout.left_x = layout.right_x - layout.size - layout.gap - layout.size;
    layout.center_x = layout.left_x + layout.size + layout.gap;
    layout.down_y = static_cast<float>(surface_height) - layout.margin - layout.size;
    layout.mid_y = layout.down_y - layout.gap - layout.size;
    layout.up_y = layout.mid_y - layout.gap - layout.size;
    return layout;
}

constexpr const char* kVertexShader = R"(
attribute vec2 aPosition;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

constexpr const char* kFragmentShader = R"(
precision mediump float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec4 uColor;
void main() {
    gl_FragColor = texture2D(uTexture, vTexCoord) * uColor;
}
)";

GLuint compileShader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
    {
        return shader;
    }

    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    std::vector<char> log(static_cast<size_t>(std::max(log_length, 1)));
    glGetShaderInfoLog(shader, log_length, nullptr, log.data());
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Shader compile failed: %s", log.data());
    glDeleteShader(shader);
    return 0;
}

GLuint createProgram()
{
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (vertex == 0 || fragment == 0)
    {
        if (vertex != 0)
        {
            glDeleteShader(vertex);
        }
        if (fragment != 0)
        {
            glDeleteShader(fragment);
        }
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "aPosition");
    glBindAttribLocation(program, 1, "aTexCoord");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE)
    {
        return program;
    }

    GLint log_length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
    std::vector<char> log(static_cast<size_t>(std::max(log_length, 1)));
    glGetProgramInfoLog(program, log_length, nullptr, log.data());
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Program link failed: %s", log.data());
    glDeleteProgram(program);
    return 0;
}

float toNdcX(float x, float surface_width)
{
    return (x / surface_width) * 2.0f - 1.0f;
}

float toNdcY(float y, float surface_height)
{
    return 1.0f - (y / surface_height) * 2.0f;
}

std::array<float, 4> whiteColor()
{
    return {1.0f, 1.0f, 1.0f, 1.0f};
}

} // namespace

AndroidVideoRenderer::AndroidVideoRenderer()
    : m_thread(&AndroidVideoRenderer::run, this)
{
}

AndroidVideoRenderer::~AndroidVideoRenderer()
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
}

void AndroidVideoRenderer::setSurface(ANativeWindow* window)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pending_window != nullptr)
    {
        ANativeWindow_release(m_pending_window);
    }
    m_pending_window = window;
    m_surface_dirty = true;
    m_cv.notify_all();
}

void AndroidVideoRenderer::clearSurface()
{
    setSurface(nullptr);
}

void AndroidVideoRenderer::submitFrame(const uint8_t* rgba, size_t size, int width, int height, int stride)
{
    if (rgba == nullptr || size == 0 || width <= 0 || height <= 0 || stride < width * 4)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending_frame.assign(rgba, rgba + size);
    m_frame_width = width;
    m_frame_height = height;
    m_frame_stride = stride;
    m_frame_dirty = true;
    m_cv.notify_all();
}

void AndroidVideoRenderer::setScreenMode(int screen_mode)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_screen_mode = std::clamp(screen_mode, 0, 2);
    m_mode_dirty = true;
    m_cv.notify_all();
}

void AndroidVideoRenderer::setFontAtlasPng(const uint8_t* png_data, size_t size)
{
    (void)png_data;
    (void)size;
}

void AndroidVideoRenderer::setMenuFontTtf(const uint8_t* ttf_data, size_t size)
{
    if (ttf_data == nullptr || size == 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending_font_ttf.assign(ttf_data, ttf_data + size);
    m_font_dirty = true;
    m_overlay_dirty = true;
    m_cv.notify_all();
}

void AndroidVideoRenderer::setOverlayState(const std::vector<OverlayChip>& chips, const OverlayMenuState& menu_state)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_overlay_chips = chips;
    m_overlay_menu = menu_state;
    m_overlay_dirty = true;
    m_cv.notify_all();
}

void AndroidVideoRenderer::run()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_exit)
    {
        m_cv.wait(lock, [this] {
            return m_exit || m_surface_dirty || m_frame_dirty || m_mode_dirty || m_overlay_dirty || m_font_dirty;
        });

        if (m_exit)
        {
            break;
        }

        if (m_surface_dirty)
        {
            applyPendingSurfaceLocked();
        }

        if (m_egl_display == nullptr || m_egl_surface == nullptr || m_program == 0)
        {
            m_frame_dirty = false;
            m_mode_dirty = false;
            m_overlay_dirty = false;
            m_font_dirty = false;
            continue;
        }

        if (m_font_dirty)
        {
            uploadFontTextureLocked();
            m_font_dirty = false;
        }

        if (m_frame_dirty)
        {
            uploadFrameLocked();
            m_frame_dirty = false;
        }
        else if (m_mode_dirty || m_overlay_dirty)
        {
            drawFrameLocked();
        }

        m_mode_dirty = false;
        m_overlay_dirty = false;
    }

    destroyEglLocked();
    if (m_pending_window != nullptr)
    {
        ANativeWindow_release(m_pending_window);
        m_pending_window = nullptr;
    }
}

void AndroidVideoRenderer::applyPendingSurfaceLocked()
{
    destroyEglLocked();
    if (m_window != nullptr)
    {
        ANativeWindow_release(m_window);
        m_window = nullptr;
    }

    m_window = m_pending_window;
    m_pending_window = nullptr;
    m_surface_dirty = false;
    m_has_uploaded_frame = false;

    if (m_window != nullptr)
    {
        initEglLocked();
        if (m_egl_display != nullptr)
        {
            glClearColor(0.04f, 0.05f, 0.08f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(static_cast<EGLDisplay>(m_egl_display), static_cast<EGLSurface>(m_egl_surface));
        }
    }
}

bool AndroidVideoRenderer::initEglLocked()
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

    const EGLSurface surface = eglCreateWindowSurface(display, config, m_window, nullptr);
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

    m_program = createProgram();
    if (m_program == 0)
    {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return false;
    }

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &m_white_texture);
    glBindTexture(GL_TEXTURE_2D, m_white_texture);
    const std::array<uint8_t, 4> white_pixel = {255, 255, 255, 255};
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white_pixel.data());

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_surface_width = ANativeWindow_getWidth(m_window);
    m_surface_height = ANativeWindow_getHeight(m_window);
    m_uploaded_width = 0;
    m_uploaded_height = 0;
    m_egl_display = display;
    m_egl_surface = surface;
    m_egl_context = context;
    return true;
}

void AndroidVideoRenderer::destroyEglLocked()
{
    const EGLDisplay display = static_cast<EGLDisplay>(m_egl_display);
    const EGLSurface surface = static_cast<EGLSurface>(m_egl_surface);
    const EGLContext context = static_cast<EGLContext>(m_egl_context);

    if (m_font_texture != 0)
    {
        glDeleteTextures(1, &m_font_texture);
        m_font_texture = 0;
    }
    if (m_white_texture != 0)
    {
        glDeleteTextures(1, &m_white_texture);
        m_white_texture = 0;
    }
    if (m_texture != 0)
    {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }
    if (m_program != 0)
    {
        glDeleteProgram(m_program);
        m_program = 0;
    }

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
    m_uploaded_width = 0;
    m_uploaded_height = 0;
}

void AndroidVideoRenderer::uploadFrameLocked()
{
    ensureTextureLocked();
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        m_frame_width,
        m_frame_height,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        m_pending_frame.data());
    m_has_uploaded_frame = true;
    drawFrameLocked();
}

void AndroidVideoRenderer::ensureTextureLocked()
{
    if (m_texture == 0 || m_frame_width <= 0 || m_frame_height <= 0)
    {
        return;
    }
    if (m_uploaded_width == m_frame_width && m_uploaded_height == m_frame_height)
    {
        return;
    }

    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        m_frame_width,
        m_frame_height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        nullptr);
    m_uploaded_width = m_frame_width;
    m_uploaded_height = m_frame_height;
}

bool AndroidVideoRenderer::uploadFontTextureLocked()
{
    if (m_pending_font_ttf.empty())
    {
        return false;
    }

    constexpr int atlas_width = 512;
    constexpr int atlas_height = 512;
    std::vector<unsigned char> alpha_pixels(static_cast<size_t>(atlas_width) * static_cast<size_t>(atlas_height), 0);
    std::vector<stbtt_bakedchar> baked(static_cast<size_t>(kMenuGlyphCount));
    const int bake_result = stbtt_BakeFontBitmap(
        m_pending_font_ttf.data(),
        0,
        kMenuBakedPixelHeight,
        alpha_pixels.data(),
        atlas_width,
        atlas_height,
        kMenuFirstChar,
        kMenuGlyphCount,
        baked.data());
    if (bake_result <= 0)
    {
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "Menu font bake failed");
        return false;
    }

    std::vector<uint8_t> rgba_pixels(static_cast<size_t>(atlas_width) * static_cast<size_t>(atlas_height) * 4, 255);
    for (size_t i = 0; i < alpha_pixels.size(); ++i)
    {
        rgba_pixels[i * 4 + 3] = alpha_pixels[i];
    }

    m_font.pixels = std::move(rgba_pixels);
    m_font.width = atlas_width;
    m_font.height = atlas_height;
    m_font.first_char = kMenuFirstChar;
    m_font.glyph_count = kMenuGlyphCount;
    m_font.baked_pixel_height = kMenuBakedPixelHeight;
    m_font.min_yoff = 0.0f;
    m_font.max_ybottom = 0.0f;
    m_font.glyphs.resize(static_cast<size_t>(kMenuGlyphCount));
    for (int glyph = 0; glyph < kMenuGlyphCount; ++glyph)
    {
        const stbtt_bakedchar& src = baked[static_cast<size_t>(glyph)];
        auto& dst = m_font.glyphs[static_cast<size_t>(glyph)];
        dst.x0 = src.x0;
        dst.y0 = src.y0;
        dst.x1 = src.x1;
        dst.y1 = src.y1;
        dst.xoff = src.xoff;
        dst.yoff = src.yoff;
        dst.xadvance = src.xadvance;

        const float glyph_top = src.yoff;
        const float glyph_bottom = src.yoff + static_cast<float>(src.y1 - src.y0);
        if (glyph == 0)
        {
            m_font.min_yoff = glyph_top;
            m_font.max_ybottom = glyph_bottom;
        }
        else
        {
            m_font.min_yoff = std::min(m_font.min_yoff, glyph_top);
            m_font.max_ybottom = std::max(m_font.max_ybottom, glyph_bottom);
        }
    }

    if (m_font_texture == 0)
    {
        glGenTextures(1, &m_font_texture);
    }
    glBindTexture(GL_TEXTURE_2D, m_font_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 m_font.width,
                 m_font.height,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 m_font.pixels.data());
    return true;
}

void AndroidVideoRenderer::drawTexturedQuadLocked(float x,
                                                  float y,
                                                  float width,
                                                  float height,
                                                  float u0,
                                                  float v0,
                                                  float u1,
                                                  float v1,
                                                  unsigned int texture,
                                                  const std::array<float, 4>& color)
{
    if (texture == 0 || m_surface_width <= 0 || m_surface_height <= 0)
    {
        return;
    }

    const GLfloat vertices[] = {
        toNdcX(x, static_cast<float>(m_surface_width)),         toNdcY(y + height, static_cast<float>(m_surface_height)), u0, v1,
        toNdcX(x + width, static_cast<float>(m_surface_width)), toNdcY(y + height, static_cast<float>(m_surface_height)), u1, v1,
        toNdcX(x, static_cast<float>(m_surface_width)),         toNdcY(y, static_cast<float>(m_surface_height)),          u0, v0,
        toNdcX(x + width, static_cast<float>(m_surface_width)), toNdcY(y, static_cast<float>(m_surface_height)),          u1, v0
    };

    glUseProgram(m_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(m_program, "uTexture"), 0);
    glUniform4f(glGetUniformLocation(m_program, "uColor"), color[0], color[1], color[2], color[3]);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void AndroidVideoRenderer::drawRectLocked(float x, float y, float width, float height, const std::array<float, 4>& color)
{
    drawTexturedQuadLocked(x, y, width, height, 0.0f, 0.0f, 1.0f, 1.0f, m_white_texture, color);
}

void AndroidVideoRenderer::drawTextLocked(float x, float y, float char_height, const std::string& text, const std::array<float, 4>& color)
{
    if (m_font_texture == 0 || m_font.glyph_count <= 0 || text.empty() || m_font.glyphs.empty())
    {
        return;
    }

    const float scale = char_height / std::max(1.0f, m_font.baked_pixel_height);
    float cursor_x = x;
    for (const unsigned char ch : text)
    {
        const int glyph_index = std::clamp<int>(static_cast<int>(ch) - m_font.first_char, 0, m_font.glyph_count - 1);
        const auto& glyph = m_font.glyphs[static_cast<size_t>(glyph_index)];
        const float glyph_width = static_cast<float>(glyph.x1 - glyph.x0) * scale;
        const float glyph_height = static_cast<float>(glyph.y1 - glyph.y0) * scale;
        const float draw_x = cursor_x + glyph.xoff * scale;
        const float draw_y = y + glyph.yoff * scale;
        const float u0 = static_cast<float>(glyph.x0) / static_cast<float>(m_font.width);
        const float u1 = static_cast<float>(glyph.x1) / static_cast<float>(m_font.width);
        const float v0 = static_cast<float>(glyph.y0) / static_cast<float>(m_font.height);
        const float v1 = static_cast<float>(glyph.y1) / static_cast<float>(m_font.height);
        drawTexturedQuadLocked(draw_x, draw_y, glyph_width, glyph_height, u0, v0, u1, v1, m_font_texture, color);
        cursor_x += glyph.xadvance * scale;
    }
}

void AndroidVideoRenderer::drawTextLineLocked(float x,
                                              float y,
                                              float max_width,
                                              float box_height,
                                              float char_height,
                                              const std::string& text,
                                              const std::array<float, 4>& color)
{
    const float char_width = char_height * 0.6f;
    const int max_chars = std::max(0, static_cast<int>(std::floor(max_width / std::max(char_width, 1.0f))));
    std::string clipped = text;
    if (max_chars > 0 && static_cast<int>(clipped.size()) > max_chars)
    {
        clipped.resize(static_cast<size_t>(max_chars));
    }
    if (clipped.empty())
    {
        return;
    }

    const float scale = char_height / std::max(1.0f, m_font.baked_pixel_height);
    const float line_top = m_font.min_yoff * scale;
    const float line_bottom = m_font.max_ybottom * scale;
    const float line_height = std::max(1.0f, line_bottom - line_top);
    const float aligned_y = y + (box_height - line_height) * 0.5f - line_top;
    drawTextLocked(x, aligned_y, char_height, clipped, color);
}

void AndroidVideoRenderer::drawHudLocked()
{
    if (m_overlay_chips.empty())
    {
        return;
    }

    float x = 8.0f;
    const float y = 8.0f;
    const float chip_height = std::max(20.0f, static_cast<float>(m_surface_height) * 0.04f);
    const float text_height = chip_height * 0.62f;
    for (const auto& chip : m_overlay_chips)
    {
        if (chip.text.empty())
        {
            continue;
        }

        const float chip_width = std::max(44.0f, 16.0f + static_cast<float>(chip.text.size()) * text_height * 0.6f);
        drawRectLocked(x,
                       y,
                       chip_width,
                       chip_height,
                       chip.alert ? std::array<float, 4>{0.54f, 0.29f, 0.29f, 0.80f}
                                  : std::array<float, 4>{0.42f, 0.42f, 0.42f, 0.80f});
        drawTextLineLocked(x + 8.0f,
                           y,
                           chip_width - 16.0f,
                           chip_height,
                           text_height,
                           chip.text,
                           whiteColor());
        x += chip_width + 6.0f;
    }
}

void AndroidVideoRenderer::drawMenuLocked()
{
    if (!m_overlay_menu.visible)
    {
        return;
    }

    const float scale = linuxMenuScaleForSurface(m_surface_width, m_surface_height);
    const float menu_width = kLinuxMenuWindowWidth * scale;
    const float menu_height = kLinuxMenuWindowHeight * scale;
    const float menu_x = std::floor((static_cast<float>(m_surface_width) - menu_width) * 0.5f);
    const float menu_offset_y = (m_surface_height == 576) ? 100.0f : kLinuxMenuCenterYOffset;
    const float menu_y = std::floor((static_cast<float>(m_surface_height) - menu_height) * 0.5f + menu_offset_y * scale);
    const float button_height = kLinuxMenuButtonHeight * scale;
    const float button_width = kLinuxMenuButtonWidth * scale;
    const float item_x = menu_x + kLinuxMenuItemInset * scale;
    const float item_text_x = item_x + 8.0f * scale;
    const float title_text_x = menu_x + 10.0f * scale;
    const float text_height = kMenuBakedPixelHeight * kLinuxMenuFontGlobalScale * scale;
    const float large_gap = (static_cast<float>(m_surface_height) > 480.0f) ? kLinuxMenuGapLarge * scale : 0.0f;
    const float small_gap = kLinuxMenuGapSmall * scale;
    float cursor_y = menu_y;

    drawRectLocked(0.0f, 0.0f, static_cast<float>(m_surface_width), static_cast<float>(m_surface_height), {0.0f, 0.0f, 0.0f, 0.40f});
    drawRectLocked(menu_x, cursor_y, menu_width, button_height, {0.38f, 0.54f, 0.41f, 1.0f});
    drawTextLineLocked(title_text_x,
                       cursor_y,
                       menu_width - 20.0f * scale,
                       button_height,
                       text_height,
                       m_overlay_menu.title,
                       {0.93f, 0.97f, 0.95f, 1.0f});
    cursor_y += button_height + large_gap;

    for (size_t index = 0; index < m_overlay_menu.items.size(); ++index)
    {
        const float row_y = cursor_y;
        const bool selected = static_cast<int>(index) == m_overlay_menu.selected_index;
        drawRectLocked(item_x,
                       row_y,
                       button_width,
                       button_height,
                       selected ? std::array<float, 4>{0.30f, 0.54f, 0.80f, 1.0f}
                                : std::array<float, 4>{0.15f, 0.20f, 0.35f, 1.0f});

        std::string item_text = m_overlay_menu.items[index];
        if (index < m_overlay_menu.statuses.size() && !m_overlay_menu.statuses[index].empty())
        {
            item_text += " ";
            item_text += m_overlay_menu.statuses[index];
        }
        drawTextLineLocked(item_text_x,
                           row_y,
                           button_width - 16.0f * scale,
                           button_height,
                           text_height,
                           item_text,
                           {1.0f, 1.0f, 1.0f, 1.0f});
        cursor_y += button_height;
    }

    if (!m_overlay_menu.status_lines.empty())
    {
        cursor_y += large_gap;
        for (size_t index = 0; index < std::min<size_t>(2, m_overlay_menu.status_lines.size()); ++index)
        {
            drawRectLocked(menu_x,
                           cursor_y,
                           menu_width,
                           button_height,
                           {0.19f, 0.19f, 0.19f, 1.0f});
            drawTextLineLocked(title_text_x,
                               cursor_y,
                               menu_width - 20.0f * scale,
                               button_height,
                               text_height,
                               m_overlay_menu.status_lines[index],
                               {1.0f, 1.0f, 1.0f, 1.0f});
            cursor_y += button_height + small_gap;
        }
    }

    if (!m_overlay_menu.footer.empty())
    {
        drawTextLineLocked(menu_x + menu_width - 240.0f * scale,
                           menu_y + menu_height - button_height,
                           230.0f * scale,
                           button_height,
                           text_height,
                           m_overlay_menu.footer,
                           {0.80f, 0.83f, 0.90f, 1.0f});
    }

    const NavPadLayout nav = buildNavPadLayout(m_surface_width, m_surface_height);
    const float nav_text_height = nav.size * kAndroidNavLabelScale;
    const std::array<float, 4> active_bg = {0.16f, 0.20f, 0.26f, 0.92f};
    const std::array<float, 4> back_bg = {0.22f, 0.18f, 0.18f, 0.92f};
    const std::array<float, 4> enter_bg = {0.18f, 0.27f, 0.18f, 0.92f};
    const std::array<float, 4> text_color = {0.95f, 0.97f, 0.98f, 1.0f};

    drawRectLocked(nav.center_x, nav.up_y, nav.size, nav.size, active_bg);
    drawTextLineLocked(nav.center_x, nav.up_y, nav.size, nav.size, nav_text_height, "UP", text_color);

    drawRectLocked(nav.left_x, nav.mid_y, nav.size, nav.size, back_bg);
    drawTextLineLocked(nav.left_x, nav.mid_y, nav.size, nav.size, nav_text_height, "BACK", text_color);

    drawRectLocked(nav.right_x, nav.mid_y, nav.size, nav.size, enter_bg);
    drawTextLineLocked(nav.right_x, nav.mid_y, nav.size, nav.size, nav_text_height, "OK", text_color);

    drawRectLocked(nav.center_x, nav.down_y, nav.size, nav.size, active_bg);
    drawTextLineLocked(nav.center_x, nav.down_y, nav.size, nav.size, nav_text_height, "DOWN", text_color);
}

void AndroidVideoRenderer::drawOverlayLocked()
{
    drawHudLocked();
    drawMenuLocked();
}

void AndroidVideoRenderer::drawFrameLocked()
{
    if (m_surface_width <= 0 || m_surface_height <= 0)
    {
        return;
    }

    glViewport(0, 0, m_surface_width, m_surface_height);
    glClearColor(0.04f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const float video_aspect = static_cast<float>(m_frame_width) / static_cast<float>(m_frame_height);
    const float screen_aspect = static_cast<float>(m_surface_width) / static_cast<float>(m_surface_height);

    float x_scale = 1.0f;
    float y_scale = 1.0f;
    float u0 = 0.0f;
    float u1 = 1.0f;
    float v0 = 1.0f;
    float v1 = 0.0f;

    if (m_screen_mode == 1)
    {
        if (video_aspect > screen_aspect)
        {
            y_scale = screen_aspect / video_aspect;
        }
        else
        {
            x_scale = video_aspect / screen_aspect;
        }
    }
    else if (m_screen_mode == 2)
    {
        if (video_aspect > screen_aspect)
        {
            const float visible = screen_aspect / video_aspect;
            const float margin = (1.0f - visible) * 0.5f;
            u0 = margin;
            u1 = 1.0f - margin;
        }
        else
        {
            const float visible = video_aspect / screen_aspect;
            const float margin = (1.0f - visible) * 0.5f;
            v1 = margin;
            v0 = 1.0f - margin;
        }
    }

    const GLfloat vertices[] = {
        -x_scale, -y_scale, u0, v0,
         x_scale, -y_scale, u1, v0,
        -x_scale,  y_scale, u0, v1,
         x_scale,  y_scale, u1, v1
    };

    if (m_has_uploaded_frame)
    {
        glUseProgram(m_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glUniform1i(glGetUniformLocation(m_program, "uTexture"), 0);
        glUniform4f(glGetUniformLocation(m_program, "uColor"), 1.0f, 1.0f, 1.0f, 1.0f);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    drawOverlayLocked();

    eglSwapBuffers(static_cast<EGLDisplay>(m_egl_display), static_cast<EGLSurface>(m_egl_surface));
}
