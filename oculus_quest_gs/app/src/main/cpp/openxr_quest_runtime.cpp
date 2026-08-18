#include <jni.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <algorithm>
#include <atomic>
#include <cmath>
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
#include "../../../../../components_gs/mcp/gs_mcp_server.h"
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
// Human-readable XrResult names for failures that happen before an XrInstance exists
// (where the runtime's own xrResultToString cannot be called yet). Once an instance is
// available QuestOpenXrRuntime::xrStr() prefers xrResultToString, which additionally
// names extension-specific results this table does not know about.
const char* xrResultFallbackName(XrResult r)
{
    switch (r)
    {
        case XR_SUCCESS:                                   return "XR_SUCCESS";
        case XR_TIMEOUT_EXPIRED:                           return "XR_TIMEOUT_EXPIRED";
        case XR_SESSION_LOSS_PENDING:                      return "XR_SESSION_LOSS_PENDING";
        case XR_EVENT_UNAVAILABLE:                         return "XR_EVENT_UNAVAILABLE";
        case XR_SPACE_BOUNDS_UNAVAILABLE:                  return "XR_SPACE_BOUNDS_UNAVAILABLE";
        case XR_SESSION_NOT_FOCUSED:                       return "XR_SESSION_NOT_FOCUSED";
        case XR_FRAME_DISCARDED:                           return "XR_FRAME_DISCARDED";
        case XR_ERROR_VALIDATION_FAILURE:                  return "XR_ERROR_VALIDATION_FAILURE";
        case XR_ERROR_RUNTIME_FAILURE:                     return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_OUT_OF_MEMORY:                       return "XR_ERROR_OUT_OF_MEMORY";
        case XR_ERROR_API_VERSION_UNSUPPORTED:             return "XR_ERROR_API_VERSION_UNSUPPORTED";
        case XR_ERROR_INITIALIZATION_FAILED:               return "XR_ERROR_INITIALIZATION_FAILED";
        case XR_ERROR_FUNCTION_UNSUPPORTED:                return "XR_ERROR_FUNCTION_UNSUPPORTED";
        case XR_ERROR_FEATURE_UNSUPPORTED:                 return "XR_ERROR_FEATURE_UNSUPPORTED";
        case XR_ERROR_EXTENSION_NOT_PRESENT:               return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_LIMIT_REACHED:                       return "XR_ERROR_LIMIT_REACHED";
        case XR_ERROR_SIZE_INSUFFICIENT:                   return "XR_ERROR_SIZE_INSUFFICIENT";
        case XR_ERROR_HANDLE_INVALID:                      return "XR_ERROR_HANDLE_INVALID";
        case XR_ERROR_INSTANCE_LOST:                       return "XR_ERROR_INSTANCE_LOST";
        case XR_ERROR_SESSION_RUNNING:                     return "XR_ERROR_SESSION_RUNNING";
        case XR_ERROR_SESSION_NOT_RUNNING:                 return "XR_ERROR_SESSION_NOT_RUNNING";
        case XR_ERROR_SESSION_LOST:                        return "XR_ERROR_SESSION_LOST";
        case XR_ERROR_SYSTEM_INVALID:                      return "XR_ERROR_SYSTEM_INVALID";
        case XR_ERROR_PATH_INVALID:                        return "XR_ERROR_PATH_INVALID";
        case XR_ERROR_PATH_COUNT_EXCEEDED:                 return "XR_ERROR_PATH_COUNT_EXCEEDED";
        case XR_ERROR_PATH_FORMAT_INVALID:                 return "XR_ERROR_PATH_FORMAT_INVALID";
        case XR_ERROR_PATH_UNSUPPORTED:                    return "XR_ERROR_PATH_UNSUPPORTED";
        case XR_ERROR_LAYER_INVALID:                       return "XR_ERROR_LAYER_INVALID";
        case XR_ERROR_LAYER_LIMIT_EXCEEDED:                return "XR_ERROR_LAYER_LIMIT_EXCEEDED";
        case XR_ERROR_SWAPCHAIN_RECT_INVALID:              return "XR_ERROR_SWAPCHAIN_RECT_INVALID";
        case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED:        return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
        case XR_ERROR_ACTION_TYPE_MISMATCH:                return "XR_ERROR_ACTION_TYPE_MISMATCH";
        case XR_ERROR_SESSION_NOT_READY:                   return "XR_ERROR_SESSION_NOT_READY";
        case XR_ERROR_SESSION_NOT_STOPPING:                return "XR_ERROR_SESSION_NOT_STOPPING";
        case XR_ERROR_TIME_INVALID:                        return "XR_ERROR_TIME_INVALID";
        case XR_ERROR_REFERENCE_SPACE_UNSUPPORTED:         return "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED";
        case XR_ERROR_FILE_ACCESS_ERROR:                   return "XR_ERROR_FILE_ACCESS_ERROR";
        case XR_ERROR_FILE_CONTENTS_INVALID:               return "XR_ERROR_FILE_CONTENTS_INVALID";
        case XR_ERROR_FORM_FACTOR_UNSUPPORTED:             return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE:             return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        case XR_ERROR_API_LAYER_NOT_PRESENT:               return "XR_ERROR_API_LAYER_NOT_PRESENT";
        case XR_ERROR_CALL_ORDER_INVALID:                  return "XR_ERROR_CALL_ORDER_INVALID";
        case XR_ERROR_GRAPHICS_DEVICE_INVALID:             return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
        case XR_ERROR_POSE_INVALID:                        return "XR_ERROR_POSE_INVALID";
        case XR_ERROR_INDEX_OUT_OF_RANGE:                  return "XR_ERROR_INDEX_OUT_OF_RANGE";
        case XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED: return "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED";
        case XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED:  return "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED";
        case XR_ERROR_NAME_DUPLICATED:                     return "XR_ERROR_NAME_DUPLICATED";
        case XR_ERROR_NAME_INVALID:                        return "XR_ERROR_NAME_INVALID";
        case XR_ERROR_ACTIONSET_NOT_ATTACHED:              return "XR_ERROR_ACTIONSET_NOT_ATTACHED";
        case XR_ERROR_ACTIONSETS_ALREADY_ATTACHED:         return "XR_ERROR_ACTIONSETS_ALREADY_ATTACHED";
        case XR_ERROR_LOCALIZED_NAME_DUPLICATED:           return "XR_ERROR_LOCALIZED_NAME_DUPLICATED";
        case XR_ERROR_LOCALIZED_NAME_INVALID:              return "XR_ERROR_LOCALIZED_NAME_INVALID";
        case XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING:  return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
        case XR_ERROR_RUNTIME_UNAVAILABLE:                 return "XR_ERROR_RUNTIME_UNAVAILABLE";
        case XR_ERROR_PERMISSION_INSUFFICIENT:             return "XR_ERROR_PERMISSION_INSUFFICIENT";
        default:                                           return "XR_ERROR_<unknown>";
    }
}

//===================================================================================
//===================================================================================
// Names the OpenXR session states so log lines are readable without a spec lookup.
const char* xrSessionStateName(XrSessionState s)
{
    switch (s)
    {
        case XR_SESSION_STATE_UNKNOWN:      return "UNKNOWN";
        case XR_SESSION_STATE_IDLE:         return "IDLE";
        case XR_SESSION_STATE_READY:        return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
        case XR_SESSION_STATE_STOPPING:     return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING:      return "EXITING";
        default:                            return "<unknown>";
    }
}

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
        m_session_state.store(XR_SESSION_STATE_UNKNOWN);
        // Quest controller input is unavailable until OpenXR explicitly reports FOCUSED.
        gs::mcp::setInjectedInputEnabled(false);
        LOGI("OpenXR: start requested");
        m_thread = std::thread(&QuestOpenXrRuntime::threadMain, this);
        return true;
    }

    void stop(JNIEnv* env)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop_requested.store(true);
        m_session_state.store(XR_SESSION_STATE_UNKNOWN);
        gs::mcp::setInjectedInputEnabled(false);
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

    bool isFocused() const
    {
        return m_session_state.load() == XR_SESSION_STATE_FOCUSED;
    }

private:
    typedef XrResult(XRAPI_PTR* PFN_xrGetInstanceProcAddrRaw)(XrInstance, const char*, PFN_xrVoidFunction*);

    //===================================================================================
    //===================================================================================
    // Formats an XrResult as "NAME (code)". Uses the runtime's xrResultToString once an
    // instance exists so extension-specific results are named correctly too.
    std::string xrStr(XrResult r) const
    {
        char buf[XR_MAX_RESULT_STRING_SIZE] = {};
        if (m_xrResultToString != nullptr && m_instance != XR_NULL_HANDLE &&
            m_xrResultToString(m_instance, r, buf) == XR_SUCCESS && buf[0] != '\0')
        {
            return fmt::format("{} ({})", buf, static_cast<int>(r));
        }
        return fmt::format("{} ({})", xrResultFallbackName(r), static_cast<int>(r));
    }

    // Per-call-site rate limit so a failure that repeats every frame stays visible in logcat
    // without flooding it (and pushing the interesting first occurrence out of the buffer).
    struct LogThrottle
    {
        std::chrono::steady_clock::time_point last_logged{};
        uint64_t suppressed = 0;
        uint64_t consecutive_failures = 0;
        bool ever_logged = false;
    };

    //===================================================================================
    //===================================================================================
    // Returns true when this call site should log now. The first occurrence always logs;
    // afterwards at most one line every 5 seconds, reporting how many were suppressed.
    static bool shouldLog(LogThrottle& t, uint64_t& out_suppressed)
    {
        constexpr auto k_interval = std::chrono::seconds(5);
        const auto now = std::chrono::steady_clock::now();
        ++t.consecutive_failures;
        if (!t.ever_logged || now - t.last_logged >= k_interval)
        {
            out_suppressed = t.suppressed;
            t.suppressed = 0;
            t.last_logged = now;
            t.ever_logged = true;
            return true;
        }
        ++t.suppressed;
        return false;
    }

    //===================================================================================
    //===================================================================================
    // Logs the interaction profile currently bound to each hand. Quest 3's Touch Plus
    // controllers report a different profile than Quest 2's Touch controllers, so this is
    // the line to check when controller input works on one headset but not the other.
    void logInteractionProfiles()
    {
        if (m_xrGetCurrentInteractionProfile == nullptr || m_xrPathToString == nullptr ||
            !m_input.attached)
        {
            return;
        }
        for (const char* hand : {"/user/hand/left", "/user/hand/right"})
        {
            XrPath hand_path = XR_NULL_PATH;
            if (m_xrStringToPath(m_instance, hand, &hand_path) != XR_SUCCESS)
            {
                continue;
            }
            XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
            const XrResult result = m_xrGetCurrentInteractionProfile(m_session, hand_path, &state);
            if (result != XR_SUCCESS)
            {
                LOGW("OpenXR: xrGetCurrentInteractionProfile({}) failed: {}", hand, xrStr(result));
                continue;
            }
            if (state.interactionProfile == XR_NULL_PATH)
            {
                LOGI("OpenXR: {} has no active interaction profile", hand);
                continue;
            }
            char name[XR_MAX_PATH_LENGTH] = {};
            uint32_t written = 0;
            if (m_xrPathToString(m_instance, state.interactionProfile, sizeof(name), &written, name) == XR_SUCCESS)
            {
                LOGI("OpenXR: {} bound to {}", hand, name);
            }
        }
    }

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
        gs::mcp::setInjectedInputEnabled(false);
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
            // Expected: NativeCore loads libopenxr_loader.so via System.loadLibrary, which
            // does not necessarily publish its symbols to this library's RTLD_DEFAULT search
            // scope. dlopen resolves it from the same classloader namespace.
            LOGW("OpenXR: xrGetInstanceProcAddr missing in RTLD_DEFAULT, trying dlopen");
            dlerror();
            m_loader_handle = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
            if (m_loader_handle == nullptr)
            {
                const char* dl_error = dlerror();
                LOGE("OpenXR: FATAL failed to dlopen libopenxr_loader.so: {}",
                     dl_error != nullptr ? dl_error : "unknown");
                return false;
            }
            LOGI("OpenXR: libopenxr_loader.so opened via dlopen");
            dlerror();
            raw_get_proc = reinterpret_cast<PFN_xrGetInstanceProcAddrRaw>(
                dlsym(m_loader_handle, "xrGetInstanceProcAddr"));
            if (raw_get_proc == nullptr)
            {
                const char* dl_error = dlerror();
                LOGE("OpenXR: FATAL dlsym(xrGetInstanceProcAddr) failed: {}",
                     dl_error != nullptr ? dl_error : "unknown");
            }
        }

        if (raw_get_proc == nullptr)
        {
            LOGE("OpenXR: FATAL xrGetInstanceProcAddr not found - loader unusable");
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
                LOGE("OpenXR: FATAL xrInitializeLoaderKHR failed: {}", xrStr(loader_init_result));
                return false;
            }
            LOGI("OpenXR: xrInitializeLoaderKHR ok");
        }
        else
        {
            // On Android the loader needs the VM/context before it can reach the runtime
            // broker; a loader without this entry point is not a supported configuration.
            LOGE("OpenXR: xrInitializeLoaderKHR NOT available - loader is not an Android "
                 "loader build; runtime discovery will fall back to filesystem scanning");
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

        // Query available extensions to optionally enable the cylinder layer and passthrough.
        m_cylinder_supported = false;
        m_passthrough_supported = false;
        {
            PFN_xrVoidFunction enum_ext_fn = nullptr;
            const XrResult resolve_result =
                raw_get_proc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &enum_ext_fn);
            if (resolve_result != XR_SUCCESS || enum_ext_fn == nullptr)
            {
                LOGE("OpenXR: FATAL cannot resolve xrEnumerateInstanceExtensionProperties: {}",
                     xrStr(resolve_result));
                return false;
            }

            auto xrEnumerateInstanceExtensionPropertiesFn =
                reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(enum_ext_fn);
            uint32_t ext_count = 0;
            const XrResult count_result =
                xrEnumerateInstanceExtensionPropertiesFn(nullptr, 0, &ext_count, nullptr);
            if (count_result != XR_SUCCESS)
            {
                // This is where a runtime that could not be located surfaces: the loader has
                // no runtime to forward the call to.
                LOGE("OpenXR: FATAL xrEnumerateInstanceExtensionProperties(count) failed: {} - "
                     "no usable OpenXR runtime was found on this device", xrStr(count_result));
                return false;
            }
            if (ext_count == 0)
            {
                LOGE("OpenXR: FATAL runtime reports zero instance extensions");
                return false;
            }

            std::vector<XrExtensionProperties> props(ext_count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
            const XrResult enum_result =
                xrEnumerateInstanceExtensionPropertiesFn(nullptr, ext_count, &ext_count, props.data());
            if (enum_result != XR_SUCCESS)
            {
                LOGE("OpenXR: FATAL xrEnumerateInstanceExtensionProperties(enumerate) failed: {}",
                     xrStr(enum_result));
                return false;
            }

            // Full list is logged once: when a headset generation behaves differently this is
            // usually the first place the difference is visible.
            LOGI("OpenXR: runtime exposes {} instance extensions", ext_count);
            for (const auto& p : props)
            {
                LOGI("OpenXR:   ext {} v{}", p.extensionName, p.extensionVersion);
                if (strcmp(p.extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME) == 0)
                {
                    m_cylinder_supported = true;
                }
                else if (strcmp(p.extensionName, XR_FB_PASSTHROUGH_EXTENSION_NAME) == 0)
                {
                    m_passthrough_supported = true;
                }
                else if (strcmp(p.extensionName, "XR_META_touch_controller_plus") == 0)
                {
                    m_touch_plus_supported = true;
                }
            }
            LOGI("OpenXR: cylinder layer extension {}", m_cylinder_supported ? "supported" : "not supported");
            LOGI("OpenXR: passthrough extension {}", m_passthrough_supported ? "supported" : "not supported");
            LOGI("OpenXR: touch_controller_plus extension {}", m_touch_plus_supported ? "supported" : "not supported");
        }

        std::vector<const char*> exts;
        exts.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
        exts.push_back(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
        if (m_cylinder_supported)
        {
            exts.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
        }
        if (m_passthrough_supported)
        {
            exts.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
        }
        if (m_touch_plus_supported)
        {
            // Quest 3 ships Touch Plus controllers. The runtime maps them onto the legacy
            // oculus/touch_controller profile, but enabling the extension lets us suggest
            // bindings against the native profile as well.
            exts.push_back("XR_META_touch_controller_plus");
        }
        instance_info.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        instance_info.enabledExtensionNames = exts.data();
        for (const char* e : exts)
        {
            LOGI("OpenXR: enabling extension {}", e);
        }

        PFN_xrVoidFunction create_instance_fn = nullptr;
        const XrResult resolve_create_result =
            raw_get_proc(XR_NULL_HANDLE, "xrCreateInstance", &create_instance_fn);
        if (resolve_create_result != XR_SUCCESS || create_instance_fn == nullptr)
        {
            LOGE("OpenXR: FATAL failed to resolve xrCreateInstance: {}", xrStr(resolve_create_result));
            return false;
        }
        auto xrCreateInstanceFn = reinterpret_cast<PFN_xrCreateInstance>(create_instance_fn);
        const XrResult create_instance_result = xrCreateInstanceFn(&instance_info, &m_instance);
        if (create_instance_result != XR_SUCCESS || m_instance == XR_NULL_HANDLE)
        {
            LOGE("OpenXR: FATAL xrCreateInstance failed: {} (requested apiVersion {}.{}.{}, {} extensions)",
                 xrStr(create_instance_result),
                 XR_VERSION_MAJOR(instance_info.applicationInfo.apiVersion),
                 XR_VERSION_MINOR(instance_info.applicationInfo.apiVersion),
                 XR_VERSION_PATCH(instance_info.applicationInfo.apiVersion),
                 instance_info.enabledExtensionCount);
            return false;
        }
        LOGI("OpenXR: xrCreateInstance ok");

        if (!resolveInstanceFunctions())
        {
            return false;
        }

        // Identify the runtime up front — this single line tells us which runtime build a
        // remote bug report was produced on.
        if (m_xrGetInstanceProperties != nullptr)
        {
            XrInstanceProperties inst_props{XR_TYPE_INSTANCE_PROPERTIES};
            const XrResult props_result = m_xrGetInstanceProperties(m_instance, &inst_props);
            if (props_result == XR_SUCCESS)
            {
                LOGI("OpenXR: runtime '{}' version {}.{}.{}",
                     inst_props.runtimeName,
                     XR_VERSION_MAJOR(inst_props.runtimeVersion),
                     XR_VERSION_MINOR(inst_props.runtimeVersion),
                     XR_VERSION_PATCH(inst_props.runtimeVersion));
            }
            else
            {
                LOGW("OpenXR: xrGetInstanceProperties failed: {}", xrStr(props_result));
            }
        }

        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        const XrResult get_system_result = m_xrGetSystem(m_instance, &system_info, &m_system_id);
        if (get_system_result != XR_SUCCESS)
        {
            LOGE("OpenXR: FATAL xrGetSystem(HEAD_MOUNTED_DISPLAY) failed: {}", xrStr(get_system_result));
            return false;
        }
        LOGI("OpenXR: xrGetSystem ok systemId={}", static_cast<unsigned long long>(m_system_id));

        querySystemProperties();

        XrGraphicsRequirementsOpenGLESKHR graphics_requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        const XrResult gfx_result =
            m_xrGetOpenGLESGraphicsRequirementsKHR(m_instance, m_system_id, &graphics_requirements);
        if (gfx_result != XR_SUCCESS)
        {
            LOGE("OpenXR: FATAL xrGetOpenGLESGraphicsRequirementsKHR failed: {}", xrStr(gfx_result));
            return false;
        }
        LOGI("OpenXR: GLES requirement min {}.{}.{} max {}.{}.{}",
             XR_VERSION_MAJOR(graphics_requirements.minApiVersionSupported),
             XR_VERSION_MINOR(graphics_requirements.minApiVersionSupported),
             XR_VERSION_PATCH(graphics_requirements.minApiVersionSupported),
             XR_VERSION_MAJOR(graphics_requirements.maxApiVersionSupported),
             XR_VERSION_MINOR(graphics_requirements.maxApiVersionSupported),
             XR_VERSION_PATCH(graphics_requirements.maxApiVersionSupported));

        if (!initEgl(graphics_requirements.minApiVersionSupported))
        {
            LOGE("OpenXR: FATAL EGL init failed");
            return false;
        }
        XrGraphicsBindingOpenGLESAndroidKHR gl_binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        gl_binding.display = m_egl_display;
        gl_binding.config = m_egl_config;
        gl_binding.context = m_egl_context;

        XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
        session_info.next = &gl_binding;
        session_info.systemId = m_system_id;
        const XrResult create_session_result = m_xrCreateSession(m_instance, &session_info, &m_session);
        if (create_session_result != XR_SUCCESS)
        {
            LOGE("OpenXR: FATAL xrCreateSession failed: {} (EGL display={} config={} context={})",
                 xrStr(create_session_result),
                 fmt::ptr(m_egl_display), fmt::ptr(m_egl_config), fmt::ptr(m_egl_context));
            return false;
        }
        LOGI("OpenXR: xrCreateSession ok");

        // VIEW space — quad layer will be head-locked to this space
        XrReferenceSpaceCreateInfo view_space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        view_space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        view_space_info.poseInReferenceSpace.orientation.w = 1.0f;
        const XrResult view_space_result =
            m_xrCreateReferenceSpace(m_session, &view_space_info, &m_view_space);
        if (view_space_result != XR_SUCCESS)
        {
            LOGE("OpenXR: FATAL xrCreateReferenceSpace(VIEW) failed: {}", xrStr(view_space_result));
            return false;
        }
        LOGI("OpenXR: VIEW space created");

        if (!initBlitPipeline())
        {
            LOGE("OpenXR: FATAL initBlitPipeline failed");
            return false;
        }

        if (!initActions())
        {
            LOGW("OpenXR: initActions failed (controller input disabled)");
        }

        if (!initQuadSwapchain())
        {
            LOGE("OpenXR: FATAL initQuadSwapchain failed");
            return false;
        }

        // Passthrough is created lazily by the frame loop the first time the user actually
        // enables it (see ensurePassthroughCreated). Creating it here ran passthrough objects
        // before xrBeginSession on every launch even though the feature defaults to off.

        frameLoop();
        return true;
    }

    //===================================================================================
    //===================================================================================
    // Logs system identity and queries the FB passthrough capability bits. The extension
    // merely being present in the instance extension list does not mean this system supports
    // passthrough, and COLOR capability is what distinguishes Quest 3 from Quest 2.
    void querySystemProperties()
    {
        if (m_xrGetSystemProperties == nullptr)
        {
            LOGW("OpenXR: xrGetSystemProperties unavailable; skipping system capability query");
            return;
        }

        XrSystemPassthroughProperties2FB pt_props{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
        XrSystemProperties sys_props{XR_TYPE_SYSTEM_PROPERTIES};
        if (m_passthrough_supported)
        {
            sys_props.next = &pt_props;
        }

        const XrResult result = m_xrGetSystemProperties(m_instance, m_system_id, &sys_props);
        if (result != XR_SUCCESS)
        {
            LOGW("OpenXR: xrGetSystemProperties failed: {}", xrStr(result));
            return;
        }

        LOGI("OpenXR: system '{}' vendorId={} maxLayers={} maxSwapchain={}x{} orientationTracking={} positionTracking={}",
             sys_props.systemName,
             sys_props.vendorId,
             sys_props.graphicsProperties.maxLayerCount,
             sys_props.graphicsProperties.maxSwapchainImageWidth,
             sys_props.graphicsProperties.maxSwapchainImageHeight,
             sys_props.trackingProperties.orientationTracking == XR_TRUE ? 1 : 0,
             sys_props.trackingProperties.positionTracking == XR_TRUE ? 1 : 0);

        if (sys_props.graphicsProperties.maxSwapchainImageWidth < static_cast<uint32_t>(k_quad_width) ||
            sys_props.graphicsProperties.maxSwapchainImageHeight < static_cast<uint32_t>(k_quad_height))
        {
            LOGE("OpenXR: system max swapchain {}x{} is smaller than the required quad {}x{}",
                 sys_props.graphicsProperties.maxSwapchainImageWidth,
                 sys_props.graphicsProperties.maxSwapchainImageHeight,
                 k_quad_width, k_quad_height);
        }

        if (m_passthrough_supported)
        {
            m_passthrough_capable = (pt_props.capabilities & XR_PASSTHROUGH_CAPABILITY_BIT_FB) != 0;
            m_passthrough_color_capable = (pt_props.capabilities & XR_PASSTHROUGH_CAPABILITY_COLOR_BIT_FB) != 0;
            LOGI("OpenXR: passthrough capabilities=0x{:08X} supported={} color={} layerDepth={}",
                 static_cast<unsigned>(pt_props.capabilities),
                 m_passthrough_capable ? 1 : 0,
                 m_passthrough_color_capable ? 1 : 0,
                 (pt_props.capabilities & XR_PASSTHROUGH_CAPABILITY_LAYER_DEPTH_BIT_FB) != 0 ? 1 : 0);

            if (!m_passthrough_capable)
            {
                LOGW("OpenXR: XR_FB_passthrough is present but this system reports no passthrough "
                     "capability; disabling passthrough");
                m_passthrough_supported = false;
            }
        }
    }

    //===================================================================================
    //===================================================================================
    // Creates the passthrough feature object and its layer on first use. Both are created in
    // the same run state (started) so the layer is never live against a paused feature.
    // Called from the frame loop, i.e. always after xrBeginSession.
    bool ensurePassthroughCreated()
    {
        if (!m_passthrough_supported || !m_passthrough_capable)
        {
            return false;
        }
        if (m_passthrough != XR_NULL_HANDLE && m_passthrough_layer != XR_NULL_HANDLE)
        {
            return true;
        }
        if (m_passthrough_create_failed)
        {
            return false;
        }

        XrPassthroughCreateInfoFB pt_info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        pt_info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
        const XrResult pt_result = m_xrCreatePassthroughFB(m_session, &pt_info, &m_passthrough);
        if (pt_result != XR_SUCCESS)
        {
            LOGE("OpenXR: xrCreatePassthroughFB failed: {}; disabling passthrough", xrStr(pt_result));
            m_passthrough = XR_NULL_HANDLE;
            m_passthrough_create_failed = true;
            return false;
        }

        XrPassthroughLayerCreateInfoFB layer_info{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
        layer_info.passthrough = m_passthrough;
        layer_info.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
        layer_info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
        const XrResult layer_result =
            m_xrCreatePassthroughLayerFB(m_session, &layer_info, &m_passthrough_layer);
        if (layer_result != XR_SUCCESS)
        {
            LOGE("OpenXR: xrCreatePassthroughLayerFB failed: {}; disabling passthrough", xrStr(layer_result));
            m_xrDestroyPassthroughFB(m_passthrough);
            m_passthrough = XR_NULL_HANDLE;
            m_passthrough_layer = XR_NULL_HANDLE;
            m_passthrough_create_failed = true;
            return false;
        }

        // Both objects were created with the running bit set.
        m_passthrough_running = true;
        m_passthrough_layer_running = true;
        m_passthrough_level_applied = 255;
        LOGI("OpenXR: passthrough created (color capable={})", m_passthrough_color_capable ? 1 : 0);
        return true;
    }

    bool resolveInstanceFunctions()
    {
        auto load = [&](const char* name, PFN_xrVoidFunction* fn) -> bool
        {
            *fn = nullptr;
            const XrResult result = m_xrGetInstanceProcAddr(m_instance, name, fn);
            if (result != XR_SUCCESS || *fn == nullptr)
            {
                LOGE("OpenXR: FATAL cannot resolve required function '{}': {}", name, xrStr(result));
                return false;
            }
            return true;
        };

        PFN_xrVoidFunction fn = nullptr;

        // Resolved first so every later failure can be reported with the runtime's own
        // result names rather than the static fallback table.
        if (m_xrGetInstanceProcAddr(m_instance, "xrResultToString", &fn) == XR_SUCCESS && fn != nullptr)
        {
            m_xrResultToString = reinterpret_cast<PFN_xrResultToString>(fn);
        }
        else
        {
            LOGW("OpenXR: xrResultToString unavailable; result codes will use the built-in table");
        }

        if (!load("xrGetInstanceProperties", &fn)) return false; m_xrGetInstanceProperties = reinterpret_cast<PFN_xrGetInstanceProperties>(fn);
        if (!load("xrGetSystemProperties", &fn)) return false; m_xrGetSystemProperties = reinterpret_cast<PFN_xrGetSystemProperties>(fn);
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
        if (!load("xrPathToString", &fn)) return false; m_xrPathToString = reinterpret_cast<PFN_xrPathToString>(fn);
        if (!load("xrGetCurrentInteractionProfile", &fn)) return false; m_xrGetCurrentInteractionProfile = reinterpret_cast<PFN_xrGetCurrentInteractionProfile>(fn);
        if (!load("xrSuggestInteractionProfileBindings", &fn)) return false; m_xrSuggestInteractionProfileBindings = reinterpret_cast<PFN_xrSuggestInteractionProfileBindings>(fn);
        if (!load("xrAttachSessionActionSets", &fn)) return false; m_xrAttachSessionActionSets = reinterpret_cast<PFN_xrAttachSessionActionSets>(fn);
        if (!load("xrSyncActions", &fn)) return false; m_xrSyncActions = reinterpret_cast<PFN_xrSyncActions>(fn);
        if (!load("xrGetActionStateBoolean", &fn)) return false; m_xrGetActionStateBoolean = reinterpret_cast<PFN_xrGetActionStateBoolean>(fn);
        if (!load("xrGetActionStateFloat", &fn)) return false; m_xrGetActionStateFloat = reinterpret_cast<PFN_xrGetActionStateFloat>(fn);
        if (!load("xrGetActionStateVector2f", &fn)) return false; m_xrGetActionStateVector2f = reinterpret_cast<PFN_xrGetActionStateVector2f>(fn);

        if (m_passthrough_supported)
        {
            bool ok = true;
            auto load_pt = [&](const char* name, PFN_xrVoidFunction* outFn) -> bool
            {
                *outFn = nullptr;
                const XrResult result = m_xrGetInstanceProcAddr(m_instance, name, outFn);
                if (result != XR_SUCCESS || *outFn == nullptr)
                {
                    LOGW("OpenXR: cannot resolve passthrough function '{}': {}", name, xrStr(result));
                    ok = false;
                    return false;
                }
                return true;
            };

            PFN_xrVoidFunction pfn = nullptr;
            if (load_pt("xrCreatePassthroughFB", &pfn))        m_xrCreatePassthroughFB = reinterpret_cast<PFN_xrCreatePassthroughFB>(pfn);
            if (load_pt("xrDestroyPassthroughFB", &pfn))       m_xrDestroyPassthroughFB = reinterpret_cast<PFN_xrDestroyPassthroughFB>(pfn);
            if (load_pt("xrPassthroughStartFB", &pfn))         m_xrPassthroughStartFB = reinterpret_cast<PFN_xrPassthroughStartFB>(pfn);
            if (load_pt("xrPassthroughPauseFB", &pfn))         m_xrPassthroughPauseFB = reinterpret_cast<PFN_xrPassthroughPauseFB>(pfn);
            if (load_pt("xrCreatePassthroughLayerFB", &pfn))   m_xrCreatePassthroughLayerFB = reinterpret_cast<PFN_xrCreatePassthroughLayerFB>(pfn);
            if (load_pt("xrDestroyPassthroughLayerFB", &pfn))  m_xrDestroyPassthroughLayerFB = reinterpret_cast<PFN_xrDestroyPassthroughLayerFB>(pfn);
            if (load_pt("xrPassthroughLayerSetStyleFB", &pfn)) m_xrPassthroughLayerSetStyleFB = reinterpret_cast<PFN_xrPassthroughLayerSetStyleFB>(pfn);
            // Layer pause/resume: needed so the layer's run state can be kept in step with the
            // feature object rather than being left running forever after the first disable.
            if (load_pt("xrPassthroughLayerPauseFB", &pfn))    m_xrPassthroughLayerPauseFB = reinterpret_cast<PFN_xrPassthroughLayerPauseFB>(pfn);
            if (load_pt("xrPassthroughLayerResumeFB", &pfn))   m_xrPassthroughLayerResumeFB = reinterpret_cast<PFN_xrPassthroughLayerResumeFB>(pfn);
            if (!ok)
            {
                LOGW("OpenXR: failed to resolve passthrough functions; disabling passthrough");
                m_passthrough_supported = false;
            }
        }
        return true;
    }

    //===================================================================================
    //===================================================================================
    // Creates the single 1280x720 swapchain used by the quad layer.
    bool initQuadSwapchain()
    {
        uint32_t format_count = 0;
        const XrResult count_result =
            m_xrEnumerateSwapchainFormats(m_session, 0, &format_count, nullptr);
        if (count_result != XR_SUCCESS)
        {
            LOGE("OpenXR: xrEnumerateSwapchainFormats(count) failed: {}", xrStr(count_result));
            return false;
        }
        if (format_count == 0)
        {
            LOGE("OpenXR: runtime reports zero supported swapchain formats");
            return false;
        }
        std::vector<int64_t> formats(format_count);
        const XrResult formats_result =
            m_xrEnumerateSwapchainFormats(m_session, format_count, &format_count, formats.data());
        if (formats_result != XR_SUCCESS)
        {
            LOGE("OpenXR: xrEnumerateSwapchainFormats(enumerate) failed: {}", xrStr(formats_result));
            return false;
        }

        // Prefer sRGB, then linear RGBA8. Anything else is used only as a last resort and is
        // called out, since colour will not be correct.
        int64_t color_format = formats[0];
        bool preferred_found = false;
        for (const int64_t f : formats)
        {
            if (f == GL_SRGB8_ALPHA8 || f == GL_RGBA8)
            {
                color_format = f;
                preferred_found = true;
                break;
            }
        }
        {
            std::string format_list;
            for (const int64_t f : formats)
            {
                format_list += fmt::format("0x{:X} ", static_cast<unsigned long long>(f));
            }
            LOGI("OpenXR: {} swapchain formats offered: {}", format_count, format_list);
        }
        if (!preferred_found)
        {
            LOGW("OpenXR: neither GL_SRGB8_ALPHA8 nor GL_RGBA8 offered; falling back to 0x{:X}",
                 static_cast<unsigned long long>(color_format));
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

        const XrResult create_result = m_xrCreateSwapchain(m_session, &ci, &m_quad_swapchain.handle);
        if (create_result != XR_SUCCESS)
        {
            LOGE("OpenXR: xrCreateSwapchain({}x{}, format 0x{:X}) failed: {}",
                 k_quad_width, k_quad_height,
                 static_cast<unsigned long long>(color_format), xrStr(create_result));
            return false;
        }

        uint32_t image_count = 0;
        const XrResult image_count_result =
            m_xrEnumerateSwapchainImages(m_quad_swapchain.handle, 0, &image_count, nullptr);
        if (image_count_result != XR_SUCCESS)
        {
            LOGE("OpenXR: xrEnumerateSwapchainImages(count) failed: {}", xrStr(image_count_result));
            return false;
        }
        if (image_count == 0)
        {
            LOGE("OpenXR: swapchain reports zero images");
            return false;
        }
        m_quad_swapchain.images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        const XrResult images_result = m_xrEnumerateSwapchainImages(
            m_quad_swapchain.handle,
            image_count,
            &image_count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(m_quad_swapchain.images.data()));
        if (images_result != XR_SUCCESS)
        {
            LOGE("OpenXR: xrEnumerateSwapchainImages(enumerate) failed: {}", xrStr(images_result));
            return false;
        }

        LOGI("OpenXR: quad swapchain ready {}x{} images={} format=0x{:X}",
             k_quad_width,
             k_quad_height,
             static_cast<int>(image_count),
             static_cast<unsigned long long>(color_format));
        return true;
    }

    // Creates this thread's EGL context. min_api_version is the runtime's reported minimum
    // OpenGL ES version from xrGetOpenGLESGraphicsRequirementsKHR; the created context is
    // checked against it rather than the result being discarded.
    bool initEgl(XrVersion min_api_version)
    {
        m_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (m_egl_display == EGL_NO_DISPLAY)
        {
            LOGE("OpenXR: eglGetDisplay(EGL_DEFAULT_DISPLAY) failed: 0x{:X}", eglGetError());
            return false;
        }
        EGLint egl_major = 0;
        EGLint egl_minor = 0;
        if (eglInitialize(m_egl_display, &egl_major, &egl_minor) != EGL_TRUE)
        {
            LOGE("OpenXR: eglInitialize failed: 0x{:X}", eglGetError());
            m_egl_display = EGL_NO_DISPLAY;
            return false;
        }
        m_egl_display_initialized = true;
        LOGI("OpenXR: EGL {}.{} initialized", egl_major, egl_minor);

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
        if (eglChooseConfig(m_egl_display, config_attribs, &m_egl_config, 1, &num) != EGL_TRUE || num == 0)
        {
            LOGE("OpenXR: eglChooseConfig found no ES3 pbuffer config (num={}): 0x{:X}",
                 num, eglGetError());
            return false;
        }

        const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        m_egl_context = eglCreateContext(m_egl_display, m_egl_config, EGL_NO_CONTEXT, ctx_attribs);
        if (m_egl_context == EGL_NO_CONTEXT)
        {
            LOGE("OpenXR: eglCreateContext(ES3) failed: 0x{:X}", eglGetError());
            return false;
        }

        const EGLint surf_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        m_egl_surface = eglCreatePbufferSurface(m_egl_display, m_egl_config, surf_attribs);
        if (m_egl_surface == EGL_NO_SURFACE)
        {
            LOGE("OpenXR: eglCreatePbufferSurface failed: 0x{:X}", eglGetError());
            return false;
        }

        if (eglMakeCurrent(m_egl_display, m_egl_surface, m_egl_surface, m_egl_context) != EGL_TRUE)
        {
            LOGE("OpenXR: eglMakeCurrent failed: 0x{:X}", eglGetError());
            return false;
        }

        // The context exists now, so the runtime's minimum can actually be checked.
        GLint gl_major = 0;
        GLint gl_minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &gl_major);
        glGetIntegerv(GL_MINOR_VERSION, &gl_minor);
        const char* gl_version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* gl_renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        LOGI("OpenXR: GL context ES {}.{} - '{}' on '{}'",
             gl_major, gl_minor,
             gl_version != nullptr ? gl_version : "?",
             gl_renderer != nullptr ? gl_renderer : "?");

        const XrVersion created_version =
            XR_MAKE_VERSION(static_cast<uint64_t>(gl_major), static_cast<uint64_t>(gl_minor), 0);
        if (created_version < min_api_version)
        {
            // Not fatal by itself — xrCreateSession is the authority — but it is the first
            // thing to look at if session creation then fails on a new headset generation.
            LOGE("OpenXR: created GLES context {}.{} is BELOW the runtime minimum {}.{}.{}; "
                 "xrCreateSession may reject it",
                 gl_major, gl_minor,
                 XR_VERSION_MAJOR(min_api_version),
                 XR_VERSION_MINOR(min_api_version),
                 XR_VERSION_PATCH(min_api_version));
        }

        // Publish for the renderer's surface backend to use as a share-group sibling — its
        // texture will then be sampleable directly from this thread. Published only after the
        // context is fully current and validated, so the renderer never shares a half-built
        // context.
        gs::openxr::setSharedEglContext(m_egl_context);
        LOGI("OpenXR: shared EGL context published for renderer share group");
        return true;
    }

    void frameLoop()
    {
        bool session_running = false;
        XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
        m_session_state.store(session_state);

        while (!m_stop_requested.load())
        {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            for (;;)
            {
                const XrResult poll_result = m_xrPollEvent(m_instance, &event);
                if (poll_result == XR_EVENT_UNAVAILABLE)
                {
                    break;
                }
                if (poll_result != XR_SUCCESS)
                {
                    LOGE("OpenXR: xrPollEvent failed: {}; stopping frame loop", xrStr(poll_result));
                    m_stop_requested.store(true);
                    break;
                }

                switch (event.type)
                {
                    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                    {
                        auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                        session_state = changed->state;
                        m_session_state.store(session_state);
                        // Horizon overlays and system dialogs move OpenXR out of FOCUSED.
                        // Reject MCP keys too, so automation cannot do what the user cannot.
                        gs::mcp::setInjectedInputEnabled(session_state == XR_SESSION_STATE_FOCUSED);
                        LOGI("OpenXR: session state={} ({})",
                             xrSessionStateName(session_state), static_cast<int>(session_state));
                        if (session_state == XR_SESSION_STATE_READY && !session_running)
                        {
                            XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
                            begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                            const XrResult begin_result = m_xrBeginSession(m_session, &begin_info);
                            if (begin_result == XR_SUCCESS)
                            {
                                session_running = true;
                                LOGI("OpenXR: xrBeginSession ok");
                            }
                            else
                            {
                                // Previously silent: the loop would spin forever rendering
                                // nothing with no indication why.
                                LOGE("OpenXR: FATAL xrBeginSession(PRIMARY_STEREO) failed: {}; "
                                     "no frames will be submitted", xrStr(begin_result));
                                m_stop_requested.store(true);
                            }
                        }
                        if (session_state == XR_SESSION_STATE_STOPPING && session_running)
                        {
                            const XrResult end_result = m_xrEndSession(m_session);
                            session_running = false;
                            if (end_result == XR_SUCCESS)
                            {
                                LOGI("OpenXR: xrEndSession ok");
                            }
                            else
                            {
                                LOGE("OpenXR: xrEndSession failed: {}", xrStr(end_result));
                            }
                        }
                        if (session_state == XR_SESSION_STATE_EXITING ||
                            session_state == XR_SESSION_STATE_LOSS_PENDING)
                        {
                            LOGI("OpenXR: session {} - stopping frame loop",
                                 xrSessionStateName(session_state));
                            m_stop_requested.store(true);
                        }
                        break;
                    }
                    case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    {
                        // The runtime is going away. Continuing to call into the instance past
                        // this point is undefined behaviour.
                        auto* lost = reinterpret_cast<XrEventDataInstanceLossPending*>(&event);
                        LOGE("OpenXR: INSTANCE_LOSS_PENDING at time {}; tearing down session",
                             static_cast<long long>(lost->lossTime));
                        m_stop_requested.store(true);
                        break;
                    }
                    case XR_TYPE_EVENT_DATA_EVENTS_LOST:
                    {
                        auto* lost = reinterpret_cast<XrEventDataEventsLost*>(&event);
                        LOGW("OpenXR: {} events lost (event queue overflow)", lost->lostEventCount);
                        break;
                    }
                    case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                    {
                        LOGI("OpenXR: interaction profile changed");
                        logInteractionProfiles();
                        break;
                    }
                    case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                    {
                        auto* ref = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&event);
                        LOGI("OpenXR: reference space {} change pending",
                             static_cast<int>(ref->referenceSpaceType));
                        break;
                    }
                    default:
                        LOGI("OpenXR: unhandled event type {}", static_cast<int>(event.type));
                        break;
                }
                event = {XR_TYPE_EVENT_DATA_BUFFER};
            }

            if (m_stop_requested.load())
            {
                break;
            }

            if (!session_running)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState frame_state{XR_TYPE_FRAME_STATE};
            const XrResult wait_result = m_xrWaitFrame(m_session, &wait_info, &frame_state);
            if (wait_result != XR_SUCCESS)
            {
                uint64_t skipped = 0;
                if (shouldLog(m_log_wait_frame, skipped))
                {
                    LOGE("OpenXR: xrWaitFrame failed: {} ({} similar suppressed)",
                         xrStr(wait_result), skipped);
                }
                if (wait_result == XR_ERROR_SESSION_LOST || wait_result == XR_ERROR_INSTANCE_LOST)
                {
                    m_stop_requested.store(true);
                }
                continue;
            }

            XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
            const XrResult begin_frame_result = m_xrBeginFrame(m_session, &begin_info);
            if (begin_frame_result != XR_SUCCESS && begin_frame_result != XR_FRAME_DISCARDED)
            {
                uint64_t skipped = 0;
                if (shouldLog(m_log_begin_frame, skipped))
                {
                    LOGE("OpenXR: xrBeginFrame failed: {} ({} similar suppressed)",
                         xrStr(begin_frame_result), skipped);
                }
                if (begin_frame_result == XR_ERROR_SESSION_LOST || begin_frame_result == XR_ERROR_INSTANCE_LOST)
                {
                    m_stop_requested.store(true);
                }
                continue;
            }

            // Read controller inputs: sync each frame and emit ImGui keys on rising edges.
            syncControllerInputs(session_state);

            XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
            end_info.displayTime = frame_state.predictedDisplayTime;
            end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

            // Reconcile passthrough state with current settings. The feature object and its
            // layer are created on first use and are always driven together, so the layer is
            // never live against a paused feature object.
            const uint8_t pt_level = std::min<uint8_t>(s_groundstation_config.screenVrPassthroughLevel, 7);
            bool pt_active = false;
            if (pt_level > 0 && m_passthrough_supported && m_passthrough_capable)
            {
                pt_active = ensurePassthroughCreated();
            }

            if (m_passthrough != XR_NULL_HANDLE)
            {
                if (pt_active && !m_passthrough_running)
                {
                    const XrResult start_result = m_xrPassthroughStartFB(m_passthrough);
                    if (start_result == XR_SUCCESS)
                    {
                        m_passthrough_running = true;
                        m_passthrough_level_applied = 255;
                        LOGI("OpenXR: passthrough started");
                    }
                    else
                    {
                        LOGE("OpenXR: xrPassthroughStartFB failed: {}", xrStr(start_result));
                        pt_active = false;
                    }
                }
                else if (!pt_active && m_passthrough_running)
                {
                    const XrResult pause_result = m_xrPassthroughPauseFB(m_passthrough);
                    if (pause_result != XR_SUCCESS)
                    {
                        LOGE("OpenXR: xrPassthroughPauseFB failed: {}", xrStr(pause_result));
                    }
                    m_passthrough_running = false;
                    LOGI("OpenXR: passthrough paused");
                }

                // Keep the layer's run state in step with the feature object.
                if (pt_active && !m_passthrough_layer_running && m_xrPassthroughLayerResumeFB != nullptr)
                {
                    const XrResult resume_result = m_xrPassthroughLayerResumeFB(m_passthrough_layer);
                    if (resume_result == XR_SUCCESS)
                    {
                        m_passthrough_layer_running = true;
                    }
                    else
                    {
                        LOGE("OpenXR: xrPassthroughLayerResumeFB failed: {}", xrStr(resume_result));
                    }
                }
                else if (!pt_active && m_passthrough_layer_running && m_xrPassthroughLayerPauseFB != nullptr)
                {
                    const XrResult layer_pause_result = m_xrPassthroughLayerPauseFB(m_passthrough_layer);
                    if (layer_pause_result != XR_SUCCESS)
                    {
                        LOGE("OpenXR: xrPassthroughLayerPauseFB failed: {}", xrStr(layer_pause_result));
                    }
                    m_passthrough_layer_running = false;
                }

                if (m_passthrough_running && pt_level != m_passthrough_level_applied)
                {
                    static constexpr float kPassthroughOpacities[] = { 0.0f, 0.02f, 0.05f, 0.10f, 0.20f, 0.50f, 0.75f, 1.00f };
                    XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB};
                    style.textureOpacityFactor = kPassthroughOpacities[pt_level];
                    style.edgeColor = {0.0f, 0.0f, 0.0f, 0.0f};
                    const XrResult style_result = m_xrPassthroughLayerSetStyleFB(m_passthrough_layer, &style);
                    if (style_result == XR_SUCCESS)
                    {
                        m_passthrough_level_applied = pt_level;
                        LOGI("OpenXR: passthrough opacity level {} ({:.2f})",
                             pt_level, kPassthroughOpacities[pt_level]);
                    }
                    else
                    {
                        LOGE("OpenXR: xrPassthroughLayerSetStyleFB failed: {}", xrStr(style_result));
                    }
                }
            }

            if (frame_state.shouldRender == XR_TRUE && renderQuadFrame())
            {
                const float distance = std::clamp(s_groundstation_config.screenVrDistance, 1.0f, 3.0f);
                const bool use_cylinder = m_cylinder_supported && s_groundstation_config.screenVrCurved;
                constexpr float kPi = 3.14159265358979323846f;
                const float tilt_rad = std::clamp(s_groundstation_config.screenVrTiltDeg, -20.0f, 20.0f) * (kPi / 180.0f);
                const float tilt_qx = std::sin(tilt_rad * 0.5f);
                const float tilt_qw = std::cos(tilt_rad * 0.5f);
                const XrCompositionLayerBaseHeader* layer = nullptr;

                if (use_cylinder)
                {
                    const float angle_deg = std::clamp(s_groundstation_config.screenVrCurvatureAngleDeg, 30.0f, 85.0f);
                    const float central_angle_rad = angle_deg * (kPi / 180.0f);
                    m_cylinder_layer = {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
                    m_cylinder_layer.space = m_view_space;
                    m_cylinder_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    m_cylinder_layer.pose.orientation = {tilt_qx, 0.0f, 0.0f, tilt_qw};
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
                    m_quad_layer.pose.orientation = {tilt_qx, 0.0f, 0.0f, tilt_qw};
                    m_quad_layer.pose.position = {0.0f, 0.0f, -distance};
                    m_quad_layer.size = {k_quad_size_x, k_quad_size_y};
                    m_quad_layer.subImage.swapchain = m_quad_swapchain.handle;
                    m_quad_layer.subImage.imageRect.offset = {0, 0};
                    m_quad_layer.subImage.imageRect.extent = {k_quad_width, k_quad_height};
                    m_quad_layer.subImage.imageArrayIndex = 0;
                    layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_quad_layer);
                }

                const XrCompositionLayerBaseHeader* layers[2] = { nullptr, nullptr };
                uint32_t layer_count = 0;
                // Passthrough must be the bottom-most layer so the video panel composites
                // over it.
                if (pt_active && m_passthrough_running && m_passthrough_layer != XR_NULL_HANDLE)
                {
                    m_passthrough_layer_composition = {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
                    m_passthrough_layer_composition.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    m_passthrough_layer_composition.space = XR_NULL_HANDLE;
                    m_passthrough_layer_composition.layerHandle = m_passthrough_layer;
                    layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_passthrough_layer_composition);
                }
                layers[layer_count++] = layer;
                end_info.layerCount = layer_count;
                end_info.layers = layers;
                endFrameChecked(end_info);
            }
            else
            {
                end_info.layerCount = 0;
                end_info.layers = nullptr;
                endFrameChecked(end_info);
            }
        }

        m_session_state.store(XR_SESSION_STATE_UNKNOWN);
        LOGI("OpenXR: frame loop exited");
    }

    //===================================================================================
    //===================================================================================
    // Submits a frame and reports failures. xrEndFrame is where a malformed layer set is
    // rejected, so a silent failure here means a permanently black panel with no clue why.
    void endFrameChecked(const XrFrameEndInfo& end_info)
    {
        const XrResult result = m_xrEndFrame(m_session, &end_info);
        if (result == XR_SUCCESS)
        {
            m_log_end_frame.consecutive_failures = 0;
            return;
        }
        uint64_t skipped = 0;
        if (shouldLog(m_log_end_frame, skipped))
        {
            LOGE("OpenXR: xrEndFrame failed: {} (layerCount={}, {} similar suppressed)",
                 xrStr(result), end_info.layerCount, skipped);
        }
        if (result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST)
        {
            LOGE("OpenXR: session/instance lost in xrEndFrame; stopping frame loop");
            m_stop_requested.store(true);
        }
    }

    //===================================================================================
    //===================================================================================
    // Acquires a swapchain image and renders the latest GS video frame onto it.
    bool renderQuadFrame()
    {
        uint32_t image_index = 0;
        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        const XrResult acquire_result =
            m_xrAcquireSwapchainImage(m_quad_swapchain.handle, &acquire_info, &image_index);
        if (acquire_result != XR_SUCCESS)
        {
            uint64_t skipped = 0;
            if (shouldLog(m_log_acquire, skipped))
            {
                LOGE("OpenXR: xrAcquireSwapchainImage failed: {} ({} similar suppressed)",
                     xrStr(acquire_result), skipped);
            }
            return false;
        }

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait_info.timeout = XR_INFINITE_DURATION;
        const XrResult wait_result = m_xrWaitSwapchainImage(m_quad_swapchain.handle, &wait_info);
        if (wait_result != XR_SUCCESS)
        {
            uint64_t skipped = 0;
            if (shouldLog(m_log_swapchain_wait, skipped))
            {
                LOGE("OpenXR: xrWaitSwapchainImage failed: {} ({} similar suppressed)",
                     xrStr(wait_result), skipped);
            }
            // The image was acquired but not waited on. Releasing it returns it to the
            // swapchain; without this the pool drains after a handful of failures and every
            // later acquire fails permanently, leaving a black panel.
            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            const XrResult release_result =
                m_xrReleaseSwapchainImage(m_quad_swapchain.handle, &release_info);
            if (release_result != XR_SUCCESS)
            {
                LOGE("OpenXR: xrReleaseSwapchainImage (after wait failure) failed: {}",
                     xrStr(release_result));
            }
            return false;
        }

        // Pull the renderer's offscreen texture id (created in the shared EGL
        // group) so we can sample it directly into the swapchain — no CPU copy.
        unsigned int renderer_tex = 0;
        int renderer_w = 0;
        int renderer_h = 0;
        bool have_renderer_tex = gs::openxr::getRendererTexture(renderer_tex, renderer_w, renderer_h);

        // A texture id from outside this share group names a different object here. Sampling
        // it would show garbage or fault the GPU, so fall back to a black panel instead.
        if (have_renderer_tex && !gs::openxr::isRendererContextShared())
        {
            uint64_t skipped = 0;
            if (shouldLog(m_log_unshared, skipped))
            {
                LOGE("OpenXR: renderer texture {} published from a NON-SHARED GL context; "
                     "refusing to sample it ({} similar suppressed)", renderer_tex, skipped);
            }
            have_renderer_tex = false;
        }

        // Wait for the renderer thread's writes to that texture to be visible to this
        // context. The two threads use separate contexts in one share group, so a producer
        // side glFlush alone does not order the consumer's sampling against them.
        gs::openxr::waitForRendererFence();

        const GLuint tex = m_quad_swapchain.images[image_index].image;
        if (m_blit_fbo == 0)
        {
            // Allocated once instead of per frame; the swapchain image is re-attached each
            // frame because the acquired index rotates.
            glGenFramebuffers(1, &m_blit_fbo);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, m_blit_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

        const GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (fbo_status != GL_FRAMEBUFFER_COMPLETE)
        {
            uint64_t skipped = 0;
            if (shouldLog(m_log_fbo, skipped))
            {
                LOGE("OpenXR: swapchain FBO incomplete: 0x{:X} (image {} tex {}, {} similar suppressed)",
                     static_cast<unsigned>(fbo_status), image_index, static_cast<unsigned>(tex), skipped);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            m_xrReleaseSwapchainImage(m_quad_swapchain.handle, &release_info);
            return false;
        }

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

        // The compositor reads this image after release; make sure our draws are submitted.
        glFlush();

        const GLenum gl_error = glGetError();
        if (gl_error != GL_NO_ERROR)
        {
            uint64_t skipped = 0;
            if (shouldLog(m_log_gl_error, skipped))
            {
                LOGE("OpenXR: GL error 0x{:X} while blitting to swapchain ({} similar suppressed)",
                     static_cast<unsigned>(gl_error), skipped);
            }
        }

        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const XrResult release_result =
            m_xrReleaseSwapchainImage(m_quad_swapchain.handle, &release_info);
        if (release_result != XR_SUCCESS)
        {
            uint64_t skipped = 0;
            if (shouldLog(m_log_release, skipped))
            {
                LOGE("OpenXR: xrReleaseSwapchainImage failed: {} ({} similar suppressed)",
                     xrStr(release_result), skipped);
            }
            return false;
        }
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
        if (m_blit_fbo != 0)
        {
            glDeleteFramebuffers(1, &m_blit_fbo);
            m_blit_fbo = 0;
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
        const XrResult action_set_result = m_xrCreateActionSet(m_instance, &asci, &m_input.action_set);
        if (action_set_result != XR_SUCCESS || m_input.action_set == XR_NULL_HANDLE)
        {
            LOGE("OpenXR: xrCreateActionSet failed: {}", xrStr(action_set_result));
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
            const XrResult result = m_xrCreateAction(m_input.action_set, &aci, &out);
            if (result != XR_SUCCESS || out == XR_NULL_HANDLE)
            {
                LOGE("OpenXR: xrCreateAction('{}') failed: {}", name, xrStr(result));
                return false;
            }
            return true;
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
        const XrResult suggest_result = m_xrSuggestInteractionProfileBindings(m_instance, &spb);
        if (suggest_result != XR_SUCCESS)
        {
            LOGE("OpenXR: xrSuggestInteractionProfileBindings(oculus/touch_controller) failed: {}",
                 xrStr(suggest_result));
            return false;
        }

        // Quest 3 ships Touch Plus controllers. Meta's runtime maps them onto the legacy
        // profile above, but suggesting against the native profile as well means the bindings
        // survive if that compatibility mapping ever stops being applied. The button and
        // thumbstick component paths are identical, so the same binding list is reused.
        if (m_touch_plus_supported)
        {
            const XrPath profile_touch_plus =
                stringToPath("/interaction_profiles/meta/touch_controller_plus");
            if (profile_touch_plus != XR_NULL_PATH)
            {
                XrInteractionProfileSuggestedBinding plus_spb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
                plus_spb.interactionProfile = profile_touch_plus;
                plus_spb.countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]);
                plus_spb.suggestedBindings = bindings;
                const XrResult plus_result = m_xrSuggestInteractionProfileBindings(m_instance, &plus_spb);
                if (plus_result == XR_SUCCESS)
                {
                    LOGI("OpenXR: suggested bindings for meta/touch_controller_plus");
                }
                else
                {
                    // Non-fatal: the legacy profile above still covers Touch Plus via the
                    // runtime's compatibility mapping.
                    LOGW("OpenXR: xrSuggestInteractionProfileBindings(meta/touch_controller_plus) "
                         "failed: {}", xrStr(plus_result));
                }
            }
            else
            {
                LOGW("OpenXR: could not resolve meta/touch_controller_plus profile path");
            }
        }

        XrSessionActionSetsAttachInfo sasai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        sasai.countActionSets = 1;
        sasai.actionSets = &m_input.action_set;
        const XrResult attach_result = m_xrAttachSessionActionSets(m_session, &sasai);
        if (attach_result != XR_SUCCESS)
        {
            LOGE("OpenXR: xrAttachSessionActionSets failed: {}", xrStr(attach_result));
            return false;
        }

        m_input.attached = true;
        LOGI("OpenXR: action set attached (touch_controller{})",
             m_touch_plus_supported ? " + touch_controller_plus" : "");
        logInteractionProfiles();
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
        LOGI("OpenXR: shutting down");
        if (m_quad_swapchain.handle != XR_NULL_HANDLE)
        {
            m_xrDestroySwapchain(m_quad_swapchain.handle);
            m_quad_swapchain.handle = XR_NULL_HANDLE;
        }
        m_quad_swapchain.images.clear();

        // Release GL resources while the EGL context is still current.
        destroyBlitPipeline();

        destroyActions();

        if (m_passthrough_layer != XR_NULL_HANDLE && m_xrDestroyPassthroughLayerFB != nullptr)
        {
            if (m_passthrough_layer_running && m_xrPassthroughLayerPauseFB != nullptr)
            {
                m_xrPassthroughLayerPauseFB(m_passthrough_layer);
                m_passthrough_layer_running = false;
            }
            m_xrDestroyPassthroughLayerFB(m_passthrough_layer);
            m_passthrough_layer = XR_NULL_HANDLE;
        }
        if (m_passthrough != XR_NULL_HANDLE && m_xrDestroyPassthroughFB != nullptr)
        {
            if (m_passthrough_running && m_xrPassthroughPauseFB != nullptr)
            {
                m_xrPassthroughPauseFB(m_passthrough);
                m_passthrough_running = false;
            }
            m_xrDestroyPassthroughFB(m_passthrough);
            m_passthrough = XR_NULL_HANDLE;
        }

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

        // The renderer must stop treating our context as its share parent before we destroy
        // it, otherwise it would keep publishing texture ids into a dead share group.
        gs::openxr::setSharedEglContext(nullptr);

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
            // Deliberately no eglTerminate: eglGetDisplay(EGL_DEFAULT_DISPLAY) is the
            // process-wide display shared with the renderer thread and the Android UI.
            // Terminating it here marked their contexts and surfaces for destruction too,
            // which is exactly the kind of teardown the compositor faults on.
            m_egl_display = EGL_NO_DISPLAY;
            m_egl_display_initialized = false;
        }

        if (m_loader_handle != nullptr)
        {
            dlclose(m_loader_handle);
            m_loader_handle = nullptr;
        }
        LOGI("OpenXR: shutdown complete");
    }

    std::mutex m_mutex;
    std::thread m_thread;
    std::atomic<bool> m_running = false;
    std::atomic<bool> m_stop_requested = false;
    std::atomic<int> m_session_state = XR_SESSION_STATE_UNKNOWN;
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
    XrCompositionLayerPassthroughFB m_passthrough_layer_composition{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    bool m_cylinder_supported = false;
    bool m_passthrough_supported = false;   // XR_FB_passthrough present in the extension list
    bool m_passthrough_capable = false;     // system actually reports passthrough capability
    bool m_passthrough_color_capable = false;
    bool m_passthrough_create_failed = false;
    bool m_touch_plus_supported = false;
    XrPassthroughFB m_passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB m_passthrough_layer = XR_NULL_HANDLE;
    bool m_passthrough_running = false;
    bool m_passthrough_layer_running = false;
    uint8_t m_passthrough_level_applied = 255;
    PFN_xrCreatePassthroughFB m_xrCreatePassthroughFB = nullptr;
    PFN_xrDestroyPassthroughFB m_xrDestroyPassthroughFB = nullptr;
    PFN_xrPassthroughStartFB m_xrPassthroughStartFB = nullptr;
    PFN_xrPassthroughPauseFB m_xrPassthroughPauseFB = nullptr;
    PFN_xrCreatePassthroughLayerFB m_xrCreatePassthroughLayerFB = nullptr;
    PFN_xrDestroyPassthroughLayerFB m_xrDestroyPassthroughLayerFB = nullptr;
    PFN_xrPassthroughLayerSetStyleFB m_xrPassthroughLayerSetStyleFB = nullptr;
    PFN_xrPassthroughLayerPauseFB m_xrPassthroughLayerPauseFB = nullptr;
    PFN_xrPassthroughLayerResumeFB m_xrPassthroughLayerResumeFB = nullptr;

    // Rate limiters for per-frame failure paths.
    LogThrottle m_log_wait_frame;
    LogThrottle m_log_begin_frame;
    LogThrottle m_log_end_frame;
    LogThrottle m_log_acquire;
    LogThrottle m_log_swapchain_wait;
    LogThrottle m_log_release;
    LogThrottle m_log_fbo;
    LogThrottle m_log_gl_error;
    LogThrottle m_log_unshared;

    bool m_egl_display_initialized = false;

    struct BlitPipelineGL
    {
        GLuint program = 0;
        GLuint vbo = 0;
        GLint loc_tex = -1;
    };
    BlitPipelineGL m_blit;
    // FBO used to render into the acquired swapchain image; created once and reused.
    GLuint m_blit_fbo = 0;

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
    PFN_xrResultToString m_xrResultToString = nullptr;
    PFN_xrGetInstanceProperties m_xrGetInstanceProperties = nullptr;
    PFN_xrGetSystemProperties m_xrGetSystemProperties = nullptr;
    PFN_xrGetCurrentInteractionProfile m_xrGetCurrentInteractionProfile = nullptr;
    PFN_xrPathToString m_xrPathToString = nullptr;
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
Java_com_esp32camfpv_questgs_QuestOpenXr_startOpenXr(JNIEnv* env, jobject /*thiz*/, jobject activity)
{
    return g_runtime.start(env, activity) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_questgs_QuestOpenXr_stopOpenXr(JNIEnv* env, jobject /*thiz*/)
{
    g_runtime.stop(env);
}

//===================================================================================
//===================================================================================
// Reports whether Quest currently grants controller-input focus to this OpenXR session.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_questgs_QuestOpenXr_isOpenXrFocused(JNIEnv* /*env*/, jobject /*thiz*/)
{
    return g_runtime.isFocused() ? JNI_TRUE : JNI_FALSE;
}
