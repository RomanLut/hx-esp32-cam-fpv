#include <jni.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "Log.h"
#include "android_jni_shared.h"
#include "openxr_video_bridge.h"
#include "imgui.h"
#include "gs_shared_state.h"
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_NO_PROTOTYPES
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

namespace
{

static constexpr int k_quad_width  = 1280;
static constexpr int k_quad_height = 720;
// Physical size of the head-locked quad (meters, 16:9 at 1.5m wide)
static constexpr float k_quad_size_x = 1.5f;
static constexpr float k_quad_size_y = 0.84375f;
// Distance in front of the user (VIEW space: -Z is forward)
static constexpr float k_quad_z = -1.5f;

//===================================================================================
//===================================================================================
// Runs an OpenXR frame loop on Quest, presenting a head-locked 1280x720 quad layer.
class QuestOpenXrRuntime
{
public:
    bool start(JNIEnv* env, jobject activity)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running.load())
        {
            return true;
        }

        if (env == nullptr || activity == nullptr)
        {
            LOGE("OpenXR start failed: null env/activity");
            return false;
        }

        m_activity_global = env->NewGlobalRef(activity);
        if (m_activity_global == nullptr)
        {
            LOGE("OpenXR start failed: NewGlobalRef(activity) failed");
            return false;
        }

        m_stop_requested.store(false);
        LOGI("OpenXR: start requested");
        m_thread = std::thread(&QuestOpenXrRuntime::threadMain, this);
        return true;
    }

    void stop(JNIEnv* env)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop_requested.store(true);
        LOGI("OpenXR: stop requested");
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        if (env != nullptr && m_activity_global != nullptr)
        {
            env->DeleteGlobalRef(m_activity_global);
            m_activity_global = nullptr;
        }
        m_running.store(false);
    }

private:
    typedef XrResult(XRAPI_PTR* PFN_xrGetInstanceProcAddrRaw)(XrInstance, const char*, PFN_xrVoidFunction*);

    void threadMain()
    {
        JNIEnv* env = nullptr;
        JavaVM* vm = androidGetJavaVm();
        if (vm == nullptr || vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr)
        {
            LOGE("OpenXR thread: failed to attach JVM");
            return;
        }
        LOGI("OpenXR: thread attached");
        m_running.store(true);

        initOpenXr(env);
        shutdownOpenXr();

        vm->DetachCurrentThread();
        LOGI("OpenXR: thread detached");
        m_running.store(false);
    }

    bool initOpenXr(JNIEnv* env)
    {
        auto* raw_get_proc = reinterpret_cast<PFN_xrGetInstanceProcAddrRaw>(
            dlsym(RTLD_DEFAULT, "xrGetInstanceProcAddr"));
        if (raw_get_proc != nullptr)
        {
            LOGI("OpenXR: resolved xrGetInstanceProcAddr from RTLD_DEFAULT");
        }
        else
        {
            LOGW("OpenXR: xrGetInstanceProcAddr missing in RTLD_DEFAULT, trying dlopen");
            m_loader_handle = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
            if (m_loader_handle == nullptr)
            {
                m_loader_handle = dlopen("libopenxr_loader_no_khr_init.so", RTLD_NOW | RTLD_LOCAL);
            }
            if (m_loader_handle == nullptr)
            {
                const char* dl_error = dlerror();
                LOGE("OpenXR: failed to dlopen loader: {}", dl_error != nullptr ? dl_error : "unknown");
                return false;
            }
            LOGI("OpenXR: loader opened via dlopen");
            raw_get_proc = reinterpret_cast<PFN_xrGetInstanceProcAddrRaw>(
                dlsym(m_loader_handle, "xrGetInstanceProcAddr"));
        }

        if (raw_get_proc == nullptr)
        {
            LOGE("OpenXR: xrGetInstanceProcAddr not found");
            return false;
        }
        m_xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(raw_get_proc);

        PFN_xrVoidFunction initialize_loader_fn = nullptr;
        if (raw_get_proc(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &initialize_loader_fn) == XR_SUCCESS &&
            initialize_loader_fn != nullptr)
        {
            auto xrInitializeLoaderKHRFn = reinterpret_cast<PFN_xrInitializeLoaderKHR>(initialize_loader_fn);
            XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
            loader_info.applicationVM = androidGetJavaVm();
            loader_info.applicationContext = m_activity_global;
            const XrResult loader_init_result =
                xrInitializeLoaderKHRFn(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader_info));
            if (loader_init_result != XR_SUCCESS)
            {
                LOGE("OpenXR: xrInitializeLoaderKHR failed {}", static_cast<int>(loader_init_result));
                return false;
            }
            LOGI("OpenXR: xrInitializeLoaderKHR ok");
        }
        else
        {
            LOGW("OpenXR: xrInitializeLoaderKHR not available");
        }

        XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        android_info.applicationVM = androidGetJavaVm();
        android_info.applicationActivity = m_activity_global;

        XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
        instance_info.next = &android_info;
        strcpy(instance_info.applicationInfo.applicationName, "esp32-cam-fpv-quest");
        instance_info.applicationInfo.applicationVersion = 1;
        strcpy(instance_info.applicationInfo.engineName, "esp32-cam-fpv");
        instance_info.applicationInfo.engineVersion = 1;
        instance_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);

        // Query available extensions to optionally enable the cylinder layer.
        m_cylinder_supported = false;
        {
            PFN_xrVoidFunction enum_ext_fn = nullptr;
            if (raw_get_proc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &enum_ext_fn) == XR_SUCCESS &&
                enum_ext_fn != nullptr)
            {
                auto xrEnumerateInstanceExtensionPropertiesFn =
                    reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(enum_ext_fn);
                uint32_t ext_count = 0;
                if (xrEnumerateInstanceExtensionPropertiesFn(nullptr, 0, &ext_count, nullptr) == XR_SUCCESS && ext_count > 0)
                {
                    std::vector<XrExtensionProperties> props(ext_count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
                    if (xrEnumerateInstanceExtensionPropertiesFn(nullptr, ext_count, &ext_count, props.data()) == XR_SUCCESS)
                    {
                        for (const auto& p : props)
                        {
                            if (strcmp(p.extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME) == 0)
                            {
                                m_cylinder_supported = true;
                                break;
                            }
                        }
                    }
                }
            }
            LOGI("OpenXR: cylinder layer extension {}", m_cylinder_supported ? "supported" : "not supported");
        }

        std::vector<const char*> exts;
        exts.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
        exts.push_back(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
        if (m_cylinder_supported)
        {
            exts.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
        }
        instance_info.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        instance_info.enabledExtensionNames = exts.data();

        PFN_xrVoidFunction create_instance_fn = nullptr;
        if (raw_get_proc(XR_NULL_HANDLE, "xrCreateInstance", &create_instance_fn) != XR_SUCCESS ||
            create_instance_fn == nullptr)
        {
            LOGE("OpenXR: failed to resolve xrCreateInstance");
            return false;
        }
        auto xrCreateInstanceFn = reinterpret_cast<PFN_xrCreateInstance>(create_instance_fn);
        if (xrCreateInstanceFn(&instance_info, &m_instance) != XR_SUCCESS || m_instance == XR_NULL_HANDLE)
        {
            LOGE("OpenXR: xrCreateInstance failed");
            return false;
        }
        LOGI("OpenXR: xrCreateInstance ok");

        if (!resolveInstanceFunctions())
        {
            return false;
        }

        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (m_xrGetSystem(m_instance, &system_info, &m_system_id) != XR_SUCCESS)
        {
            LOGE("OpenXR: xrGetSystem failed");
            return false;
        }
        LOGI("OpenXR: xrGetSystem ok");

        XrGraphicsRequirementsOpenGLESKHR graphics_requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        if (m_xrGetOpenGLESGraphicsRequirementsKHR(m_instance, m_system_id, &graphics_requirements) != XR_SUCCESS)
        {
            LOGE("OpenXR: xrGetOpenGLESGraphicsRequirementsKHR failed");
            return false;
        }

        if (!initEgl())
        {
            LOGE("OpenXR: EGL init failed");
            return false;
        }
        XrGraphicsBindingOpenGLESAndroidKHR gl_binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        gl_binding.display = m_egl_display;
        gl_binding.config = m_egl_config;
        gl_binding.context = m_egl_context;

        XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
        session_info.next = &gl_binding;
        session_info.systemId = m_system_id;
        if (m_xrCreateSession(m_instance, &session_info, &m_session) != XR_SUCCESS)
        {
            LOGE("OpenXR: xrCreateSession failed");
            return false;
        }
        LOGI("OpenXR: xrCreateSession ok");

        // VIEW space — quad layer will be head-locked to this space
        XrReferenceSpaceCreateInfo view_space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        view_space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        view_space_info.poseInReferenceSpace.orientation.w = 1.0f;
        if (m_xrCreateReferenceSpace(m_session, &view_space_info, &m_view_space) != XR_SUCCESS)
        {
            LOGE("OpenXR: xrCreateReferenceSpace(VIEW) failed");
            return false;
        }
        LOGI("OpenXR: VIEW space created");

        if (!initBlitPipeline())
        {
            LOGE("OpenXR: initBlitPipeline failed");
            return false;
        }

        if (!initActions())
        {
            LOGW("OpenXR: initActions failed (controller input disabled)");
        }

        if (!initQuadSwapchain())
        {
            LOGE("OpenXR: initQuadSwapchain failed");
            return false;
        }

        frameLoop();
        return true;
    }

    bool resolveInstanceFunctions()
    {
        auto load = [&](const char* name, PFN_xrVoidFunction* fn) -> bool
        {
            return m_xrGetInstanceProcAddr(m_instance, name, fn) == XR_SUCCESS && *fn != nullptr;
        };

        PFN_xrVoidFunction fn = nullptr;
        if (!load("xrDestroyInstance", &fn)) return false; m_xrDestroyInstance = reinterpret_cast<PFN_xrDestroyInstance>(fn);
        if (!load("xrGetSystem", &fn)) return false; m_xrGetSystem = reinterpret_cast<PFN_xrGetSystem>(fn);
        if (!load("xrCreateSession", &fn)) return false; m_xrCreateSession = reinterpret_cast<PFN_xrCreateSession>(fn);
        if (!load("xrDestroySession", &fn)) return false; m_xrDestroySession = reinterpret_cast<PFN_xrDestroySession>(fn);
        if (!load("xrPollEvent", &fn)) return false; m_xrPollEvent = reinterpret_cast<PFN_xrPollEvent>(fn);
        if (!load("xrBeginSession", &fn)) return false; m_xrBeginSession = reinterpret_cast<PFN_xrBeginSession>(fn);
        if (!load("xrEndSession", &fn)) return false; m_xrEndSession = reinterpret_cast<PFN_xrEndSession>(fn);
        if (!load("xrWaitFrame", &fn)) return false; m_xrWaitFrame = reinterpret_cast<PFN_xrWaitFrame>(fn);
        if (!load("xrBeginFrame", &fn)) return false; m_xrBeginFrame = reinterpret_cast<PFN_xrBeginFrame>(fn);
        if (!load("xrEndFrame", &fn)) return false; m_xrEndFrame = reinterpret_cast<PFN_xrEndFrame>(fn);
        if (!load("xrCreateReferenceSpace", &fn)) return false; m_xrCreateReferenceSpace = reinterpret_cast<PFN_xrCreateReferenceSpace>(fn);
        if (!load("xrDestroySpace", &fn)) return false; m_xrDestroySpace = reinterpret_cast<PFN_xrDestroySpace>(fn);
        if (!load("xrEnumerateSwapchainFormats", &fn)) return false; m_xrEnumerateSwapchainFormats = reinterpret_cast<PFN_xrEnumerateSwapchainFormats>(fn);
        if (!load("xrCreateSwapchain", &fn)) return false; m_xrCreateSwapchain = reinterpret_cast<PFN_xrCreateSwapchain>(fn);
        if (!load("xrDestroySwapchain", &fn)) return false; m_xrDestroySwapchain = reinterpret_cast<PFN_xrDestroySwapchain>(fn);
        if (!load("xrEnumerateSwapchainImages", &fn)) return false; m_xrEnumerateSwapchainImages = reinterpret_cast<PFN_xrEnumerateSwapchainImages>(fn);
        if (!load("xrAcquireSwapchainImage", &fn)) return false; m_xrAcquireSwapchainImage = reinterpret_cast<PFN_xrAcquireSwapchainImage>(fn);
        if (!load("xrWaitSwapchainImage", &fn)) return false; m_xrWaitSwapchainImage = reinterpret_cast<PFN_xrWaitSwapchainImage>(fn);
        if (!load("xrReleaseSwapchainImage", &fn)) return false; m_xrReleaseSwapchainImage = reinterpret_cast<PFN_xrReleaseSwapchainImage>(fn);
        if (!load("xrGetOpenGLESGraphicsRequirementsKHR", &fn)) return false; m_xrGetOpenGLESGraphicsRequirementsKHR = reinterpret_cast<PFN_xrGetOpenGLESGraphicsRequirementsKHR>(fn);
        if (!load("xrCreateActionSet", &fn)) return false; m_xrCreateActionSet = reinterpret_cast<PFN_xrCreateActionSet>(fn);
        if (!load("xrDestroyActionSet", &fn)) return false; m_xrDestroyActionSet = reinterpret_cast<PFN_xrDestroyActionSet>(fn);
        if (!load("xrCreateAction", &fn)) return false; m_xrCreateAction = reinterpret_cast<PFN_xrCreateAction>(fn);
        if (!load("xrDestroyAction", &fn)) return false; m_xrDestroyAction = reinterpret_cast<PFN_xrDestroyAction>(fn);
        if (!load("xrStringToPath", &fn)) return false; m_xrStringToPath = reinterpret_cast<PFN_xrStringToPath>(fn);
        if (!load("xrSuggestInteractionProfileBindings", &fn)) return false; m_xrSuggestInteractionProfileBindings = reinterpret_cast<PFN_xrSuggestInteractionProfileBindings>(fn);
        if (!load("xrAttachSessionActionSets", &fn)) return false; m_xrAttachSessionActionSets = reinterpret_cast<PFN_xrAttachSessionActionSets>(fn);
        if (!load("xrSyncActions", &fn)) return false; m_xrSyncActions = reinterpret_cast<PFN_xrSyncActions>(fn);
        if (!load("xrGetActionStateBoolean", &fn)) return false; m_xrGetActionStateBoolean = reinterpret_cast<PFN_xrGetActionStateBoolean>(fn);
        if (!load("xrGetActionStateFloat", &fn)) return false; m_xrGetActionStateFloat = reinterpret_cast<PFN_xrGetActionStateFloat>(fn);
        if (!load("xrGetActionStateVector2f", &fn)) return false; m_xrGetActionStateVector2f = reinterpret_cast<PFN_xrGetActionStateVector2f>(fn);
        return true;
    }

    //===================================================================================
    //===================================================================================
    // Creates the single 1280x720 swapchain used by the quad layer.
    bool initQuadSwapchain()
    {
        uint32_t format_count = 0;
        if (m_xrEnumerateSwapchainFormats(m_session, 0, &format_count, nullptr) != XR_SUCCESS || format_count == 0)
        {
            return false;
        }
        std::vector<int64_t> formats(format_count);
        if (m_xrEnumerateSwapchainFormats(m_session, format_count, &format_count, formats.data()) != XR_SUCCESS)
        {
            return false;
        }

        int64_t color_format = formats[0];
        for (const int64_t f : formats)
        {
            if (f == GL_SRGB8_ALPHA8 || f == GL_RGBA8)
            {
                color_format = f;
                break;
            }
        }

        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = color_format;
        ci.sampleCount = 1;
        ci.width = static_cast<uint32_t>(k_quad_width);
        ci.height = static_cast<uint32_t>(k_quad_height);
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;

        if (m_xrCreateSwapchain(m_session, &ci, &m_quad_swapchain.handle) != XR_SUCCESS)
        {
            return false;
        }

        uint32_t image_count = 0;
        if (m_xrEnumerateSwapchainImages(m_quad_swapchain.handle, 0, &image_count, nullptr) != XR_SUCCESS || image_count == 0)
        {
            return false;
        }
        m_quad_swapchain.images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        if (m_xrEnumerateSwapchainImages(
                m_quad_swapchain.handle,
                image_count,
                &image_count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(m_quad_swapchain.images.data())) != XR_SUCCESS)
        {
            return false;
        }

        LOGI("OpenXR: quad swapchain ready {}x{} images={} format={}",
             k_quad_width,
             k_quad_height,
             static_cast<int>(image_count),
             static_cast<int>(color_format));
        return true;
    }

    bool initEgl()
    {
        m_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (m_egl_display == EGL_NO_DISPLAY || !eglInitialize(m_egl_display, nullptr, nullptr))
        {
            return false;
        }

        const EGLint config_attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        EGLint num = 0;
        if (!eglChooseConfig(m_egl_display, config_attribs, &m_egl_config, 1, &num) || num == 0)
        {
            return false;
        }

        const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        m_egl_context = eglCreateContext(m_egl_display, m_egl_config, EGL_NO_CONTEXT, ctx_attribs);
        if (m_egl_context == EGL_NO_CONTEXT)
        {
            return false;
        }

        // Publish for the renderer's surface backend to use as a share-group
        // sibling — its texture will then be sampleable directly from this thread.
        gs::openxr::setSharedEglContext(m_egl_context);

        const EGLint surf_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        m_egl_surface = eglCreatePbufferSurface(m_egl_display, m_egl_config, surf_attribs);
        if (m_egl_surface == EGL_NO_SURFACE)
        {
            return false;
        }

        return eglMakeCurrent(m_egl_display, m_egl_surface, m_egl_surface, m_egl_context) == EGL_TRUE;
    }

    void frameLoop()
    {
        bool session_running = false;
        XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;

        while (!m_stop_requested.load())
        {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            while (m_xrPollEvent(m_instance, &event) == XR_SUCCESS)
            {
                if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
                {
                    auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                    session_state = changed->state;
                    LOGI("OpenXR: session state={}", static_cast<int>(session_state));
                    if (session_state == XR_SESSION_STATE_READY && !session_running)
                    {
                        XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
                        begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        if (m_xrBeginSession(m_session, &begin_info) == XR_SUCCESS)
                        {
                            session_running = true;
                            LOGI("OpenXR: xrBeginSession ok");
                        }
                    }
                    if (session_state == XR_SESSION_STATE_STOPPING && session_running)
                    {
                        m_xrEndSession(m_session);
                        session_running = false;
                        LOGI("OpenXR: xrEndSession");
                    }
                    if (session_state == XR_SESSION_STATE_EXITING || session_state == XR_SESSION_STATE_LOSS_PENDING)
                    {
                        m_stop_requested.store(true);
                    }
                }
                event = {XR_TYPE_EVENT_DATA_BUFFER};
            }

            if (!session_running)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState frame_state{XR_TYPE_FRAME_STATE};
            if (m_xrWaitFrame(m_session, &wait_info, &frame_state) != XR_SUCCESS)
            {
                continue;
            }

            XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
            m_xrBeginFrame(m_session, &begin_info);

            // Read controller inputs: sync each frame and emit ImGui keys on rising edges.
            syncControllerInputs(session_state);

            XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
            end_info.displayTime = frame_state.predictedDisplayTime;
            end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

            if (frame_state.shouldRender == XR_TRUE && renderQuadFrame())
            {
                const float distance = std::clamp(s_groundstation_config.screenVrDistance, 1.0f, 3.0f);
                const bool use_cylinder = m_cylinder_supported && s_groundstation_config.screenVrCurved;
                const XrCompositionLayerBaseHeader* layer = nullptr;

                if (use_cylinder)
                {
                    constexpr float kPi = 3.14159265358979323846f;
                    const float angle_deg = std::clamp(s_groundstation_config.screenVrCurvatureAngleDeg, 30.0f, 85.0f);
                    const float central_angle_rad = angle_deg * (kPi / 180.0f);
                    m_cylinder_layer = {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
                    m_cylinder_layer.space = m_view_space;
                    m_cylinder_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    m_cylinder_layer.pose.orientation.w = 1.0f;
                    m_cylinder_layer.pose.position = {0.0f, 0.0f, 0.0f};
                    m_cylinder_layer.radius = distance;
                    m_cylinder_layer.centralAngle = central_angle_rad;
                    m_cylinder_layer.aspectRatio = k_quad_size_x / k_quad_size_y;
                    m_cylinder_layer.subImage.swapchain = m_quad_swapchain.handle;
                    m_cylinder_layer.subImage.imageRect.offset = {0, 0};
                    m_cylinder_layer.subImage.imageRect.extent = {k_quad_width, k_quad_height};
                    m_cylinder_layer.subImage.imageArrayIndex = 0;
                    layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_cylinder_layer);
                }
                else
                {
                    m_quad_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                    m_quad_layer.space = m_view_space;
                    m_quad_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    m_quad_layer.pose.orientation.w = 1.0f;
                    m_quad_layer.pose.position = {0.0f, 0.0f, -distance};
                    m_quad_layer.size = {k_quad_size_x, k_quad_size_y};
                    m_quad_layer.subImage.swapchain = m_quad_swapchain.handle;
                    m_quad_layer.subImage.imageRect.offset = {0, 0};
                    m_quad_layer.subImage.imageRect.extent = {k_quad_width, k_quad_height};
                    m_quad_layer.subImage.imageArrayIndex = 0;
                    layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_quad_layer);
                }

                end_info.layerCount = 1;
                end_info.layers = &layer;
            }
            else
            {
                end_info.layerCount = 0;
                end_info.layers = nullptr;
            }

            m_xrEndFrame(m_session, &end_info);
        }
    }

    //===================================================================================
    //===================================================================================
    // Acquires a swapchain image and renders the latest GS video frame onto it.
    bool renderQuadFrame()
    {
        uint32_t image_index = 0;
        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (m_xrAcquireSwapchainImage(m_quad_swapchain.handle, &acquire_info, &image_index) != XR_SUCCESS)
        {
            return false;
        }

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait_info.timeout = XR_INFINITE_DURATION;
        if (m_xrWaitSwapchainImage(m_quad_swapchain.handle, &wait_info) != XR_SUCCESS)
        {
            return false;
        }

        // Pull the renderer's offscreen texture id (created in the shared EGL
        // group) so we can sample it directly into the swapchain — no CPU copy.
        unsigned int renderer_tex = 0;
        int renderer_w = 0;
        int renderer_h = 0;
        const bool have_renderer_tex = gs::openxr::getRendererTexture(renderer_tex, renderer_w, renderer_h);

        const GLuint tex = m_quad_swapchain.images[image_index].image;
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glViewport(0, 0, k_quad_width, k_quad_height);

        if (have_renderer_tex)
        {
            drawTextureFullscreen(static_cast<GLuint>(renderer_tex));
        }
        else
        {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);

        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        m_xrReleaseSwapchainImage(m_quad_swapchain.handle, &release_info);
        return true;
    }

    //===================================================================================
    //===================================================================================
    // Compiles the fullscreen-quad shader and VBO used to sample the renderer's
    // shared GL texture into the OpenXR swapchain image each frame.
    bool initBlitPipeline()
    {
        static const char* k_vs = R"(#version 300 es
layout(location = 0) in vec2 a_pos;
out vec2 v_uv;
void main()
{
    v_uv = vec2((a_pos.x + 1.0) * 0.5, (a_pos.y + 1.0) * 0.5);
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";
        static const char* k_fs = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
out vec4 frag;
void main() { frag = texture(u_tex, v_uv); }
)";

        auto compile = [](GLenum stage, const char* src) -> GLuint
        {
            const GLuint sh = glCreateShader(stage);
            glShaderSource(sh, 1, &src, nullptr);
            glCompileShader(sh);
            GLint ok = 0;
            glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (ok == GL_FALSE)
            {
                char log[512] = {};
                glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
                LOGE("OpenXR: shader compile failed: {}", log);
                glDeleteShader(sh);
                return 0;
            }
            return sh;
        };

        const GLuint vs = compile(GL_VERTEX_SHADER, k_vs);
        const GLuint fs = compile(GL_FRAGMENT_SHADER, k_fs);
        if (vs == 0 || fs == 0)
        {
            if (vs != 0) glDeleteShader(vs);
            if (fs != 0) glDeleteShader(fs);
            return false;
        }

        const GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (linked == GL_FALSE)
        {
            char log[512] = {};
            glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
            LOGE("OpenXR: program link failed: {}", log);
            glDeleteProgram(prog);
            return false;
        }
        m_blit.program = prog;
        m_blit.loc_tex = glGetUniformLocation(prog, "u_tex");

        static const float k_verts[] = {
            -1.0f, -1.0f,
             1.0f, -1.0f,
            -1.0f,  1.0f,
             1.0f,  1.0f,
        };
        glGenBuffers(1, &m_blit.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m_blit.vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(k_verts), k_verts, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        LOGI("OpenXR: blit pipeline ready");
        return true;
    }

    //===================================================================================
    //===================================================================================
    // Draws the given GL texture as a fullscreen quad into the currently bound FBO.
    void drawTextureFullscreen(GLuint texture)
    {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glUseProgram(m_blit.program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        if (m_blit.loc_tex >= 0)
        {
            glUniform1i(m_blit.loc_tex, 0);
        }
        glBindBuffer(GL_ARRAY_BUFFER, m_blit.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    }

    //===================================================================================
    //===================================================================================
    // Releases GL resources owned by the blit pipeline. Must be called while the
    // EGL context is still current.
    void destroyBlitPipeline()
    {
        if (m_blit.vbo != 0)
        {
            glDeleteBuffers(1, &m_blit.vbo);
            m_blit.vbo = 0;
        }
        if (m_blit.program != 0)
        {
            glDeleteProgram(m_blit.program);
            m_blit.program = 0;
        }
        m_blit.loc_tex = -1;
    }

    //===================================================================================
    //===================================================================================
    // Creates the action set, button/trigger/thumbstick actions, suggests Touch
    // controller bindings, and attaches the set to the active session.
    bool initActions()
    {
        XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strncpy(asci.actionSetName, "gameplay", sizeof(asci.actionSetName) - 1);
        std::strncpy(asci.localizedActionSetName, "Gameplay", sizeof(asci.localizedActionSetName) - 1);
        asci.priority = 0;
        if (m_xrCreateActionSet(m_instance, &asci, &m_input.action_set) != XR_SUCCESS ||
            m_input.action_set == XR_NULL_HANDLE)
        {
            LOGE("OpenXR: xrCreateActionSet failed");
            return false;
        }

        auto makeAction = [&](const char* name,
                              const char* localized,
                              XrActionType type,
                              XrAction& out) -> bool
        {
            XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
            std::strncpy(aci.actionName, name, sizeof(aci.actionName) - 1);
            std::strncpy(aci.localizedActionName, localized, sizeof(aci.localizedActionName) - 1);
            aci.actionType = type;
            return m_xrCreateAction(m_input.action_set, &aci, &out) == XR_SUCCESS && out != XR_NULL_HANDLE;
        };

        if (!makeAction("button_a", "Button A", XR_ACTION_TYPE_BOOLEAN_INPUT, m_input.action_a) ||
            !makeAction("button_b", "Button B", XR_ACTION_TYPE_BOOLEAN_INPUT, m_input.action_b) ||
            !makeAction("trigger", "Index Trigger", XR_ACTION_TYPE_FLOAT_INPUT, m_input.action_trigger) ||
            !makeAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, m_input.action_thumbstick) ||
            !makeAction("thumb_click", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT, m_input.action_thumb_click))
        {
            LOGE("OpenXR: xrCreateAction failed");
            return false;
        }

        auto stringToPath = [&](const char* s) -> XrPath
        {
            XrPath p = XR_NULL_PATH;
            m_xrStringToPath(m_instance, s, &p);
            return p;
        };

        const XrPath profile_touch = stringToPath("/interaction_profiles/oculus/touch_controller");
        if (profile_touch == XR_NULL_PATH)
        {
            LOGE("OpenXR: failed to resolve touch_controller path");
            return false;
        }

        const XrActionSuggestedBinding bindings[] = {
            { m_input.action_a,           stringToPath("/user/hand/right/input/a/click") },
            { m_input.action_a,           stringToPath("/user/hand/left/input/x/click") },
            { m_input.action_b,           stringToPath("/user/hand/right/input/b/click") },
            { m_input.action_b,           stringToPath("/user/hand/left/input/y/click") },
            { m_input.action_trigger,     stringToPath("/user/hand/right/input/trigger/value") },
            { m_input.action_trigger,     stringToPath("/user/hand/left/input/trigger/value") },
            { m_input.action_thumbstick,  stringToPath("/user/hand/right/input/thumbstick") },
            { m_input.action_thumbstick,  stringToPath("/user/hand/left/input/thumbstick") },
            { m_input.action_thumb_click, stringToPath("/user/hand/right/input/thumbstick/click") },
            { m_input.action_thumb_click, stringToPath("/user/hand/left/input/thumbstick/click") },
        };

        XrInteractionProfileSuggestedBinding spb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        spb.interactionProfile = profile_touch;
        spb.countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]);
        spb.suggestedBindings = bindings;
        if (m_xrSuggestInteractionProfileBindings(m_instance, &spb) != XR_SUCCESS)
        {
            LOGE("OpenXR: xrSuggestInteractionProfileBindings failed");
            return false;
        }

        XrSessionActionSetsAttachInfo sasai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        sasai.countActionSets = 1;
        sasai.actionSets = &m_input.action_set;
        if (m_xrAttachSessionActionSets(m_session, &sasai) != XR_SUCCESS)
        {
            LOGE("OpenXR: xrAttachSessionActionSets failed");
            return false;
        }

        m_input.attached = true;
        LOGI("OpenXR: action set attached (touch_controller)");
        return true;
    }

    //===================================================================================
    //===================================================================================
    // Polls the action set each frame and publishes ImGui keys for rising edges.
    void syncControllerInputs(int session_state)
    {
        if (!m_input.attached || m_input.action_set == XR_NULL_HANDLE)
        {
            return;
        }
        // Actions only deliver state while the session is FOCUSED.
        if (session_state != XR_SESSION_STATE_FOCUSED)
        {
            return;
        }

        XrActiveActionSet active{};
        active.actionSet = m_input.action_set;
        active.subactionPath = XR_NULL_PATH;
        XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
        sync_info.countActiveActionSets = 1;
        sync_info.activeActionSets = &active;
        if (m_xrSyncActions(m_session, &sync_info) != XR_SUCCESS)
        {
            return;
        }

        // Match Linux kernel auto-repeat defaults so held controller inputs
        // produce the same cadence as GPIO buttons routed through uinput.
        constexpr auto k_repeat_delay = std::chrono::milliseconds(250);
        constexpr auto k_repeat_period = std::chrono::milliseconds(33);
        const auto now = std::chrono::steady_clock::now();

        auto handleRepeat = [&](bool current, InputState::Repeat& r, ImGuiKey key)
        {
            if (current && !r.prev)
            {
                gs::openxr::publishImGuiKey(static_cast<int>(key));
                r.press_time = now;
                r.last_repeat = now;
            }
            else if (current && r.prev)
            {
                if (now - r.press_time >= k_repeat_delay &&
                    now - r.last_repeat >= k_repeat_period)
                {
                    gs::openxr::publishImGuiKey(static_cast<int>(key));
                    r.last_repeat = now;
                }
            }
            r.prev = current;
        };

        auto pollBool = [&](XrAction action, InputState::Repeat& r, ImGuiKey key)
        {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = action;
            XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (m_xrGetActionStateBoolean(m_session, &gi, &s) != XR_SUCCESS)
            {
                return;
            }
            const bool current = (s.isActive == XR_TRUE) && (s.currentState == XR_TRUE);
            handleRepeat(current, r, key);
        };

        auto pollFloatPress = [&](XrAction action, InputState::Repeat& r, ImGuiKey key)
        {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = action;
            XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
            if (m_xrGetActionStateFloat(m_session, &gi, &s) != XR_SUCCESS)
            {
                return;
            }
            const bool current = (s.isActive == XR_TRUE) && (s.currentState > 0.5f);
            handleRepeat(current, r, key);
        };

        pollBool(m_input.action_b, m_input.r_b, ImGuiKey_R);
        pollBool(m_input.action_a, m_input.r_a, ImGuiKey_G);
        pollBool(m_input.action_thumb_click, m_input.r_thumb_click, ImGuiKey_Enter);
        pollFloatPress(m_input.action_trigger, m_input.r_trigger, ImGuiKey_Enter);

        XrActionStateGetInfo tgi{XR_TYPE_ACTION_STATE_GET_INFO};
        tgi.action = m_input.action_thumbstick;
        XrActionStateVector2f tv{XR_TYPE_ACTION_STATE_VECTOR2F};
        bool up = false, down = false, left = false, right = false;
        if (m_xrGetActionStateVector2f(m_session, &tgi, &tv) == XR_SUCCESS && tv.isActive == XR_TRUE)
        {
            constexpr float k_thr = 0.6f;
            up    = tv.currentState.y >  k_thr;
            down  = tv.currentState.y < -k_thr;
            left  = tv.currentState.x < -k_thr;
            right = tv.currentState.x >  k_thr;
        }
        handleRepeat(up,    m_input.r_thumb_up,    ImGuiKey_UpArrow);
        handleRepeat(down,  m_input.r_thumb_down,  ImGuiKey_DownArrow);
        handleRepeat(left,  m_input.r_thumb_left,  ImGuiKey_LeftArrow);
        handleRepeat(right, m_input.r_thumb_right, ImGuiKey_RightArrow);
    }

    //===================================================================================
    //===================================================================================
    // Destroys actions and the action set. Safe to call on a partially-built input set.
    void destroyActions()
    {
        auto destroy = [&](XrAction& a)
        {
            if (a != XR_NULL_HANDLE)
            {
                m_xrDestroyAction(a);
                a = XR_NULL_HANDLE;
            }
        };
        destroy(m_input.action_a);
        destroy(m_input.action_b);
        destroy(m_input.action_trigger);
        destroy(m_input.action_thumbstick);
        destroy(m_input.action_thumb_click);
        if (m_input.action_set != XR_NULL_HANDLE)
        {
            m_xrDestroyActionSet(m_input.action_set);
            m_input.action_set = XR_NULL_HANDLE;
        }
        m_input.attached = false;
    }

    void shutdownOpenXr()
    {
        if (m_quad_swapchain.handle != XR_NULL_HANDLE)
        {
            m_xrDestroySwapchain(m_quad_swapchain.handle);
            m_quad_swapchain.handle = XR_NULL_HANDLE;
        }
        m_quad_swapchain.images.clear();

        // Release GL resources while the EGL context is still current.
        destroyBlitPipeline();

        destroyActions();

        if (m_view_space != XR_NULL_HANDLE)
        {
            m_xrDestroySpace(m_view_space);
            m_view_space = XR_NULL_HANDLE;
        }
        if (m_session != XR_NULL_HANDLE)
        {
            m_xrDestroySession(m_session);
            m_session = XR_NULL_HANDLE;
        }
        if (m_instance != XR_NULL_HANDLE)
        {
            m_xrDestroyInstance(m_instance);
            m_instance = XR_NULL_HANDLE;
        }

        if (m_egl_display != EGL_NO_DISPLAY)
        {
            eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (m_egl_surface != EGL_NO_SURFACE)
            {
                eglDestroySurface(m_egl_display, m_egl_surface);
                m_egl_surface = EGL_NO_SURFACE;
            }
            if (m_egl_context != EGL_NO_CONTEXT)
            {
                eglDestroyContext(m_egl_display, m_egl_context);
                m_egl_context = EGL_NO_CONTEXT;
            }
            eglTerminate(m_egl_display);
            m_egl_display = EGL_NO_DISPLAY;
        }

        if (m_loader_handle != nullptr)
        {
            dlclose(m_loader_handle);
            m_loader_handle = nullptr;
        }
    }

    std::mutex m_mutex;
    std::thread m_thread;
    std::atomic<bool> m_running = false;
    std::atomic<bool> m_stop_requested = false;
    jobject m_activity_global = nullptr;

    void* m_loader_handle = nullptr;

    EGLDisplay m_egl_display = EGL_NO_DISPLAY;
    EGLConfig m_egl_config = nullptr;
    EGLContext m_egl_context = EGL_NO_CONTEXT;
    EGLSurface m_egl_surface = EGL_NO_SURFACE;

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_system_id = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_view_space = XR_NULL_HANDLE;

    struct SwapchainState
    {
        XrSwapchain handle = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
    };
    SwapchainState m_quad_swapchain;
    XrCompositionLayerQuad m_quad_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerCylinderKHR m_cylinder_layer{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    bool m_cylinder_supported = false;

    struct BlitPipelineGL
    {
        GLuint program = 0;
        GLuint vbo = 0;
        GLint loc_tex = -1;
    };
    BlitPipelineGL m_blit;

    struct InputState
    {
        XrActionSet action_set = XR_NULL_HANDLE;
        XrAction action_a = XR_NULL_HANDLE;
        XrAction action_b = XR_NULL_HANDLE;
        XrAction action_trigger = XR_NULL_HANDLE;
        XrAction action_thumbstick = XR_NULL_HANDLE;
        XrAction action_thumb_click = XR_NULL_HANDLE;
        bool attached = false;
        struct Repeat
        {
            bool prev = false;
            std::chrono::steady_clock::time_point press_time{};
            std::chrono::steady_clock::time_point last_repeat{};
        };
        Repeat r_a;
        Repeat r_b;
        Repeat r_trigger;
        Repeat r_thumb_click;
        Repeat r_thumb_up;
        Repeat r_thumb_down;
        Repeat r_thumb_left;
        Repeat r_thumb_right;
    };
    InputState m_input;

    PFN_xrGetInstanceProcAddr m_xrGetInstanceProcAddr = nullptr;
    PFN_xrDestroyInstance m_xrDestroyInstance = nullptr;
    PFN_xrGetSystem m_xrGetSystem = nullptr;
    PFN_xrCreateSession m_xrCreateSession = nullptr;
    PFN_xrDestroySession m_xrDestroySession = nullptr;
    PFN_xrPollEvent m_xrPollEvent = nullptr;
    PFN_xrBeginSession m_xrBeginSession = nullptr;
    PFN_xrEndSession m_xrEndSession = nullptr;
    PFN_xrWaitFrame m_xrWaitFrame = nullptr;
    PFN_xrBeginFrame m_xrBeginFrame = nullptr;
    PFN_xrEndFrame m_xrEndFrame = nullptr;
    PFN_xrCreateReferenceSpace m_xrCreateReferenceSpace = nullptr;
    PFN_xrDestroySpace m_xrDestroySpace = nullptr;
    PFN_xrEnumerateSwapchainFormats m_xrEnumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain m_xrCreateSwapchain = nullptr;
    PFN_xrDestroySwapchain m_xrDestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages m_xrEnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage m_xrAcquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage m_xrWaitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage m_xrReleaseSwapchainImage = nullptr;
    PFN_xrGetOpenGLESGraphicsRequirementsKHR m_xrGetOpenGLESGraphicsRequirementsKHR = nullptr;
    PFN_xrCreateActionSet m_xrCreateActionSet = nullptr;
    PFN_xrDestroyActionSet m_xrDestroyActionSet = nullptr;
    PFN_xrCreateAction m_xrCreateAction = nullptr;
    PFN_xrDestroyAction m_xrDestroyAction = nullptr;
    PFN_xrStringToPath m_xrStringToPath = nullptr;
    PFN_xrSuggestInteractionProfileBindings m_xrSuggestInteractionProfileBindings = nullptr;
    PFN_xrAttachSessionActionSets m_xrAttachSessionActionSets = nullptr;
    PFN_xrSyncActions m_xrSyncActions = nullptr;
    PFN_xrGetActionStateBoolean m_xrGetActionStateBoolean = nullptr;
    PFN_xrGetActionStateFloat m_xrGetActionStateFloat = nullptr;
    PFN_xrGetActionStateVector2f m_xrGetActionStateVector2f = nullptr;
};

QuestOpenXrRuntime g_runtime;

}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_startOpenXr(JNIEnv* env, jobject /*thiz*/, jobject activity)
{
    return g_runtime.start(env, activity) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_stopOpenXr(JNIEnv* env, jobject /*thiz*/)
{
    g_runtime.stop(env);
}
