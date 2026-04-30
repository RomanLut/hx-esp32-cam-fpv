#include "gs_video_stabilization_shared.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

#include "../../components/common/Clock.h"
#include "Log.h"
#include "gs_shared_state.h"
#include "../../OpenCV/OpenCVWrapper/include/gs_vision_opencv_wrapper.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
using CreateFn = GsVisionStabilizer* (*)(const GsVisionStabilizerConfig* config);
using DestroyFn = void (*)(GsVisionStabilizer* stabilizer);
using ResetFn = void (*)(GsVisionStabilizer* stabilizer);
using EstimateFrameFn = int32_t (*)(
    GsVisionStabilizer* stabilizer,
    const GsVisionImage* input,
    GsVisionStabilizerFrameResult* result);

//===================================================================================
//===================================================================================
// Holds the lazily loaded OpenCV stabilization API and its state instance.
struct StabilizationApi
{
#if defined(_WIN32)
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif
    CreateFn create = nullptr;
    DestroyFn destroy = nullptr;
    ResetFn reset = nullptr;
    EstimateFrameFn estimate_frame = nullptr;
    GsVisionStabilizer* stabilizer = nullptr;
    GsVisionStabilizerConfig config = {};
    bool attempted = false;
    bool logged_ready = false;
    std::string error;
};

std::mutex s_stabilization_mutex;
StabilizationApi s_stabilization_api;
std::atomic<uint32_t> s_stabilization_count = 0;
std::atomic<uint32_t> s_stabilization_min_ms = 9999;
std::atomic<uint32_t> s_stabilization_max_ms = 0;
std::atomic<uint32_t> s_stabilization_stage_log_count = 0;
std::mutex s_stabilization_transform_mutex;
gs::stabilization::StabilizationTransform s_stabilization_transform;

//===================================================================================
//===================================================================================
// Converts a duration to floating-point milliseconds for stage timing logs.
float durationMs(Clock::duration duration)
{
    return std::chrono::duration<float, std::milli>(duration).count();
}

//===================================================================================
//===================================================================================
// Records one measured stabilization duration for the current stats window.
void recordStabilizationDuration(uint32_t duration_ms)
{
    s_stabilization_count.fetch_add(1);
    uint32_t current_min = s_stabilization_min_ms.load();
    while(duration_ms < current_min && !s_stabilization_min_ms.compare_exchange_weak(current_min, duration_ms))
    {
    }
    uint32_t current_max = s_stabilization_max_ms.load();
    while(duration_ms > current_max && !s_stabilization_max_ms.compare_exchange_weak(current_max, duration_ms))
    {
    }
}

//===================================================================================
//===================================================================================
// Logs a compact stabilization timing breakdown periodically for Android profiling.
void logStabilizationStageTimings(const char* source_format,
                                  int width,
                                  int height,
                                  float total_ms,
                                  float rgb565_to_rgb24_ms,
                                  float rgb24_impl_ms,
                                  float stabilized_alloc_ms,
                                  float wrapper_call_ms,
                                  float rgb24_to_rgb565_ms,
                                  const GsVisionStabilizerFrameResult& result)
{
    const uint32_t count = s_stabilization_stage_log_count.fetch_add(1) + 1;
    if(count % 30u != 0u)
    {
        return;
    }

    LOGI("Video stabilization stages format={} size={}x{} total={:.2f}ms rgb565_to_rgb24={:.2f}ms rgb24_impl={:.2f}ms stabilized_alloc={:.2f}ms wrapper_call={:.2f}ms rgb24_to_rgb565={:.2f}ms wrapper_total={:.2f}ms convert={:.2f}ms gray={:.2f}ms feature={:.2f}ms optical_flow={:.2f}ms affine={:.2f}ms first_warp={:.2f}ms zoom_warp={:.2f}ms store={:.2f}ms output={:.2f}ms tracked={} inliers={}",
         source_format,
         width,
         height,
         total_ms,
         rgb565_to_rgb24_ms,
         rgb24_impl_ms,
         stabilized_alloc_ms,
         wrapper_call_ms,
         rgb24_to_rgb565_ms,
         result.total_ms,
         result.convert_ms,
         result.gray_ms,
         result.feature_ms,
         result.optical_flow_ms,
         result.affine_ms,
         result.first_warp_ms,
         result.zoom_warp_ms,
         result.store_ms,
         result.output_ms,
         result.tracked_points,
         result.inliers);
}

//===================================================================================
//===================================================================================
// Builds the wrapper config from the persisted GS stabilization state.
GsVisionStabilizerConfig buildWrapperConfig()
{
    GsVisionStabilizerConfig config = {};
    config.roi_divisor = s_imageStabilizationState.roi_divisor;
    config.zoom_factor = s_imageStabilizationState.zoom_factor;
    config.process_variance = s_imageStabilizationState.process_var;
    config.measurement_variance = s_imageStabilizationState.measurement_var;
    config.max_corners = s_imageStabilizationState.max_corners;
    config.quality_level = s_imageStabilizationState.quality_level;
    config.min_distance = s_imageStabilizationState.min_distance;
    return config;
}

//===================================================================================
//===================================================================================
// Returns true when two wrapper configs contain the same tuning values.
bool configsEqual(const GsVisionStabilizerConfig& a, const GsVisionStabilizerConfig& b)
{
    return a.roi_divisor == b.roi_divisor &&
           a.zoom_factor == b.zoom_factor &&
           a.process_variance == b.process_variance &&
           a.measurement_variance == b.measurement_variance &&
           a.max_corners == b.max_corners &&
           a.quality_level == b.quality_level &&
           a.min_distance == b.min_distance;
}

//===================================================================================
//===================================================================================
// Recreates the wrapper stabilizer when settings changed since the instance was built.
bool ensureConfiguredLocked(StabilizationApi& api)
{
    if(api.create == nullptr || api.destroy == nullptr)
    {
        return false;
    }

    const GsVisionStabilizerConfig config = buildWrapperConfig();
    if(api.stabilizer != nullptr && configsEqual(api.config, config))
    {
        return true;
    }

    if(api.stabilizer != nullptr)
    {
        api.destroy(api.stabilizer);
        api.stabilizer = nullptr;
    }

    api.config = config;
    api.stabilizer = api.create(&api.config);
    if(api.stabilizer == nullptr)
    {
        api.error = "OpenCV wrapper failed to create stabilization state.";
        return false;
    }

    return true;
}

//===================================================================================
//===================================================================================
// Adds one possible OpenCV wrapper library name or path to the search list.
void addCandidatePath(std::vector<std::string>& candidates, const char* path)
{
    if(path != nullptr && path[0] != 0)
    {
        candidates.emplace_back(path);
    }
}

//===================================================================================
//===================================================================================
// Builds platform-specific candidate paths for loading the OpenCV wrapper.
std::vector<std::string> buildOpenCvWrapperCandidates()
{
    std::vector<std::string> candidates;
#if defined(_WIN32)
    addCandidatePath(candidates, "OpenCVWrapper.dll");
#elif defined(__ANDROID__)
    addCandidatePath(candidates, "libOpenCVWrapper.so");
#else
    addCandidatePath(candidates, "libOpenCVWrapper.so");
    addCandidatePath(candidates, "../OpenCV/OpenCVWrapper/Prebuilt/linux/arm64/libOpenCVWrapper.so");
    addCandidatePath(candidates, "../OpenCV/OpenCVWrapper/Prebuilt/linux/x64/libOpenCVWrapper.so");
    addCandidatePath(candidates, "OpenCV/OpenCVWrapper/Prebuilt/linux/arm64/libOpenCVWrapper.so");
    addCandidatePath(candidates, "OpenCV/OpenCVWrapper/Prebuilt/linux/x64/libOpenCVWrapper.so");
#endif
    return candidates;
}

//===================================================================================
//===================================================================================
// Returns a symbol pointer from the loaded OpenCV wrapper handle.
template <typename T>
T loadOpenCvSymbol(StabilizationApi& api, const char* name)
{
#if defined(_WIN32)
    return reinterpret_cast<T>(GetProcAddress(api.handle, name));
#else
    return reinterpret_cast<T>(dlsym(api.handle, name));
#endif
}

//===================================================================================
//===================================================================================
// Loads the OpenCV wrapper and creates a stabilizer state object on demand.
StabilizationApi& loadStabilizationApiLocked()
{
    if(s_stabilization_api.attempted)
    {
        return s_stabilization_api;
    }

    s_stabilization_api.attempted = true;
    const std::vector<std::string> candidates = buildOpenCvWrapperCandidates();
    for(const std::string& candidate : candidates)
    {
#if defined(_WIN32)
        s_stabilization_api.handle = LoadLibraryA(candidate.c_str());
#else
        s_stabilization_api.handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if(s_stabilization_api.handle == nullptr)
        {
            continue;
        }

        s_stabilization_api.create = loadOpenCvSymbol<CreateFn>(s_stabilization_api, "gs_vision_stabilizer_create");
        s_stabilization_api.destroy = loadOpenCvSymbol<DestroyFn>(s_stabilization_api, "gs_vision_stabilizer_destroy");
        s_stabilization_api.reset = loadOpenCvSymbol<ResetFn>(s_stabilization_api, "gs_vision_stabilizer_reset");
        s_stabilization_api.estimate_frame = loadOpenCvSymbol<EstimateFrameFn>(s_stabilization_api, "gs_vision_stabilizer_estimate_frame");

        if(s_stabilization_api.create == nullptr ||
           s_stabilization_api.destroy == nullptr ||
           s_stabilization_api.reset == nullptr ||
           s_stabilization_api.estimate_frame == nullptr)
        {
            s_stabilization_api.error = "OpenCV wrapper is missing stabilization symbols.";
            return s_stabilization_api;
        }

        if(!ensureConfiguredLocked(s_stabilization_api))
        {
            return s_stabilization_api;
        }

        return s_stabilization_api;
    }

    s_stabilization_api.error = "OpenCV wrapper library was not found.";
    return s_stabilization_api;
}
}

namespace gs::stabilization
{

//===================================================================================
//===================================================================================
// Returns true when video stabilization is enabled for this process.
bool isEnabled()
{
    return s_imageStabilizationState.enabled;
}

//===================================================================================
//===================================================================================
// Resets temporal stabilization state so the next frame becomes a new reference.
void reset()
{
    if(!isEnabled() && !s_stabilization_api.attempted)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(s_stabilization_mutex);
    StabilizationApi& api = loadStabilizationApiLocked();
    if(api.reset != nullptr && ensureConfiguredLocked(api) && api.stabilizer != nullptr)
    {
        api.reset(api.stabilizer);
    }
    {
        std::lock_guard<std::mutex> transform_lock(s_stabilization_transform_mutex);
        s_stabilization_transform = {};
    }
}

//===================================================================================
//===================================================================================
// Estimates stabilization transform for a caller-owned frame without modifying pixels.
bool estimateFrame(const uint8_t* pixels,
                   size_t size,
                   int width,
                   int height,
                   int stride,
                   GsVisionImageFormat format,
                   const char* log_format)
{
    if(!isEnabled())
    {
        std::lock_guard<std::mutex> transform_lock(s_stabilization_transform_mutex);
        s_stabilization_transform = {};
        return false;
    }

    if(pixels == nullptr ||
       size == 0 ||
       width <= 0 ||
       height <= 0 ||
       stride < width * (format == GS_VISION_IMAGE_FORMAT_RGB565 ? 2 : 3) ||
       size < static_cast<size_t>(stride) * static_cast<size_t>(height))
    {
        return false;
    }

    const Clock::time_point start = Clock::now();
    std::lock_guard<std::mutex> lock(s_stabilization_mutex);
    StabilizationApi& api = loadStabilizationApiLocked();
    if(api.estimate_frame == nullptr || !ensureConfiguredLocked(api) || api.stabilizer == nullptr)
    {
        return false;
    }

    if(!api.logged_ready)
    {
        LOGI("Video stabilization enabled using OpenCV wrapper.");
        api.logged_ready = true;
    }

    const GsVisionImage input = {
        pixels,
        width,
        height,
        stride,
        format
    };
    GsVisionStabilizerFrameResult result = {};
    if(api.estimate_frame(api.stabilizer, &input, &result) == 0)
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> transform_lock(s_stabilization_transform_mutex);
        s_stabilization_transform.enabled = result.stabilized != 0;
        s_stabilization_transform.width = width;
        s_stabilization_transform.height = height;
        s_stabilization_transform.m00 = result.transform_00;
        s_stabilization_transform.m01 = result.transform_01;
        s_stabilization_transform.m02 = result.transform_02;
        s_stabilization_transform.m10 = result.transform_10;
        s_stabilization_transform.m11 = result.transform_11;
        s_stabilization_transform.m12 = result.transform_12;
    }

    const float total_ms_float = durationMs(Clock::now() - start);
    logStabilizationStageTimings(log_format,
                                 width,
                                 height,
                                 total_ms_float,
                                 0.0f,
                                 total_ms_float,
                                 0.0f,
                                 result.total_ms,
                                 0.0f,
                                 result);
    const uint32_t duration_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
    recordStabilizationDuration(duration_ms);
    return result.stabilized != 0;
}

//===================================================================================
//===================================================================================
// Estimates stabilization transform for an RGB565 frame without modifying pixels.
bool estimateRgb565Frame(const uint8_t* pixels,
                         size_t size,
                         int width,
                         int height,
                         int stride)
{
    return estimateFrame(pixels, size, width, height, stride, GS_VISION_IMAGE_FORMAT_RGB565, "RGB565_ESTIMATE");
}

//===================================================================================
//===================================================================================
// Estimates stabilization transform for an RGB565 frame without modifying pixels.
bool estimateRgb565Frame(const std::vector<uint8_t>& pixels,
                         int width,
                         int height,
                         int stride)
{
    return estimateRgb565Frame(pixels.data(), pixels.size(), width, height, stride);
}

//===================================================================================
//===================================================================================
// Estimates stabilization transform for a caller-owned RGB24 frame without copying.
bool estimateRgbFrame(const uint8_t* pixels,
                      size_t size,
                      int width,
                      int height,
                      int stride)
{
    return estimateFrame(pixels, size, width, height, stride, GS_VISION_IMAGE_FORMAT_RGB8, "RGB24_ESTIMATE");
}

//===================================================================================
//===================================================================================
// Returns the latest transform estimated for render-time stabilization.
StabilizationTransform getLatestTransform()
{
    std::lock_guard<std::mutex> transform_lock(s_stabilization_transform_mutex);
    return s_stabilization_transform;
}

//===================================================================================
//===================================================================================
// Returns and resets stabilization timings accumulated since the previous call.
StabilizationStats consumeStats()
{
    StabilizationStats stats = {};
    stats.count = s_stabilization_count.exchange(0);
    stats.min_ms = s_stabilization_min_ms.exchange(9999);
    stats.max_ms = s_stabilization_max_ms.exchange(0);
    if(stats.count == 0)
    {
        stats.min_ms = 0;
        stats.max_ms = 0;
    }
    return stats;
}

} // namespace gs::stabilization
