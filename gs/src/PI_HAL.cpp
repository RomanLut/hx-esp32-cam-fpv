#include "main.h"

#include "PI_HAL.h"
#include "Log.h"

#define USE_SDL
//#define USE_MANGA_SCREEN2

//#define USE_BOUBLE_BUFFER

#include <fstream>
#include <future>
#include <atomic>
#include <mutex>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "Clock.h"

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

#ifdef TEST_LATENCY
extern "C"
{
#include "pigpio.h"
}
#endif

extern uint8_t s_font_droid_sans[];

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

#ifdef TEST_LATENCY

bool PI_HAL::init_pigpio()
{
    if (m_impl->pigpio_is_isitialized)
        return true;

    LOGI("Initializing pigpio");
    if (gpioCfgClock(2, PI_CLOCK_PCM, 0) < 0 ||
            gpioCfgPermissions(static_cast<uint64_t>(-1)))
    {
        LOGE("Cannot configure pigpio");
        return false;
    }
    if (gpioInitialise() < 0)
    {
        LOGE("Cannot init pigpio");
        return false;
    }

    m_impl->pigpio_is_isitialized = true;

    return true;
}

// ///////////////////////////////////////////////////////////////////////////////////////////////////

void PI_HAL::shutdown_pigpio()
{
    if (m_impl->pigpio_is_isitialized)
        gpioTerminate();

    m_impl->pigpio_is_isitialized = false;
}
#endif
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
    io.DisplaySize.x = m_impl->width;
    io.DisplaySize.y = m_impl->height;


#endif
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static GLuint       g_VideoTexture;
void PI_HAL::set_video_channel(unsigned int id)
{
    g_VideoTexture = id;   
}

bool PI_HAL::init_display_sdl()
{
    // Setup SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
    {
        printf("Error: %s\n", SDL_GetError());
        return -1;
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
        // Desired display mode
        SDL_DisplayMode desiredMode;
        desiredMode.w = m_impl->width;
        desiredMode.h = m_impl->height;
        desiredMode.format = 0;  // Format 0 means any format
        desiredMode.refresh_rate = 0;  // Refresh rate 0 means any refresh rate
        desiredMode.driverdata = 0;  // Driverdata should be 0

        // Closest display mode found
        SDL_DisplayMode closestMode;

        if (SDL_GetClosestDisplayMode(0, &desiredMode, &closestMode)) 
        {
            printf("Display mode:");
            printf("  Width: %d\n", closestMode.w);
            printf("  Height: %d\n", closestMode.h);
            printf("  Refresh Rate: %d\n", closestMode.refresh_rate);
            printf("  Pixel Format: %s\n", SDL_GetPixelFormatName(closestMode.format));            

            m_impl->width = closestMode.w;
            m_impl->height = closestMode.h;

            SDL_WindowFlags window_flags = (SDL_WindowFlags)(
                SDL_WINDOW_FULLSCREEN | 
                SDL_WINDOW_OPENGL | 
                SDL_WINDOW_SHOWN | 
                SDL_WINDOW_BORDERLESS );
            m_impl->window = SDL_CreateWindow("esp32-cam-fpv", 0, 0, m_impl->width, m_impl->height, window_flags);

            if (SDL_SetWindowDisplayMode(m_impl->window, &closestMode) != 0) 
            {
                printf("SDL_SetWindowDisplayMode Error: %s\n", SDL_GetError());
                SDL_DestroyWindow(m_impl->window);
                SDL_Quit();
                return false;
            }
        }
        else
        {
            printf("Can not find videomode %dx%d", m_impl->width, m_impl->height);
        }
    }
    else
    {
        SDL_DisplayMode mode;
        int res = SDL_GetCurrentDisplayMode(0, &mode);
        if ( (m_impl->width > (uint32_t)mode.w) || (m_impl->height > (uint32_t)mode.h) )
        {
            m_impl->width = mode.w;
            m_impl->height = mode.h;
        }

        SDL_WindowFlags window_flags = (SDL_WindowFlags)(
            SDL_WINDOW_OPENGL | 
            SDL_WINDOW_SHOWN | 
            SDL_WINDOW_RESIZABLE | 
            SDL_WINDOW_ALLOW_HIGHDPI);
        m_impl->window = SDL_CreateWindow("esp32-cam-fpv", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, m_impl->width, m_impl->height, window_flags);
    }

    printf("width:%d height:%d\n",m_impl->width,m_impl->height);

    SDL_GLContext gl_context = SDL_GL_CreateContext(m_impl->window);
    SDL_GL_MakeCurrent(m_impl->window, gl_context);
    SDL_GL_SetSwapInterval(m_impl->vsync ? 1 : 0 ); // Enable vsync

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
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

///////////////////////////////////////////////////////////////////////////////////////////////////

bool PI_HAL::update_display()
{
    SDL_Event event;
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    while (SDL_PollEvent(&event))
    {
        ImGui_ImplSDL2_ProcessEvent(&event);
        switch(event.type){
        case SDL_FINGERMOTION:
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
        {
            SDL_TouchFingerEvent& ev = *(SDL_TouchFingerEvent*)&event;
            //io.MousePos = ImVec2(ev.x * m_impl->width, ev.y * m_impl->height);
            //io.MouseDown[0] = event.type == SDL_FINGERUP ? false : true;
            if(event.type == SDL_FINGERMOTION || event.type == SDL_FINGERDOWN ){
                io.MouseDown[0] = true;
            }
        }
        break;
        case SDL_QUIT:
            shutdown_display();
            break;
        }
    }

    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(get_display_size());
    ImGui::SetNextWindowBgAlpha(0);
    ImGui::Begin("", nullptr, ImGuiWindowFlags_NoTitleBar | 
                            ImGuiWindowFlags_NoResize | 
                            ImGuiWindowFlags_NoMove | 
                            ImGuiWindowFlags_NoScrollbar | 
                            ImGuiWindowFlags_NoCollapse | 
                            ImGuiWindowFlags_NoInputs);
    for(auto &func:render_callbacks){
        func();
    }

    int x1, y1, x2, y2;
    calculateLetterBoxAndBorder(m_impl->width,m_impl->height, x1, y1, x2, y2);

    ImGui::GetWindowDrawList()->AddImage((ImTextureID)g_VideoTexture,ImVec2(x1,y1),ImVec2(x2,y2));

    ImGui::End();
    
    // Rendering
    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(0,0,0,1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(m_impl->window);
    
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

    // Update buttons
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDown[0] = touch.is_pressed;

    // Update mouse position
    io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
    float mouse_x = touch.y;
    float mouse_y = s_height - touch.x;
    io.MousePos = ImVec2(mouse_x, mouse_y);
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

#ifdef TEST_LATENCY
    if (!init_pigpio())
    {
        LOGE("Cannot initialize pigpio");
        return false;
    }
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
    //io.Fonts->AddFontFromMemoryTTF(s_font_droid_sans, 16, 16.f);
    io.Fonts->Build();

    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void PI_HAL::shutdown()
{
    ImGui_ImplOpenGL3_Shutdown();

#ifdef TEST_LATENCY
    shutdown_pigpio();
#endif

    shutdown_ts();
    shutdown_display();
}

///////////////////////////////////////////////////////////////////////////////////////////////////

ImVec2 PI_HAL::get_display_size() const
{
    return ImVec2(m_impl->width, m_impl->height);
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
        SDL_GL_SetSwapInterval(m_impl->vsync ? 1 : 0 ); // Enable vsync
    }

}

///////////////////////////////////////////////////////////////////////////////////////////////////

bool PI_HAL::process()
{
    update_ts();
    return update_display();
}

