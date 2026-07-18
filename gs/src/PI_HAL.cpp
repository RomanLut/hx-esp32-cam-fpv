#include "main.h"

#include "PI_HAL.h"
#include "Log.h"

#define USE_SDL
//#define USE_MANGA_SCREEN2

//#define USE_BOUBLE_BUFFER

#include <fstream>
#include <future>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <cfloat>
#include <cstdlib>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "../../components/common/Clock.h"
#include "../../components_gs/mcp/gs_mcp_server.h"
#include "gs_runtime_state.h"
#include "gs_shared_state.h"
#include "gs_stats.h"
#include "gs_lens_correction_shared.h"
#include "gs_video_shader_renderer.h"
#include "gs_video_stabilization_shared.h"

#ifdef USE_MANGA_SCREEN2
#include <tslib.h> //needs libts-dev 
#endif


#ifdef USE_SDL
#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include "imgui_impl_sdl2.h"
#else
extern "C"
{
#include "interface/vcos/vcos.h"
#include <bcm_host.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_brcm.h>
}
#endif

namespace
{

static GLuint g_VideoTexture;
static gs::render::VideoShaderRenderer g_VideoShaderRenderer;

//===================================================================================
//===================================================================================
// Maps one injected ImGui key used by MCP to the SDL keycode consumed by the backend.
std::optional<SDL_Keycode> mapInjectedImGuiKeyToSdlKeycode(ImGuiKey key)
{
    switch (key)
    {
    case ImGuiKey_UpArrow: return SDLK_UP;
    case ImGuiKey_DownArrow: return SDLK_DOWN;
    case ImGuiKey_LeftArrow: return SDLK_LEFT;
    case ImGuiKey_RightArrow: return SDLK_RIGHT;
    case ImGuiKey_Enter: return SDLK_RETURN;
    case ImGuiKey_KeypadEnter: return SDLK_KP_ENTER;
    case ImGuiKey_Escape: return SDLK_ESCAPE;
    case ImGuiKey_R: return SDLK_r;
    case ImGuiKey_G: return SDLK_g;
    case ImGuiKey_S: return SDLK_s;
    case ImGuiKey_Space: return SDLK_SPACE;
    default: return std::nullopt;
    }
}

//===================================================================================
//===================================================================================
// Pushes one queued MCP key transition into the SDL event queue before polling input.
void pushInjectedSdlKeyTransition()
{
    gs::mcp::InjectedKeyTransition transition = {};
    if (!gs::mcp::popInjectedKeyTransition(transition))
    {
        return;
    }

    const std::optional<SDL_Keycode> keycode = mapInjectedImGuiKeyToSdlKeycode(transition.key);
    if (!keycode.has_value())
    {
        return;
    }

    SDL_Event event = {};
    event.type = transition.down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.type = event.type;
    event.key.state = transition.down ? SDL_PRESSED : SDL_RELEASED;
    event.key.repeat = 0;
    event.key.keysym.sym = *keycode;
    event.key.keysym.scancode = SDL_GetScancodeFromKey(*keycode);
    SDL_PushEvent(&event);
}

//===================================================================================
//===================================================================================
// Flips final ImGui draw data as one whole-screen image, including video and menus.
void applyScreenFlipToDrawData(ImDrawData* draw_data, float surface_width, float surface_height)
{
    if (draw_data == nullptr)
    {
        return;
    }

    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        ImDrawList* cmd_list = draw_data->CmdLists[n];
        for (ImDrawVert& vertex : cmd_list->VtxBuffer)
        {
            vertex.pos.x = surface_width - vertex.pos.x;
            vertex.pos.y = surface_height - vertex.pos.y;
        }

        for (ImDrawCmd& cmd : cmd_list->CmdBuffer)
        {
            const float x1 = cmd.ClipRect.x;
            const float y1 = cmd.ClipRect.y;
            const float x2 = cmd.ClipRect.z;
            const float y2 = cmd.ClipRect.w;
            cmd.ClipRect.x = surface_width - x2;
            cmd.ClipRect.y = surface_height - y2;
            cmd.ClipRect.z = surface_width - x1;
            cmd.ClipRect.w = surface_height - y1;
        }
    }
}

//===================================================================================
//===================================================================================
// Renders one canonical left-eye ImGui frame into both VR eye regions.
void renderDrawDataWithVrReplication(ImDrawData* draw_data,
                                     float surface_width,
                                     float surface_height,
                                     float vr_separation,
                                     bool screen_flip)
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
        std::vector<ImVec2> positions;
        std::vector<ImVec4> clip_rects;
    };

    std::vector<SavedList> saved(draw_data->CmdListsCount);
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        saved[n].positions.resize(cmd_list->VtxBuffer.Size);
        for (int i = 0; i < cmd_list->VtxBuffer.Size; i++)
        {
            saved[n].positions[i] = cmd_list->VtxBuffer[i].pos;
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
                cmd_list->VtxBuffer[i].pos.x = saved[n].positions[i].x + dx;
                cmd_list->VtxBuffer[i].pos.y = saved[n].positions[i].y;
            }

            for (int i = 0; i < cmd_list->CmdBuffer.Size; i++)
            {
                const ImVec4& source_clip = saved[n].clip_rects[i];
                ImVec4& clip = cmd_list->CmdBuffer[i].ClipRect;
                clip.y = source_clip.y;
                clip.w = source_clip.w;

                // Linux authors one canonical left-eye frame and replays it into
                // both halves. Commands already outside the canonical eye are clipped
                // so stale full-screen windows cannot leak across.
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

        if (screen_flip)
        {
            applyScreenFlipToDrawData(draw_data, surface_width, surface_height);
        }
    };

    apply_eye(+offset, 0.0f, half_w);
    ImGui_ImplOpenGL3_RenderDrawData(draw_data);
    apply_eye(half_w - offset, half_w, full_w);
    ImGui_ImplOpenGL3_RenderDrawData(draw_data);
}

//===================================================================================
//===================================================================================
// Applies GS screen zoom after the video rectangle has been letterboxed.
gs::render::RectI applyScreenZoom(const gs::render::RectI& rect, float zoom)
{
    if (zoom == 1.0f)
    {
        return rect;
    }

    const float center_x = (static_cast<float>(rect.x1) + static_cast<float>(rect.x2)) * 0.5f;
    const float center_y = (static_cast<float>(rect.y1) + static_cast<float>(rect.y2)) * 0.5f;
    const float width = static_cast<float>(rect.x2 - rect.x1) * zoom;
    const float height = static_cast<float>(rect.y2 - rect.y1) * zoom;

    return {
        static_cast<int>(std::round(center_x - width * 0.5f)),
        static_cast<int>(std::round(center_y - height * 0.5f)),
        static_cast<int>(std::round(center_x + width * 0.5f)),
        static_cast<int>(std::round(center_y + height * 0.5f))
    };
}

//===================================================================================
//===================================================================================
// Draws the video texture inside one viewport, clipping zoomed video to that viewport.
void drawVideoInViewport(int quad_x,
                         int clip_x,
                         int y,
                         int width,
                         int height,
                         float video_aspect,
                         ScreenAspectRatio screen_aspect_ratio,
                         float screen_zoom,
                         float surface_width,
                         float surface_height,
                         int frame_width,
                         int frame_height)
{
    const gs::render::RectI letterboxed =
        gs::render::buildLetterboxedRect(quad_x,
                                         y,
                                         width,
                                         height,
                                         video_aspect,
                                         screen_aspect_ratio);
    const gs::render::RectI zoomed = applyScreenZoom(letterboxed, screen_zoom);
    gs::render::VideoQuad quad;
    quad.x = static_cast<float>(zoomed.x1);
    quad.y = static_cast<float>(zoomed.y1);
    quad.width = static_cast<float>(zoomed.x2 - zoomed.x1);
    quad.height = static_cast<float>(zoomed.y2 - zoomed.y1);
    quad.v0 = 0.0f;
    quad.v1 = 1.0f;

    const gs::render::LensCorrectionParams lens_params =
        gs::render::buildLensCorrectionParams(s_lensCorrectionState);
    const gs::stabilization::StabilizationTransform stabilization_transform =
        s_decoder.getRenderStabilizationTransform();
    const gs::render::VideoPostprocessingParams postprocessing_params =
        s_decoder.get_postprocessing_params();
    g_VideoShaderRenderer.draw(g_VideoTexture,
                               quad,
                               static_cast<float>(clip_x),
                               static_cast<float>(y),
                               static_cast<float>(width),
                               static_cast<float>(height),
                               surface_width,
                               surface_height,
                               frame_width,
                               frame_height,
                               lens_params,
                               stabilization_transform,
                               postprocessing_params);
}

//===================================================================================
//===================================================================================
// Refreshes the cached SDL logical window size used by ImGui layout and pointer math.
void refreshSdlWindowSize(SDL_Window* window, uint32_t& width, uint32_t& height)
{
    if (window == nullptr)
    {
        return;
    }

    int window_width = 0;
    int window_height = 0;
    SDL_GetWindowSize(window, &window_width, &window_height);
    if (window_width > 0 && window_height > 0)
    {
        width = static_cast<uint32_t>(window_width);
        height = static_cast<uint32_t>(window_height);
    }
}

} // namespace

/* To install & compile SDL2 with DRM:

--- Install dependencies

sudo apt build-dep libsdl2
sudo apt install libdrm-dev libgbm-dev

--- Build SDL2:
git clone SDL2
cd SDL
mkdir build
cd build 
../configure --disable-video-rpi --enable-video-kmsdrm --enable-video-x11 --disable-video-opengl
make -j5
sudo make install

--- Run:
sudo -E LD_LIBRARY_PATH=/usr/local/lib DISPLAY=:0 ./gs
*/

///////////////////////////////////////////////////////////////////////////////////////////////////

struct PI_HAL::Impl
{
    uint32_t width = 1280;
    uint32_t height = 720;
    int refresh_rate = 0; //Hz, 0 if unknown

    bool fullscreen = true;
    bool vsync = true;

    std::mutex context_mutex;

#ifdef USE_SDL
    SDL_Window* window = nullptr;
    SDL_GLContext context;
#else
    EGLDisplay display = nullptr;
    EGLSurface surface = nullptr;
    EGLContext context = nullptr;
#endif

#ifdef USE_MANGA_SCREEN2
    tsdev* ts = nullptr;
#endif

    constexpr static size_t MAX_TOUCHES = 3;
    constexpr static size_t MAX_SAMPLES = 10;

#ifdef USE_MANGA_SCREEN2
    ts_sample_mt** ts_samples;
#endif

    struct Touch
    {
        int id = 0;
        int32_t x = 0;
        int32_t y = 0;
        bool is_pressed = false;
    };
    std::array<Touch, MAX_TOUCHES> touches;
    bool touch_was_pressed = false;
    bool pointer_tap_pending = false;
    float pointer_tap_x = 0.0f;
    float pointer_tap_y = 0.0f;
    std::function<void(float, float)> pointer_tap_callback;

    bool pigpio_is_isitialized = false;
    float target_backlight = 1.0f;
    float backlight = 0.0f;

#ifdef USE_MANGA_SCREEN2
    std::future<void> backlight_future;
    std::atomic_bool backlight_future_cancelled;
    Clock::time_point backlight_tp = Clock::now();
    std::shared_ptr<FILE> backlight_uart;
#endif
};

///////////////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////////////

bool PI_HAL::init_display_dispmanx()
{
#ifndef USE_SDL
    static const EGLint attribute_list[] =
    {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_BUFFER_SIZE, 8,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    static const EGLint context_attributes[] =
    {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    }; 

    EGLConfig config;

    // get an EGL display connection
    m_impl->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    assert(m_impl->display != EGL_NO_DISPLAY);

    // init the EGL display connection
    EGLBoolean result = eglInitialize(m_impl->display, NULL, NULL);
    assert(result != EGL_FALSE);

    // get an appropriate EGL frame buffer configuration
    EGLint num_config;
    result = eglChooseConfig(m_impl->display, attribute_list, &config, 1, &num_config);
    assert(EGL_FALSE != result);

    result = eglBindAPI(EGL_OPENGL_ES_API);
    assert(result != EGL_FALSE);

    // create an EGL rendering context
    m_impl->context = eglCreateContext(m_impl->display, config, EGL_NO_CONTEXT, context_attributes);
    assert(m_impl->context!=EGL_NO_CONTEXT);

    // create an EGL window surface
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t success = graphics_get_display_size(0 /* LCD */, &width, &height);
    assert(success >= 0);

    VC_RECT_T dst_rect;
    vc_dispmanx_rect_set(&dst_rect, 0, 0, width, height);

    VC_RECT_T src_rect;
    vc_dispmanx_rect_set(&src_rect, 0, 0, (width<<16), (height<<16));

    DISPMANX_DISPLAY_HANDLE_T dispman_display = vc_dispmanx_display_open(0 /* LCD */);
    DISPMANX_UPDATE_HANDLE_T dispman_update = vc_dispmanx_update_start(0);

    VC_DISPMANX_ALPHA_T alpha;
    memset(&alpha, 0, sizeof(alpha));
    alpha.flags = (DISPMANX_FLAGS_ALPHA_T)(DISPMANX_FLAGS_ALPHA_FROM_SOURCE | DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS);
    alpha.opacity = 255;
    alpha.mask = 0;

    DISPMANX_ELEMENT_HANDLE_T dispman_element = vc_dispmanx_element_add(dispman_update, 
                                                dispman_display,
                                                0/*layer*/, 
                                                &dst_rect, 
                                                0/*src*/,
                                                &src_rect, 
                                                DISPMANX_PROTECTION_NONE, 
                                                &alpha /*alpha*/, 
                                                0/*clamp*/, 
                                                DISPMANX_NO_ROTATE/*transform*/);

    static EGL_DISPMANX_WINDOW_T nativewindow;
    nativewindow.element = dispman_element;
    nativewindow.width = width;
    nativewindow.height = height;
    vc_dispmanx_update_submit_sync(dispman_update);

    m_impl->surface = eglCreateWindowSurface(m_impl->display, config, &nativewindow, NULL);
    assert(m_impl->surface != EGL_NO_SURFACE);

    // connect the context to the surface
    result = eglMakeCurrent(m_impl->display, m_impl->surface, m_impl->surface, m_impl->context);
    assert(EGL_FALSE != result);

    eglSwapInterval(m_impl->display, 0);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.KeyRepeatDelay = 1.0f;
    io.KeyRepeatRate = 0.1f;
    io.DisplaySize.x = m_impl->width;
    io.DisplaySize.y = m_impl->height;


#endif
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void PI_HAL::set_video_channel(unsigned int id)
{
    g_VideoTexture = id;   
}

//===================================================================================
//===================================================================================
// Initializes the SDL display, OpenGL context, and ImGui backends.
bool PI_HAL::init_display_sdl()
{
    // Setup SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
    {
        printf("Error: %s\n", SDL_GetError());
        // This function returns bool; -1 converts to true and previously allowed
        // initialization without an ImGui context, causing a segmentation fault.
        return false;
    }

    // Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
    // GL ES 2.0 + GLSL 100
    const char* glsl_version = "#version 100";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#elif defined(__APPLE__)
    // GL 3.2 Core + GLSL 150
    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // Always required on Mac
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    // From 2.0.18: Enable native IME.
#ifdef SDL_HINT_IME_SHOW_UI
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif

    // Create window with graphics context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    if (m_impl->fullscreen)
    {
        int i = 0;
        for ( ; i < 3; i++)
        {
            // Desired display mode
            SDL_DisplayMode desiredMode;
            desiredMode.w = m_impl->width;
            desiredMode.h = m_impl->height;
            desiredMode.format = 0;  // Format 0 means any format
            desiredMode.refresh_rate = 0;  // Refresh rate 0 means any refresh rate
            desiredMode.driverdata = 0;  // Driverdata should be 0

            // Closest display mode found (fallback when no exact resolution mode exists)
            SDL_DisplayMode closestMode;

            printf("Trying mode %d %d...\n", desiredMode.w, desiredMode.h);

            if (SDL_GetClosestDisplayMode(0, &desiredMode, &closestMode))
            {
                // Prefer the highest refresh mode among exact width/height matches.
                SDL_DisplayMode selectedMode = closestMode;
                int best_refresh_rate = closestMode.refresh_rate;
                int best_pixel_format = static_cast<int>(closestMode.format);
                const int display_modes_count = SDL_GetNumDisplayModes(0);
                for (int mode_index = 0; mode_index < display_modes_count; mode_index++)
                {
                    SDL_DisplayMode candidate_mode = {};
                    if (SDL_GetDisplayMode(0, mode_index, &candidate_mode) != 0)
                    {
                        continue;
                    }

                    if (candidate_mode.w != closestMode.w || candidate_mode.h != closestMode.h)
                    {
                        continue;
                    }

                    const int candidate_refresh_rate = candidate_mode.refresh_rate > 0 ? candidate_mode.refresh_rate : 0;
                    const int candidate_pixel_format = static_cast<int>(candidate_mode.format);
                    const bool is_better_refresh = candidate_refresh_rate > best_refresh_rate;
                    const bool is_refresh_tie_with_preferred_format =
                        (candidate_refresh_rate == best_refresh_rate) &&
                        (candidate_pixel_format == static_cast<int>(closestMode.format)) &&
                        (best_pixel_format != static_cast<int>(closestMode.format));
                    const bool is_refresh_tie_with_higher_format_id =
                        (candidate_refresh_rate == best_refresh_rate) &&
                        (candidate_pixel_format > best_pixel_format);
                    if (is_better_refresh || is_refresh_tie_with_preferred_format || is_refresh_tie_with_higher_format_id)
                    {
                        selectedMode = candidate_mode;
                        best_refresh_rate = candidate_refresh_rate;
                        best_pixel_format = candidate_pixel_format;
                    }
                }

                printf("Display mode:");
                printf("  Width: %d\n", selectedMode.w);
                printf("  Height: %d\n", selectedMode.h);
                printf("  Refresh Rate: %d\n", selectedMode.refresh_rate);
                printf("  Pixel Format: %s\n", SDL_GetPixelFormatName(selectedMode.format));

                m_impl->width = selectedMode.w;
                m_impl->height = selectedMode.h;

                SDL_WindowFlags window_flags = (SDL_WindowFlags)(
                    SDL_WINDOW_FULLSCREEN | 
                    SDL_WINDOW_OPENGL | 
                    SDL_WINDOW_SHOWN | 
                    SDL_WINDOW_BORDERLESS );
                m_impl->window = SDL_CreateWindow("esp32-cam-fpv", 0, 0, m_impl->width, m_impl->height, window_flags);

                if (SDL_SetWindowDisplayMode(m_impl->window, &selectedMode) != 0)
                {
                    printf("SDL_SetWindowDisplayMode Error: %s\n", SDL_GetError());
                    SDL_DestroyWindow(m_impl->window);
                    SDL_Quit();
                    return false;
                }
                break;
            }
            else
            {
                if ( i == 0 )
                {
                    //try PAL
                    m_impl->width = 720;
                    m_impl->height = 576;
                }
                else if ( i == 1 )
                {
                    //try NTSC
                    m_impl->width = 720;
                    m_impl->height = 480;
                } 
            }
        }
        
        if ( i == 3 )
        {
            printf("Can not find videomode!" );
            return false;
        }
    }
    else
    {
        SDL_DisplayMode mode;
        int res = SDL_GetCurrentDisplayMode(0, &mode);
        if ( res == 0 )
        {
            if ( (m_impl->width > (uint32_t)mode.w) || (m_impl->height > (uint32_t)mode.h) )
            {
                m_impl->width = mode.w;
                m_impl->height = mode.h;
            }
        }

        SDL_WindowFlags window_flags = (SDL_WindowFlags)(
            SDL_WINDOW_OPENGL | 
            SDL_WINDOW_SHOWN | 
            SDL_WINDOW_RESIZABLE | 
            SDL_WINDOW_ALLOW_HIGHDPI);
        m_impl->window = SDL_CreateWindow("esp32-cam-fpv", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, m_impl->width, m_impl->height, window_flags);
    }

    printf("width:%d height:%d\n",m_impl->width,m_impl->height);

    {
        // Capture the active display mode refresh rate for both fullscreen and
        // windowed paths so the GS display menu can report it.
        SDL_DisplayMode current_mode = {};
        if (SDL_GetWindowDisplayMode(m_impl->window, &current_mode) == 0)
        {
            m_impl->refresh_rate = current_mode.refresh_rate;
        }
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(m_impl->window);
    SDL_GL_MakeCurrent(m_impl->window, gl_context);
    const int requested_swap_interval = m_impl->vsync ? 1 : 0;
    const int swap_interval_result = SDL_GL_SetSwapInterval(requested_swap_interval);
    LOGI("SDL swap interval request={} result={} error={}",
         requested_swap_interval,
         swap_interval_result,
         swap_interval_result == 0 ? "-" : SDL_GetError());

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.KeyRepeatDelay = 1.0f;
    io.KeyRepeatRate = 0.1f;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // Setup Platform/Renderer backends
    ImGui_ImplSDL2_InitForOpenGL(m_impl->window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    ImVec2 display_size = get_display_size();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowBorderSize = 0.0f;
    style.ScrollbarSize = display_size.x / 80.f;
    //style.TouchExtraPadding = ImVec2(style.ScrollbarSize * 2.f, style.ScrollbarSize * 2.f);
    //style.ItemSpacing = ImVec2(size.x / 200, size.x / 200);
    style.ItemInnerSpacing = ImVec2(style.ItemSpacing.x / 2, style.ItemSpacing.y / 2);
    io.FontGlobalScale = 2.f;

    
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

bool PI_HAL::init_display()
{
#ifdef USE_SDL
    if (!init_display_sdl())
        return false;
#else
    if (!init_display_dispmanx())
        return false;
#endif

#ifdef USE_MANGA_SCREEN2
    {
        m_impl->backlight_uart.reset(fopen("/dev/ttyACM0", "wb"), fclose);
        if (!m_impl->backlight_uart)
        {
            LOGW("Failed to initialize backlight uart");
            return false;
        }
    }
#endif

return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void PI_HAL::shutdown_display_dispmanx()
{
#ifndef USE_SDL
    g_VideoShaderRenderer.release();
    // clear screen
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(m_impl->display, m_impl->surface);

    // Release OpenGL resources
    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(m_impl->display, m_impl->surface);
    eglDestroyContext(m_impl->display, m_impl->context);
    eglTerminate(m_impl->display);
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void PI_HAL::shutdown_display_sdl()
{
#ifdef USE_SDL
    g_VideoShaderRenderer.release();
    SDL_GL_DeleteContext(m_impl->context);
    SDL_DestroyWindow(m_impl->window);
    SDL_Quit();
#endif
}
///////////////////////////////////////////////////////////////////////////////////////////////////

void PI_HAL::shutdown_display()
{
#ifdef USE_SDL
    shutdown_display_sdl();
#else
    shutdown_display_dispmanx();
#endif
}

//===================================================================================
//===================================================================================
// Updates SDL input, refreshes the live window layout, and renders one ImGui frame.
bool PI_HAL::update_display()
{
    pushInjectedSdlKeyTransition();

    SDL_Event event;
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    while (SDL_PollEvent(&event))
    {
        // Runtime pointer input is captured as semantic taps instead of being
        // forwarded to ImGui mouse routing. VR/menu controls are hit-tested in
        // canonical overlay coordinates so both eyes and platforms behave alike.
        switch(event.type){
        case SDL_MOUSEBUTTONUP:
            if (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT)
            {
                queue_pointer_tap(static_cast<float>(event.button.x),
                                  static_cast<float>(event.button.y));
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEMOTION:
        case SDL_MOUSEWHEEL:
            break;
        case SDL_FINGERMOTION:
        case SDL_FINGERDOWN:
            break;
        case SDL_FINGERUP:
            queue_pointer_tap(event.tfinger.x * static_cast<float>(m_impl->width),
                              event.tfinger.y * static_cast<float>(m_impl->height));
            break;
        default:
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
            {
                shutdown_display();
            }
            break;
        }
    }
    dispatch_pending_pointer_tap();

    // WSLg/SDL can clamp or resize the window after creation. VR layout must use
    // the live logical window size, otherwise the right eye can be placed beyond
    // ImGui's display rectangle and disappears on the right side of the screen.
    refreshSdlWindowSize(m_impl->window, m_impl->width, m_impl->height);

    // Measure UI construction separately from GL submission and presentation.
    const Clock::time_point ui_build_begin = Clock::now();

    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    io.AddMouseButtonEvent(0, false);
    io.AddMouseButtonEvent(1, false);
    io.AddMouseButtonEvent(2, false);
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    const ImVec2 display_size = get_display_size();
    const int display_width = static_cast<int>(display_size.x);
    const int display_height = static_cast<int>(display_size.y);
    ImGui::SetNextWindowSize(display_size);
    ImGui::SetNextWindowBgAlpha(0);
    ImGui::Begin("mainWindow", nullptr, ImGuiWindowFlags_NoTitleBar | 
                            ImGuiWindowFlags_NoResize | 
                            ImGuiWindowFlags_NoMove | 
                            ImGuiWindowFlags_NoScrollbar | 
                            ImGuiWindowFlags_NoCollapse | 
                            ImGuiWindowFlags_NoInputs);

    const float video_aspect = s_decoder.isAspect16x9() ? (16.0f / 9.0f) : (4.0f / 3.0f);
    const int viewport_width = s_groundstation_config.vrMode ? (display_width / 2) : display_width;
    const ImVec2 frame_resolution = s_decoder.get_video_resolution();
    const int frame_width = static_cast<int>(frame_resolution.x);
    const int frame_height = static_cast<int>(frame_resolution.y);

    for(auto &func:render_callbacks)
    {
        func();
    }

    ImGui::End();

    // Rendering
    ImGui::Render();
    const Clock::time_point ui_build_end = Clock::now();
    const Clock::time_point gl_submit_begin = ui_build_end;
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(0,0,0,1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (s_groundstation_config.vrMode)
    {
        const float half_w = io.DisplaySize.x * 0.5f;
        const float offset = s_groundstation_config.screenVrSeparation * half_w;
        // Zoom is intentionally applied after letterboxing. Per-eye scissor
        // clipping keeps zoomed VR video from bleeding into the other eye.
        drawVideoInViewport(static_cast<int>(std::round(offset)),
                            0,
                            0,
                            viewport_width,
                            display_height,
                            video_aspect,
                            s_groundstation_config.screenAspectRatio,
                            s_groundstation_config.screenZoom,
                            io.DisplaySize.x,
                            io.DisplaySize.y,
                            frame_width,
                            frame_height);
        drawVideoInViewport(static_cast<int>(std::round(half_w - offset)),
                            static_cast<int>(std::round(half_w)),
                            0,
                            viewport_width,
                            display_height,
                            video_aspect,
                            s_groundstation_config.screenAspectRatio,
                            s_groundstation_config.screenZoom,
                            io.DisplaySize.x,
                            io.DisplaySize.y,
                            frame_width,
                            frame_height);
    }
    else
    {
        drawVideoInViewport(0,
                            0,
                            0,
                            viewport_width,
                            display_height,
                            video_aspect,
                            s_groundstation_config.screenAspectRatio,
                            s_groundstation_config.screenZoom,
                            io.DisplaySize.x,
                            io.DisplaySize.y,
                            frame_width,
                            frame_height);
    }
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (s_groundstation_config.vrMode)
    {
        renderDrawDataWithVrReplication(draw_data,
                                        io.DisplaySize.x,
                                        io.DisplaySize.y,
                                        s_groundstation_config.screenVrSeparation,
                                        s_groundstation_config.screenFlipV);
    }
    else
    {
        if (s_groundstation_config.screenFlipV)
        {
            applyScreenFlipToDrawData(draw_data, io.DisplaySize.x, io.DisplaySize.y);
        }
        ImGui_ImplOpenGL3_RenderDrawData(draw_data);
    }
    const Clock::time_point gl_submit_end = Clock::now();

    // glFinish is diagnostic-only because it changes pipeline synchronization.
    // Enable it explicitly to separate GPU completion from the KMS swap wait.
    static const bool force_gpu_finish = []
    {
        const char* value = std::getenv("GS_RENDER_TIMING_GL_FINISH");
        return value != nullptr && value[0] != 0 && value[0] != '0';
    }();
    int gl_finish_duration_us = 0;
    if (force_gpu_finish)
    {
        const Clock::time_point gl_finish_begin = Clock::now();
        glFinish();
        gl_finish_duration_us = static_cast<int>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - gl_finish_begin).count());
    }

    const Clock::time_point swap_begin = Clock::now();
    SDL_GL_SwapWindow(m_impl->window);
    const int ui_build_duration_us = static_cast<int>(
        std::chrono::duration_cast<std::chrono::microseconds>(ui_build_end - ui_build_begin).count());
    const int gl_submit_duration_us = static_cast<int>(
        std::chrono::duration_cast<std::chrono::microseconds>(gl_submit_end - gl_submit_begin).count());
    const int swap_duration_us = static_cast<int>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - swap_begin).count());
    const int swap_duration_ms = swap_duration_us / 1000;
    {
        std::lock_guard<std::mutex> lock(s_gs_stats_mutex);
        s_gs_stats.gpuWaitLastFrameMS = swap_duration_ms;
        s_gs_stats.gpuWaitMaxMS = std::max(s_gs_stats.gpuWaitMaxMS, swap_duration_ms);
        s_gs_stats.renderUiBuildLastUS = ui_build_duration_us;
        s_gs_stats.renderUiBuildMaxUS = std::max(s_gs_stats.renderUiBuildMaxUS, ui_build_duration_us);
        s_gs_stats.renderGlSubmitLastUS = gl_submit_duration_us;
        s_gs_stats.renderGlSubmitMaxUS = std::max(s_gs_stats.renderGlSubmitMaxUS, gl_submit_duration_us);
        s_gs_stats.renderGlFinishLastUS = gl_finish_duration_us;
        s_gs_stats.renderGlFinishMaxUS = std::max(s_gs_stats.renderGlFinishMaxUS, gl_finish_duration_us);
        s_gs_stats.renderSwapLastUS = swap_duration_us;
        s_gs_stats.renderSwapMaxUS = std::max(s_gs_stats.renderSwapMaxUS, swap_duration_us);
        s_gs_stats.renderTimingGlFinishEnabled = force_gpu_finish ? 1 : 0;
    }
    
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

bool PI_HAL::init_ts()
{
#ifdef USE_MANGA_SCREEN2
    m_impl->ts = ts_open("/dev/input/event0", 1);
    if (!m_impl->ts)
    {
        LOGE("ts_open failes");
        return false;
    }
    int res = ts_config(m_impl->ts);
    if (res < 0)
    {
        LOGE("ts_config failed");
        return false;
    }

    m_impl->ts_samples = new ts_sample_mt*[Impl::MAX_SAMPLES];
    for (size_t i = 0; i < Impl::MAX_SAMPLES; i++)
    {
        m_impl->ts_samples[i] = new ts_sample_mt[Impl::MAX_TOUCHES];
        memset(m_impl->ts_samples[i], 0, sizeof(ts_sample_mt));
    }
#endif

    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void PI_HAL::shutdown_ts()
{
#ifdef USE_MANGA_SCREEN2
    ts_close(m_impl->ts);
    m_impl->ts = nullptr;
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void PI_HAL::update_ts()
{
//    for (Impl::Touch& touch: m_impl->touches)
//    {
//        touch.is_pressed = false;
//    }

#ifdef USE_MANGA_SCREEN2
    int ret = ts_read_mt(m_impl->ts, (ts_sample_mt**)m_impl->ts_samples, Impl::MAX_TOUCHES, Impl::MAX_SAMPLES);
    for (int sampleIndex = 0; sampleIndex < ret; sampleIndex++)
    {
        for (size_t slotIndex = 0; slotIndex < Impl::MAX_TOUCHES; slotIndex++)
        {
            Impl::Touch& touch = m_impl->touches[slotIndex];

            ts_sample_mt& sample = m_impl->ts_samples[sampleIndex][slotIndex];
            if (sample.valid < 1)
                continue;

//            printf("%ld.%06ld: %d %6d %6d %6d\n",
//                   sample.tv.tv_sec,
//                   sample.tv.tv_usec,
//                   sample.slot,
//                   sample.x,
//                   sample.y,
//                   sample.pressure);

            touch.is_pressed = sample.pressure > 0;
            touch.x = sample.x;
            touch.y = sample.y;
            touch.id = sample.slot;
        }
    }

    Impl::Touch& touch = m_impl->touches[0];

    float mouse_x = touch.y;
    float mouse_y = static_cast<float>(m_impl->height) - static_cast<float>(touch.x);
    if (m_impl->touch_was_pressed && !touch.is_pressed)
    {
        queue_pointer_tap(mouse_x, mouse_y);
    }
    m_impl->touch_was_pressed = touch.is_pressed;
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////

PI_HAL::PI_HAL()
{
    m_impl.reset(new Impl());
}

///////////////////////////////////////////////////////////////////////////////////////////////////

PI_HAL::~PI_HAL()
{

}

///////////////////////////////////////////////////////////////////////////////////////////////////

bool PI_HAL::init()
{
#ifndef USE_SDL
    bcm_host_init();
#endif

    if (!init_display())
    {
        LOGE("Cannot initialize display");
        return false;
    }
    if (!init_ts())
    {
        LOGE("Cannot initialize touch screen");
        return false;
    }

    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowBorderSize = 0.0f;

    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void PI_HAL::shutdown()
{
    ImGui_ImplOpenGL3_Shutdown();

    shutdown_ts();
    shutdown_display();
}

///////////////////////////////////////////////////////////////////////////////////////////////////

ImVec2 PI_HAL::get_display_size() const
{
    return ImVec2(m_impl->width, m_impl->height);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

int PI_HAL::get_refresh_rate() const
{
    return m_impl->refresh_rate;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void PI_HAL::set_backlight(float brightness)
{
    m_impl->target_backlight = std::min(std::max(brightness, 0.f), 1.f);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void* PI_HAL::get_window()
{
    return m_impl->window;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void* PI_HAL::get_main_context()
{
    return m_impl->context;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void* PI_HAL::lock_main_context()
{
    m_impl->context_mutex.lock();
    SDL_GL_MakeCurrent(m_impl->window, m_impl->context);
    return m_impl->context;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void PI_HAL::unlock_main_context()
{
    SDL_GL_MakeCurrent(m_impl->window, nullptr);
    m_impl->context_mutex.unlock();
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void PI_HAL::set_width( int w )
{
    m_impl->width = w;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void PI_HAL::set_height( int h )
{
    m_impl->height = h;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void PI_HAL::set_fullscreen( bool b )
{
    m_impl->fullscreen = b;
}

void PI_HAL::set_vsync( bool b, bool apply )
{
    m_impl->vsync = b;
    if ( apply )
    {
        const int requested_swap_interval = m_impl->vsync ? 1 : 0;
        const int swap_interval_result = SDL_GL_SetSwapInterval(requested_swap_interval);
        LOGI("SDL swap interval request={} result={} error={}",
             requested_swap_interval,
             swap_interval_result,
             swap_interval_result == 0 ? "-" : SDL_GetError());
    }

}

///////////////////////////////////////////////////////////////////////////////////////////////////

//===================================================================================
//===================================================================================
// Sets the callback used to translate pointer taps into runtime semantic actions.
void PI_HAL::set_pointer_tap_callback(std::function<void(float, float)> func)
{
    m_impl->pointer_tap_callback = std::move(func);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

//===================================================================================
//===================================================================================
// Stores one pending semantic pointer tap for runtime overlay dispatch.
void PI_HAL::queue_pointer_tap(float x, float y)
{
    m_impl->pointer_tap_pending = true;
    m_impl->pointer_tap_x = x;
    m_impl->pointer_tap_y = y;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

//===================================================================================
//===================================================================================
// Invokes the runtime tap callback once before ImGui starts the next frame.
void PI_HAL::dispatch_pending_pointer_tap()
{
    if (!m_impl->pointer_tap_pending)
    {
        return;
    }

    m_impl->pointer_tap_pending = false;
    if (m_impl->pointer_tap_callback)
    {
        m_impl->pointer_tap_callback(m_impl->pointer_tap_x, m_impl->pointer_tap_y);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////

bool PI_HAL::process()
{
    update_ts();
    return update_display();
}
