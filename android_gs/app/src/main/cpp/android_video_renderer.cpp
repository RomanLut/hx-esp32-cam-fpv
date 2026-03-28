#include "android_video_renderer.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>

#include "core/osd_menu_imgui_shared.h"
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

constexpr const char* kLogTag = "AndroidGSRenderer";
constexpr float kLinuxMenuFontGlobalScale = 2.0f;
constexpr float kAndroidNavButtonSize = 72.0f;
constexpr float kAndroidNavGap = 10.0f;
constexpr float kAndroidNavMargin = 18.0f;
constexpr float kAndroidNavLabelScale = 0.75f;

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

bool shouldReplacePendingFrame(uint32_t new_frame_id, uint32_t old_frame_id)
{
    return new_frame_id > old_frame_id ||
           (old_frame_id >= 10u && new_frame_id < old_frame_id - 10u);
}

ImVec4 toImGuiColor(const std::array<float, 4>& color)
{
    return ImVec4(color[0], color[1], color[2], color[3]);
}

bool pointInRect(float x, float y, const AndroidVideoRenderer::Rect& rect)
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

struct AttachGuard
{
    JNIEnv* env = nullptr;
    bool detach = false;

    bool attach()
    {
        JavaVM* vm = androidGetJavaVm();
        if (vm == nullptr)
        {
            return false;
        }

        const jint get_env_result = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (get_env_result == JNI_OK)
        {
            return true;
        }

        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        {
            env = nullptr;
            return false;
        }

        detach = true;
        return true;
    }

    ~AttachGuard()
    {
        JavaVM* vm = androidGetJavaVm();
        if (detach && vm != nullptr)
        {
            vm->DetachCurrentThread();
        }
    }
};

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

    AttachGuard jni;
    if (jni.attach())
    {
        releaseFrameRefLocked(m_locked_frame);
        releaseFrameRefLocked(m_pending_frame);
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

void AndroidVideoRenderer::submitFrame(const uint8_t* pixels,
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

void AndroidVideoRenderer::submitFrame(jobject direct_buffer_global_ref,
                                       const uint8_t* pixels,
                                       size_t size,
                                       int width,
                                       int height,
                                       int stride,
                                       uint32_t frame_id,
                                       PixelFormat pixel_format)
{
    const int min_stride = pixel_format == PixelFormat::RGB565 ? width * 2 : width * 3;
    if (direct_buffer_global_ref == nullptr ||
        pixels == nullptr ||
        size == 0 ||
        width <= 0 ||
        height <= 0 ||
        stride < min_stride)
    {
        return;
    }

    PendingFrame frame;
    frame.direct_buffer_global_ref = direct_buffer_global_ref;
    frame.direct_pixels = pixels;
    frame.direct_size = size;
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

void AndroidVideoRenderer::submitFrame(std::vector<uint8_t>&& pixels,
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

void AndroidVideoRenderer::setScreenMode(int screen_mode)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_screen_mode = std::clamp(screen_mode, 0, 2);
    m_mode_dirty = true;
    m_cv.notify_all();
}

void AndroidVideoRenderer::setOverlayState(const std::vector<OverlayChip>& chips,
                                           const OverlayMenuState& menu_state,
                                           const OverlayStatsState& stats_state,
                                           const OverlayPacketDebugState& packet_debug_state)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_overlay_chips = chips;
    m_overlay_menu = menu_state;
    m_overlay_stats = stats_state;
    m_overlay_packet_debug = packet_debug_state;
    m_overlay_dirty = true;
    m_cv.notify_all();
}

AndroidVideoRenderer::MenuAction AndroidVideoRenderer::dispatchTap(float x, float y)
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

AndroidVideoRenderer::RendererStats AndroidVideoRenderer::consumeStats()
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

void AndroidVideoRenderer::run()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_exit)
    {
        m_cv.wait(lock, [this] {
            return m_exit || m_surface_dirty || m_frame_dirty.load() || m_mode_dirty || m_overlay_dirty || m_touch_pending;
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
    m_uploaded_pixel_format = PixelFormat::RGB24;

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
    return initImGuiLocked();
}

void AndroidVideoRenderer::destroyEglLocked()
{
    const EGLDisplay display = static_cast<EGLDisplay>(m_egl_display);
    const EGLSurface surface = static_cast<EGLSurface>(m_egl_surface);
    const EGLContext context = static_cast<EGLContext>(m_egl_context);

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

    destroyImGuiLocked();

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
    m_uploaded_pixel_format = PixelFormat::RGB24;
}

void AndroidVideoRenderer::releaseFrameRefLocked(PendingFrame& frame)
{
    if (frame.direct_buffer_global_ref != nullptr)
    {
        AttachGuard jni;
        if (jni.attach())
        {
            jni.env->DeleteGlobalRef(frame.direct_buffer_global_ref);
        }
        frame.direct_buffer_global_ref = nullptr;
    }
    frame.direct_pixels = nullptr;
    frame.direct_size = 0;
    frame.pixels.clear();
}

bool AndroidVideoRenderer::initImGuiLocked()
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

void AndroidVideoRenderer::destroyImGuiLocked()
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

void AndroidVideoRenderer::uploadFrameLocked()
{
    if (m_locked_frame.pixels.empty() && m_locked_frame.direct_pixels == nullptr)
    {
        return;
    }

    if (kAndroidPerfMode != AndroidPerfMode::SkipUpload)
    {
        const auto upload_begin = std::chrono::steady_clock::now();
        ensureTextureLocked();
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const void* upload_pixels = m_locked_frame.direct_pixels != nullptr
            ? static_cast<const void*>(m_locked_frame.direct_pixels)
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
    }
    drawFrameLocked();
}

void AndroidVideoRenderer::ensureTextureLocked()
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

void AndroidVideoRenderer::drawHudLocked()
{
    if (m_overlay_chips.empty())
    {
        return;
    }
    drawHudImGuiLocked();
}

void AndroidVideoRenderer::drawHudImGuiLocked()
{
    if (m_imgui_context == nullptr || m_overlay_chips.empty())
    {
        return;
    }

    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_context));
    const float chip_height = std::max(20.0f, static_cast<float>(m_surface_height) * 0.04f);
    const float start_x = 8.0f;
    const float start_y = 8.0f;
    float x = start_x;

    for (const auto& chip : m_overlay_chips)
    {
        if (chip.text.empty())
        {
            continue;
        }

        const ImVec2 text_size = ImGui::CalcTextSize(chip.text.c_str());
        const float chip_width = std::max(44.0f, 16.0f + text_size.x);
        const ImVec4 bg = chip.alert ? ImVec4(0.54f, 0.29f, 0.29f, 0.80f)
                                     : ImVec4(0.42f, 0.42f, 0.42f, 0.80f);
        ImGui::SetCursorPos(ImVec2(x, start_y));
        ImGui::PushStyleColor(ImGuiCol_Button, bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
        ImGui::Button(chip.text.c_str(), ImVec2(chip_width, chip_height));
        ImGui::PopStyleColor(3);
        x += chip_width + 6.0f;
    }
}

void AndroidVideoRenderer::drawStatsLocked()
{
    if (!m_overlay_stats.visible || m_imgui_context == nullptr)
    {
        return;
    }

    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_context));
    gs::stats::drawFullscreenStatsPanel(m_overlay_stats.snapshot);
}

void AndroidVideoRenderer::drawPacketDebugLocked()
{
    if (!m_overlay_packet_debug.visible || m_imgui_context == nullptr || m_overlay_packet_debug.lines.empty())
    {
        return;
    }

    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_context));
    ImGui::SetCursorPos(ImVec2(10.0f, 80.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.65f));
    ImGui::BeginChild("PACKET_DEBUG",
                      ImVec2(std::min(760.0f, ImGui::GetIO().DisplaySize.x - 20.0f),
                             std::min(520.0f, ImGui::GetIO().DisplaySize.y - 90.0f)),
                      true,
                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav);
    for (const auto& line : m_overlay_packet_debug.lines)
    {
        ImGui::TextUnformatted(line.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void AndroidVideoRenderer::drawMenuLocked()
{
    if (!m_overlay_menu.visible)
    {
        return;
    }

    drawRectLocked(0.0f, 0.0f, static_cast<float>(m_surface_width), static_cast<float>(m_surface_height), {0.0f, 0.0f, 0.0f, 0.40f});
    drawMenuImGuiLocked();
}

void AndroidVideoRenderer::drawMenuImGuiLocked()
{
    if (m_imgui_context == nullptr)
    {
        return;
    }

    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui_context));
    const auto layout = gs::menu::imgui::buildMenuFrameLayout(static_cast<float>(m_surface_width),
                                                              static_cast<float>(m_surface_height),
                                                              true,
                                                              29.0f);
    m_menu_item_bounds.clear();
    gs::menu::imgui::beginMenuWindow("OSD_MENU_ANDROID", layout);
    gs::menu::imgui::drawMenuTitle(m_overlay_menu.title.c_str(), layout);
    m_menu_bounds = {
        ImGui::GetWindowPos().x,
        ImGui::GetWindowPos().y,
        ImGui::GetWindowSize().x,
        ImGui::GetWindowSize().y};

    for (size_t index = 0; index < m_overlay_menu.items.size(); ++index)
    {
        std::string item_text = m_overlay_menu.items[index];
        if (index < m_overlay_menu.statuses.size() && !m_overlay_menu.statuses[index].empty())
        {
            item_text += " ";
            item_text += m_overlay_menu.statuses[index];
        }
        if (gs::menu::imgui::drawMenuItem(item_text.c_str(), layout, static_cast<int>(index) == m_overlay_menu.selected_index))
        {
            m_touch_action = {MenuActionKind::SelectItem, static_cast<int>(index)};
        }
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        m_menu_item_bounds.push_back({min.x, min.y, max.x - min.x, max.y - min.y});
    }

    if (!m_overlay_menu.status_lines.empty())
    {
        gs::menu::imgui::drawLargeGap(layout);
        for (size_t index = 0; index < std::min<size_t>(2, m_overlay_menu.status_lines.size()); ++index)
        {
            gs::menu::imgui::drawMenuStatus(m_overlay_menu.status_lines[index].c_str(), layout);
            if (index + 1 < std::min<size_t>(2, m_overlay_menu.status_lines.size()))
            {
                gs::menu::imgui::drawSmallGap(layout);
            }
        }
    }

    gs::menu::imgui::drawMenuFooterRight(m_overlay_menu.footer.c_str(), layout);
    gs::menu::imgui::endMenuWindow();

    const NavPadLayout nav = buildNavPadLayout(m_surface_width, m_surface_height);
    const ImVec2 nav_size(nav.size, nav.size);
    m_nav_up_bounds = {nav.center_x, nav.up_y, nav.size, nav.size};
    m_nav_left_bounds = {nav.left_x, nav.mid_y, nav.size, nav.size};
    m_nav_right_bounds = {nav.right_x, nav.mid_y, nav.size, nav.size};
    m_nav_down_bounds = {nav.center_x, nav.down_y, nav.size, nav.size};
    const ImVec4 active_bg = toImGuiColor({0.16f, 0.20f, 0.26f, 0.92f});
    const ImVec4 back_bg = toImGuiColor({0.22f, 0.18f, 0.18f, 0.92f});
    const ImVec4 enter_bg = toImGuiColor({0.18f, 0.27f, 0.18f, 0.92f});

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(m_surface_width), static_cast<float>(m_surface_height)), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::Begin("OSD_MENU_ANDROID_NAV",
                 nullptr,
                 ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground);

    auto drawNavButton = [&](const char* label, float x, float y, const ImVec4& bg, MenuActionKind action)
    {
        ImGui::SetCursorPos(ImVec2(x, y));
        ImGui::PushStyleColor(ImGuiCol_Button, bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
        if (ImGui::Button(label, nav_size))
        {
            m_touch_action = {action, -1};
        }
        ImGui::PopStyleColor(3);
    };

    drawNavButton("UP", nav.center_x, nav.up_y, active_bg, MenuActionKind::Up);
    drawNavButton("LEFT", nav.left_x, nav.mid_y, back_bg, MenuActionKind::Back);
    drawNavButton("RIGHT", nav.right_x, nav.mid_y, enter_bg, MenuActionKind::Activate);
    drawNavButton("DOWN", nav.center_x, nav.down_y, active_bg, MenuActionKind::Down);

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void AndroidVideoRenderer::drawOverlayLocked()
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
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 0.0f));
        ImGui::Begin("ANDROID_FULLSCREEN_OVERLAY",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoFocusOnAppearing);
    }

    drawHudLocked();
    drawStatsLocked();
    drawPacketDebugLocked();
    drawMenuLocked();

    if (m_touch_pending)
    {
        if (m_touch_action.kind == MenuActionKind::None && !pointInRect(m_touch_x, m_touch_y, m_menu_bounds))
        {
            m_touch_action = {MenuActionKind::Outside, -1};
        }
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

    const auto swap_begin = std::chrono::steady_clock::now();
    eglSwapBuffers(static_cast<EGLDisplay>(m_egl_display), static_cast<EGLSurface>(m_egl_surface));
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
