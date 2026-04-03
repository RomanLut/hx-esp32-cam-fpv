#include "gs_video_renderer.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>

#include "core/osd_menu_controller.h"
#include "core/osd_menu_imgui_shared.h"
#include "gs_runtime_menu_ui.h"
#include "gs_runtime_ui.h"
#include "gs_video_layout_shared.h"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "utils/lodepng.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

namespace
{

constexpr const char* kLogTag = "GsVideoRenderer";
constexpr float kLinuxMenuFontGlobalScale = 2.0f;
constexpr float kTouchNavLabelScale = 0.75f;

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

bool shouldReplacePendingFrame(uint32_t new_frame_id, uint32_t old_frame_id)
{
    return new_frame_id > old_frame_id ||
           (old_frame_id >= 10u && new_frame_id < old_frame_id - 10u);
}

ImVec4 toImGuiColor(const std::array<float, 4>& color)
{
    return ImVec4(color[0], color[1], color[2], color[3]);
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
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s glError=0x%04x", stage, error);
    }
}

} // namespace

GsVideoRenderer::GsVideoRenderer()
    : m_thread(&GsVideoRenderer::run, this)
{
}

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

void GsVideoRenderer::setSurface(void* surface_handle)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_surface_backend.setSurface(surface_handle);
    m_surface_dirty = true;
    m_cv.notify_all();
}

void GsVideoRenderer::clearSurface()
{
    setSurface(nullptr);
}

void GsVideoRenderer::submitFrame(const uint8_t* pixels,
                                       size_t size,
                                       int width,
                                       int height,
                                       int stride,
                                       uint32_t frame_id,
                                       PixelFormat pixel_format)
{
    const int min_stride = pixel_format == PixelFormat::RGB565 ? width * 2 : width * 3;
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
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_has_pending_frame)
        {
            if (!shouldReplacePendingFrame(frame.frame_id, m_pending_frame.frame_id))
            {
                m_discarded_pending_count.fetch_add(1);
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

void GsVideoRenderer::submitFrame(std::shared_ptr<void> external_frame_ref,
                                       const uint8_t* pixels,
                                       size_t size,
                                       int width,
                                       int height,
                                       int stride,
                                       uint32_t frame_id,
                                       PixelFormat pixel_format)
{
    const int min_stride = pixel_format == PixelFormat::RGB565 ? width * 2 : width * 3;
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

void GsVideoRenderer::submitFrame(std::vector<uint8_t>&& pixels,
                                       int width,
                                       int height,
                                       int stride,
                                       uint32_t frame_id,
                                       PixelFormat pixel_format)
{
    const int min_stride = pixel_format == PixelFormat::RGB565 ? width * 2 : width * 3;
    if (pixels.empty() || width <= 0 || height <= 0 || stride < min_stride)
    {
        return;
    }

    PendingFrame frame;
    frame.pixels = std::move(pixels);
    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.frame_id = frame_id;
    frame.pixel_format = pixel_format;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_has_pending_frame)
        {
            if (!shouldReplacePendingFrame(frame.frame_id, m_pending_frame.frame_id))
            {
                m_discarded_pending_count.fetch_add(1);
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

void GsVideoRenderer::setScreenMode(int screen_mode)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_screen_mode = std::clamp(screen_mode, 0, 2);
    m_mode_dirty = true;
    m_cv.notify_all();
}

void GsVideoRenderer::setVsync(bool enabled)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vsync = enabled;
    m_surface_backend.setVsync(enabled);
}

void GsVideoRenderer::setVrMode(bool enabled)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vr_mode = enabled;
    m_mode_dirty = true;
    m_cv.notify_all();
}

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

void GsVideoRenderer::clearFlightOsd()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    s_flightOSD.clear();
    m_overlay_dirty = true;
    m_cv.notify_all();
}

void GsVideoRenderer::setFlightOsdFont(const std::string& font_name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    s_flightOSD.setFontName(font_name);
    m_overlay_dirty = true;
    m_cv.notify_all();
}

void GsVideoRenderer::setFlightOsdChars(const std::array<std::array<uint8_t, OSD_COLS>, OSD_ROWS>& chars)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    s_flightOSD.clear();
    for (int row = 0; row < OSD_ROWS; ++row)
    {
        for (int col = 0; col < OSD_COLS; ++col)
        {
            const uint8_t c = chars[row][col];
            if (c != 0)
            {
                s_flightOSD.setLowChar(row, col, c);
            }
        }
    }
    m_overlay_dirty = true;
    m_cv.notify_all();
}

void GsVideoRenderer::setFrameUiState(const RuntimeFrameUiState& frame_ui_state)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frame_ui_state = frame_ui_state;
    m_screen_mode = std::clamp(frame_ui_state.screen_mode, 0, 2);
    m_vsync = frame_ui_state.vsync;
    m_vr_mode = frame_ui_state.vr_mode;
    m_menu_footer = frame_ui_state.menu_footer;
    m_surface_backend.setVsync(m_vsync);
    m_overlay_dirty = true;
    m_cv.notify_all();
}

void GsVideoRenderer::setMenuBinding(gs::menu::OSDMenuController* menu_controller,
                                          Ground2Air_Config_Packet* config_packet,
                                          std::mutex* menu_mutex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_menu_controller = menu_controller;
    m_menu_config = config_packet;
    m_menu_mutex = menu_mutex;
}

void GsVideoRenderer::setMenuFooter(const std::string& footer)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_menu_footer = footer;
}

bool GsVideoRenderer::isMenuVisible()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_menu_visible;
}

void GsVideoRenderer::queueMenuOpen()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_open_menu_requested = true;
    m_close_menu_requested = false;
    m_overlay_dirty = true;
    m_cv.notify_all();
}

void GsVideoRenderer::queueMenuClose()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_close_menu_requested = true;
    m_open_menu_requested = false;
    m_overlay_dirty = true;
    m_cv.notify_all();
}

void GsVideoRenderer::queueMouseTap(float x, float y)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_touch_pending = true;
    m_touch_x = x;
    m_touch_y = y;
    ++m_touch_sequence;
    m_cv.notify_all();
}

void GsVideoRenderer::queueKeyPress(ImGuiKey key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending_key_presses.push_back(key);
    m_cv.notify_all();
}

GsVideoRenderer::MenuAction GsVideoRenderer::dispatchTap(float x, float y)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_overlay_menu.visible || m_imgui_context == nullptr)
    {
        return {};
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
        return {MenuActionKind::Activate, -1};
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

void GsVideoRenderer::run()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_exit)
    {
        m_cv.wait(lock, [this] {
            return m_exit ||
                   m_surface_dirty ||
                   m_frame_dirty.load() ||
                   m_mode_dirty ||
                   m_overlay_dirty ||
                   m_touch_pending ||
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

        if (!m_surface_backend.isReady() || m_program == 0)
        {
            m_frame_dirty.store(false);
            m_mode_dirty = false;
            m_overlay_dirty = false;
            if (m_touch_pending)
            {
                m_touch_pending = false;
                m_touch_processed_sequence = m_touch_sequence;
                m_touch_action = {};
                m_cv.notify_all();
            }
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
                m_frame_stride = m_locked_frame.stride;
                have_new_frame = true;
            }

            if (have_new_frame)
            {
                uploadFrameLocked();
            }

            m_frame_dirty.store(m_has_pending_frame);
        }
        else if (m_mode_dirty || m_overlay_dirty || m_touch_pending || !m_pending_key_presses.empty())
        {
            drawFrameLocked();
        }

        m_mode_dirty = false;
        m_overlay_dirty = false;
    }

    destroyImGuiLocked();
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
}

void GsVideoRenderer::applyPendingSurfaceLocked()
{
    destroyImGuiLocked();
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
    m_surface_dirty = false;
    m_has_uploaded_frame = false;
    m_uploaded_pixel_format = PixelFormat::RGB24;
    m_uploaded_width = 0;
    m_uploaded_height = 0;
    m_surface_width = 0;
    m_surface_height = 0;

    if (m_surface_backend.applyPendingSurface(m_vsync))
    {
        m_program = createProgram();
        if (m_program != 0)
        {
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

            m_surface_width = m_surface_backend.surfaceWidth();
            m_surface_height = m_surface_backend.surfaceHeight();

            glClearColor(0.04f, 0.05f, 0.08f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            m_surface_backend.swapBuffers();
            initImGuiLocked();
        }
    }
}

void GsVideoRenderer::releaseFrameRefLocked(PendingFrame& frame)
{
    frame.external_frame_ref.reset();
    frame.external_pixels = nullptr;
    frame.external_size = 0;
    frame.pixels.clear();
}

bool GsVideoRenderer::initImGuiLocked()
{
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
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    io.DisplaySize = ImVec2(static_cast<float>(m_surface_width), static_cast<float>(m_surface_height));

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScrollbarSize = static_cast<float>(m_surface_width) / 80.0f;
    style.ItemInnerSpacing = ImVec2(style.ItemSpacing.x / 2.0f, style.ItemSpacing.y / 2.0f);
    io.FontGlobalScale = kLinuxMenuFontGlobalScale;

    if (!ImGui_ImplOpenGL3_Init("#version 100"))
    {
        destroyImGuiLocked();
        return false;
    }

    return true;
}

void GsVideoRenderer::destroyImGuiLocked()
{
    if (m_imgui_context == nullptr)
    {
        return;
    }

    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_context));
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext(static_cast<ImGuiContext*>(m_imgui_context));
    m_imgui_context = nullptr;
}

void GsVideoRenderer::uploadFrameLocked()
{
    if (m_locked_frame.pixels.empty() && m_locked_frame.external_pixels == nullptr)
    {
        return;
    }

    const auto upload_begin = std::chrono::steady_clock::now();
    ensureTextureLocked();
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const void* upload_pixels = m_locked_frame.external_pixels != nullptr
        ? static_cast<const void*>(m_locked_frame.external_pixels)
        : static_cast<const void*>(m_locked_frame.pixels.data());
    const GLenum upload_type = m_locked_frame.pixel_format == PixelFormat::RGB565
        ? GL_UNSIGNED_SHORT_5_6_5
        : GL_UNSIGNED_BYTE;
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        m_frame_width,
        m_frame_height,
        GL_RGB,
        upload_type,
        upload_pixels);
    logGlError("glTexSubImage2D");
    const uint32_t duration_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - upload_begin).count());
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
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGB,
        m_frame_width,
        m_frame_height,
        0,
        GL_RGB,
        texture_type,
        nullptr);
    logGlError("glTexImage2D");
    m_uploaded_width = m_frame_width;
    m_uploaded_height = m_frame_height;
    m_uploaded_pixel_format = m_locked_frame.pixel_format;
}

void GsVideoRenderer::drawTexturedQuadLocked(float x,
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

void GsVideoRenderer::drawRectLocked(float x, float y, float width, float height, const std::array<float, 4>& color)
{
    drawTexturedQuadLocked(x, y, width, height, 0.0f, 0.0f, 1.0f, 1.0f, m_white_texture, color);
}

void GsVideoRenderer::drawHudLocked()
{
    drawHudImGuiLocked();
}

void GsVideoRenderer::drawHudImGuiLocked()
{
    if (m_imgui_context == nullptr)
    {
        return;
    }

    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_context));
    drawRuntimeFrameUiContent(m_frame_ui_state);
}

void GsVideoRenderer::drawStatsLocked()
{
}

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
        if (m_close_menu_requested)
        {
            m_menu_controller->close();
        }
        m_open_menu_requested = false;
        m_close_menu_requested = false;
        m_menu_visible = m_menu_controller->isVisible();
    }

    if (!m_menu_visible)
    {
        return;
    }

    drawRectLocked(0.0f, 0.0f, static_cast<float>(m_surface_width), static_cast<float>(m_surface_height), {0.0f, 0.0f, 0.0f, 0.40f});
    drawMenuImGuiLocked();
}

void GsVideoRenderer::drawMenuImGuiLocked()
{
    if (m_imgui_context == nullptr || m_menu_controller == nullptr || m_menu_config == nullptr)
    {
        return;
    }

    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_context));
    RuntimeMenuUiState menu_ui = {};
    menu_ui.visible = m_menu_visible;
    menu_ui.vr_mode = m_vr_mode;
    menu_ui.touch_nav_enabled = true;
    menu_ui.surface_width = static_cast<float>(m_surface_width);
    menu_ui.surface_height = static_cast<float>(m_surface_height);
    drawRuntimeMenuUi(menu_ui, [this]
    {
        if (m_menu_mutex != nullptr)
        {
            std::lock_guard<std::mutex> menu_lock(*m_menu_mutex);
            m_menu_controller->draw(*m_menu_config);
            m_menu_visible = m_menu_controller->isVisible();
        }
    });
}

void GsVideoRenderer::drawOverlayLocked()
{
    if (m_imgui_context != nullptr)
    {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_context));
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(m_surface_width), static_cast<float>(m_surface_height));
        io.DeltaTime = 1.0f / 60.0f;
        m_touch_action = {};
        if (m_touch_pending)
        {
            io.AddMousePosEvent(m_touch_x, m_touch_y);
            io.AddMouseButtonEvent(0, true);
            io.AddMouseButtonEvent(0, false);
        }
        for (const ImGuiKey key : m_pending_key_presses)
        {
            io.AddKeyEvent(key, true);
            io.AddKeyEvent(key, false);
        }
        m_pending_key_presses.clear();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 0.0f));
        ImGui::Begin("FULLSCREEN_OVERLAY",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoFocusOnAppearing);
    }

    s_flightOSD.draw(m_surface_width, m_surface_height, m_frame_width, m_frame_height, m_screen_mode, m_vr_mode);
    drawHudLocked();
    drawStatsLocked();
    drawMenuLocked();

    if (m_touch_pending)
    {
        m_touch_pending = false;
        m_touch_processed_sequence = m_touch_sequence;
        m_cv.notify_all();
    }

    if (m_imgui_context != nullptr)
    {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_context));
        ImGui::End();
        ImGui::PopStyleVar(4);
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
}

void GsVideoRenderer::drawFrameLocked()
{
    if (m_surface_width <= 0 || m_surface_height <= 0)
    {
        return;
    }

    glViewport(0, 0, m_surface_width, m_surface_height);
    glClearColor(0.04f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (m_has_uploaded_frame)
    {
        const auto drawVideoCopy = [this](float rect_x, float rect_y, float rect_width, float rect_height)
        {
            const gs::render::VideoQuad quad =
                gs::render::buildVideoQuad(rect_x,
                                           rect_y,
                                           rect_width,
                                           rect_height,
                                           m_frame_width,
                                           m_frame_height,
                                           m_screen_mode);

            drawTexturedQuadLocked(
                quad.x,
                quad.y,
                quad.width,
                quad.height,
                quad.u0,
                quad.v0,
                quad.u1,
                quad.v1,
                m_texture,
                whiteColor());
        };

        if (m_vr_mode)
        {
            const float half_width = static_cast<float>(m_surface_width) * 0.5f;
            drawVideoCopy(0.0f, 0.0f, half_width, static_cast<float>(m_surface_height));
            drawVideoCopy(half_width, 0.0f, static_cast<float>(m_surface_width) - half_width, static_cast<float>(m_surface_height));
        }
        else
        {
            drawVideoCopy(0.0f, 0.0f, static_cast<float>(m_surface_width), static_cast<float>(m_surface_height));
        }
    }

    drawOverlayLocked();

    const auto swap_begin = std::chrono::steady_clock::now();
    m_surface_backend.swapBuffers();
    const uint32_t swap_duration_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - swap_begin).count());
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
