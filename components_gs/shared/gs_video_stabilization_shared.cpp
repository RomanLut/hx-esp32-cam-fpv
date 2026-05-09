#include "gs_video_stabilization_shared.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>

#include "../../components/common/Clock.h"
#include "Log.h"
#include "gs_shared_state.h"
#include "gs_video_layout_shared.h"
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
using PrepareFrameFeaturesFn = int32_t (*)(
    GsVisionStabilizer* stabilizer,
    const GsVisionImage* input,
    float* feature_ms);

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
    PrepareFrameFeaturesFn prepare_frame_features = nullptr;
    GsVisionStabilizer* stabilizer = nullptr;
    GsVisionStabilizerConfig config = {};
    bool attempted = false;
    bool logged_ready = false;
    std::string error;
};

std::mutex s_stabilization_mutex;
StabilizationApi s_stabilization_api;
std::atomic<uint32_t> s_stabilization_count = 0;
std::atomic<uint32_t> s_stabilization_feature_last_ms = 0;
std::atomic<uint32_t> s_stabilization_feature_max_ms = 0;
std::atomic<uint32_t> s_stabilization_motion_last_ms = 0;
std::atomic<uint32_t> s_stabilization_motion_max_ms = 0;
std::mutex s_motion_estimate_mutex;
gs::stabilization::StabilizationMotionEstimate s_latest_motion_estimate;
std::mutex s_render_trajectory_state_mutex;
gs::stabilization::RenderTrajectoryState s_render_trajectory_state;
uint32_t s_last_frame_id = 0;
int s_last_frame_width = 0;
int s_last_frame_height = 0;
int s_last_frame_stride = 0;
GsVisionImageFormat s_last_frame_format = GS_VISION_IMAGE_FORMAT_RGB8;
bool s_last_frame_id_valid = false;

//===================================================================================
//===================================================================================
// Returns bytes per pixel for OpenCV wrapper image formats used in GS.
int bytesPerPixelForFormat(GsVisionImageFormat format)
{
    switch (format)
    {
        case GS_VISION_IMAGE_FORMAT_GRAY8:
            return 1;
        case GS_VISION_IMAGE_FORMAT_BGR8:
        case GS_VISION_IMAGE_FORMAT_RGB8:
            return 3;
        case GS_VISION_IMAGE_FORMAT_BGRA8:
        case GS_VISION_IMAGE_FORMAT_RGBA8:
            return 4;
        case GS_VISION_IMAGE_FORMAT_RGB565:
            return 2;
        default:
            return 0;
    }
}

//===================================================================================
//===================================================================================
// Records one measured stabilization feature and motion duration for the current stats window.
void recordStabilizationDurations(uint32_t feature_ms, uint32_t motion_ms)
{
    s_stabilization_count.fetch_add(1);
    s_stabilization_feature_last_ms.store(feature_ms);
    s_stabilization_motion_last_ms.store(motion_ms);
    uint32_t current_feature_max = s_stabilization_feature_max_ms.load();
    while(feature_ms > current_feature_max &&
          !s_stabilization_feature_max_ms.compare_exchange_weak(current_feature_max, feature_ms))
    {
    }
    uint32_t current_motion_max = s_stabilization_motion_max_ms.load();
    while(motion_ms > current_motion_max &&
          !s_stabilization_motion_max_ms.compare_exchange_weak(current_motion_max, motion_ms))
    {
    }
}

//===================================================================================
//===================================================================================
// Builds the wrapper config from the persisted GS stabilization state.
GsVisionStabilizerConfig buildWrapperConfig()
{
    GsVisionStabilizerConfig config = {};
    config.roi_divisor = s_imageStabilizationState.roi_divisor;
    // Keep fixed wrapper defaults for advanced OpenCV tuning not exposed in GS UI.
    config.zoom_factor = 0.75f;
    config.max_corners = 400;
    config.quality_level = 0.01f;
    config.min_distance = 20.0f;
    return config;
}

//===================================================================================
//===================================================================================
// Returns true when two wrapper configs contain the same tuning values.
bool configsEqual(const GsVisionStabilizerConfig& a, const GsVisionStabilizerConfig& b)
{
    return a.roi_divisor == b.roi_divisor;
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
        s_stabilization_api.prepare_frame_features = loadOpenCvSymbol<PrepareFrameFeaturesFn>(
            s_stabilization_api,
            "gs_vision_stabilizer_prepare_frame_features");

        if(s_stabilization_api.create == nullptr ||
           s_stabilization_api.destroy == nullptr ||
           s_stabilization_api.reset == nullptr ||
           s_stabilization_api.estimate_frame == nullptr ||
           s_stabilization_api.prepare_frame_features == nullptr)
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
// Converts configured 30 FPS stabilization decay into a decay multiplier for an arbitrary frame interval.
float computeTrajectoryDecayMultiplier(float decay_per_30fps_frame, float frame_delta_seconds)
{
    const float decay = std::clamp(decay_per_30fps_frame, 0.01f, 1.0f);
    const float baseline_seconds = 1.0f / 30.0f;
    const float ratio = std::clamp(frame_delta_seconds / baseline_seconds, 0.0f, 8.0f);
    return std::pow(1.0f - decay, ratio);
}

//===================================================================================
//===================================================================================
// Builds a delay scale for decay from normalized translation/rotation proximity to limits.
float computeLimitDelayScale(float dx,
                             float dy,
                             float angle_rad,
                             uint32_t width,
                             uint32_t height,
                             float max_offset_screen_fraction,
                             float max_angle_deg,
                             float limit_release_boost)
{
    const float max_dx = std::max(1.0f, static_cast<float>(width) * max_offset_screen_fraction);
    const float max_dy = std::max(1.0f, static_cast<float>(height) * max_offset_screen_fraction);
    const float max_angle_rad = max_angle_deg * (3.1415926535f / 180.0f);
    const float dx_relative = std::clamp(std::abs(dx) / max_dx, 0.0f, 1.0f);
    const float dy_relative = std::clamp(std::abs(dy) / max_dy, 0.0f, 1.0f);
    const float angle_relative = std::clamp(std::abs(angle_rad) / std::max(max_angle_rad, 0.0001f), 0.0f, 1.0f);
    const float current_limit_value = std::max(dx_relative, std::max(dy_relative, angle_relative));
    const float boost = std::clamp(limit_release_boost, 0.0f, 10.0f);
    if(boost <= 0.0f)
    {
        return 1.0f;
    }
    return 1.0f / (1.0f + boost * current_limit_value);
}

//===================================================================================
//===================================================================================
// Estimates one weighted translation from tracked old/new feature points.
bool computeTrackedFeaturePairTranslation(const StabilizationMotionEstimate& latest_motion_estimate,
                                          float& out_dx,
                                          float& out_dy)
{
    out_dx = 0.0f;
    out_dy = 0.0f;
    if(!latest_motion_estimate.valid ||
       latest_motion_estimate.feature_old_xs.empty() ||
       latest_motion_estimate.feature_old_xs.size() != latest_motion_estimate.feature_old_ys.size() ||
       latest_motion_estimate.feature_old_xs.size() != latest_motion_estimate.feature_new_xs.size() ||
       latest_motion_estimate.feature_old_xs.size() != latest_motion_estimate.feature_new_ys.size() ||
       latest_motion_estimate.feature_old_xs.size() != latest_motion_estimate.feature_confidence.size() ||
       latest_motion_estimate.feature_old_xs.size() != latest_motion_estimate.feature_status.size())
    {
        return false;
    }

    float sum_dx = 0.0f;
    float sum_dy = 0.0f;
    float sum_w = 0.0f;
    for(size_t i = 0; i < latest_motion_estimate.feature_old_xs.size(); ++i)
    {
        if(latest_motion_estimate.feature_status[i] == 0)
        {
            continue;
        }
        const float confidence = std::clamp(latest_motion_estimate.feature_confidence[i], 0.0f, 1.0f);
        const float w = std::max(0.05f, confidence);
        sum_dx += (latest_motion_estimate.feature_new_xs[i] - latest_motion_estimate.feature_old_xs[i]) * w;
        sum_dy += (latest_motion_estimate.feature_new_ys[i] - latest_motion_estimate.feature_old_ys[i]) * w;
        sum_w += w;
    }

    if(sum_w <= 0.0f)
    {
        return false;
    }
    out_dx = sum_dx / sum_w;
    out_dy = sum_dy / sum_w;
    return true;
}

//===================================================================================
//===================================================================================
// Draws tracked stabilization feature vectors in screen space from ROI and motion state.
void drawStabilizationFeatureOverlay(ImDrawList* draw_list,
                                     float roi_min_x,
                                     float roi_min_y,
                                     float roi_max_x,
                                     float roi_max_y,
                                     int frame_width,
                                     int frame_height,
                                     float roi_divisor,
                                     const StabilizationMotionEstimate& motion_state)
{
    if(draw_list == nullptr || frame_width <= 0 || frame_height <= 0 || roi_divisor <= 1.05f)
    {
        return;
    }

    const size_t feature_count = motion_state.feature_old_xs.size();
    if(feature_count == 0 ||
       feature_count != motion_state.feature_old_ys.size() ||
       feature_count != motion_state.feature_new_xs.size() ||
       feature_count != motion_state.feature_new_ys.size() ||
       feature_count != motion_state.feature_confidence.size() ||
       feature_count != motion_state.feature_status.size())
    {
        return;
    }

    const float fw = static_cast<float>(frame_width);
    const float fh = static_cast<float>(frame_height);
    const int roi_mx = static_cast<int>(fw / roi_divisor);
    const int roi_my = static_cast<int>(fh / roi_divisor);
    const int roi_x = std::clamp(roi_mx, 0, frame_width - 1);
    const int roi_y = std::clamp(roi_my, 0, frame_height - 1);
    const int roi_w = std::max(1, frame_width - roi_x * 2);
    const int roi_h = std::max(1, frame_height - roi_y * 2);
    const float nx0 = static_cast<float>(roi_x) / fw;
    const float ny0 = static_cast<float>(roi_y) / fh;
    const float nxr = static_cast<float>(roi_w) / fw;
    const float nyr = static_cast<float>(roi_h) / fh;
    if(nxr <= 0.0001f || nyr <= 0.0001f)
    {
        return;
    }

    const float qw = (roi_max_x - roi_min_x) / nxr;
    const float qh = (roi_max_y - roi_min_y) / nyr;
    const float qx = roi_min_x - nx0 * qw;
    const float qy = roi_min_y - ny0 * qh;
    for(size_t i = 0; i < feature_count; ++i)
    {
        const float old_sx = qx + motion_state.feature_old_xs[i] / fw * qw;
        const float old_sy = qy + motion_state.feature_old_ys[i] / fh * qh;
        const float new_sx = qx + motion_state.feature_new_xs[i] / fw * qw;
        const float new_sy = qy + motion_state.feature_new_ys[i] / fh * qh;
        if(motion_state.feature_status[i] == 0)
        {
            const ImU32 color = IM_COL32(255, 48, 48, 220);
            draw_list->AddLine(ImVec2(old_sx, old_sy), ImVec2(new_sx, new_sy), color, 1.0f);
            draw_list->AddCircleFilled(ImVec2(old_sx, old_sy), 3.0f, color);
            continue;
        }

        const float confidence = std::clamp(motion_state.feature_confidence[i], 0.0f, 1.0f);
        const uint8_t red = static_cast<uint8_t>(255.0f * (1.0f - confidence));
        const ImU32 color = IM_COL32(red, 255, 0, 220);
        draw_list->AddLine(ImVec2(old_sx, old_sy), ImVec2(new_sx, new_sy), color, 1.0f);
        draw_list->AddCircleFilled(ImVec2(new_sx, new_sy), 3.0f, color);
    }
}

//===================================================================================
//===================================================================================
// Draws stabilization ROI outline and tracked feature vectors for one viewport.
void drawStabilizationRoiOverlay(float quad_x,
                                 float viewport_width,
                                 float overlay_width,
                                 float surface_height,
                                 int frame_width,
                                 int frame_height,
                                 int screen_mode,
                                 float zoom)
{
    if(!s_imageStabilizationState.debug)
    {
        return;
    }
    const float roi_divisor = s_imageStabilizationState.roi_divisor;
    if(roi_divisor <= 1.05f ||
       frame_width <= 0 ||
       frame_height <= 0 ||
       overlay_width <= 0.0f ||
       surface_height <= 0.0f)
    {
        return;
    }

    gs::render::StabilizationRoiScreenRect r = {};
    if(!gs::render::computeStabilizationRoiScreenRect(quad_x,
                                                      0.0f,
                                                      viewport_width,
                                                      surface_height,
                                                      frame_width,
                                                      frame_height,
                                                      screen_mode,
                                                      zoom,
                                                      roi_divisor,
                                                      r))
    {
        return;
    }

    // After drawMenuLocked the current window is not FULLSCREEN_OVERLAY; use the
    // foreground draw list so ROI lines land in screen space on top of the menu.
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    if(draw_list == nullptr)
    {
        return;
    }
    const float min_x = std::clamp(std::min(r.min_x, r.max_x), 0.0f, overlay_width);
    const float max_x = std::clamp(std::max(r.min_x, r.max_x), 0.0f, overlay_width);
    const float min_y = std::clamp(std::min(r.min_y, r.max_y), 0.0f, surface_height);
    const float max_y = std::clamp(std::max(r.min_y, r.max_y), 0.0f, surface_height);
    if(max_x - min_x < 1.0f || max_y - min_y < 1.0f)
    {
        return;
    }
    draw_list->AddRect(ImVec2(min_x, min_y),
                       ImVec2(max_x, max_y),
                       IM_COL32(255, 255, 255, 255),
                       0.0f,
                       0,
                       1.0f);

    const gs::stabilization::StabilizationMotionEstimate debug_state = gs::stabilization::getLatestMotionEstimate();
    gs::stabilization::drawStabilizationFeatureOverlay(draw_list,
                                                       r.min_x,
                                                       r.min_y,
                                                       r.max_x,
                                                       r.max_y,
                                                       frame_width,
                                                       frame_height,
                                                       roi_divisor,
                                                       debug_state);
}

//===================================================================================
//===================================================================================
// Draws stabilization ROI outline and tracked feature vectors using letterboxed quad geometry.
void drawStabilizationRoiOverlayLetterboxed(float quad_x,
                                            float viewport_width,
                                            float overlay_width,
                                            float surface_height,
                                            float video_aspect,
                                            ScreenAspectRatio screen_aspect_ratio,
                                            float screen_zoom,
                                            int frame_width,
                                            int frame_height)
{
    if(!s_imageStabilizationState.debug)
    {
        return;
    }
    const float roi_divisor = s_imageStabilizationState.roi_divisor;
    if(roi_divisor <= 1.05f ||
       frame_width <= 0 ||
       frame_height <= 0 ||
       overlay_width <= 0.0f ||
       surface_height <= 0.0f)
    {
        return;
    }

    gs::render::StabilizationRoiScreenRect roi_rect = {};
    if(!gs::render::computeStabilizationRoiScreenRectLetterboxed(quad_x,
                                                                  0,
                                                                  static_cast<int>(std::lround(viewport_width)),
                                                                  static_cast<int>(std::lround(surface_height)),
                                                                  video_aspect,
                                                                  screen_aspect_ratio,
                                                                  screen_zoom,
                                                                  frame_width,
                                                                  frame_height,
                                                                  roi_divisor,
                                                                  roi_rect))
    {
        return;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if(draw_list == nullptr)
    {
        return;
    }
    const float min_x = std::clamp(std::min(roi_rect.min_x, roi_rect.max_x), 0.0f, overlay_width);
    const float max_x = std::clamp(std::max(roi_rect.min_x, roi_rect.max_x), 0.0f, overlay_width);
    const float min_y = std::clamp(std::min(roi_rect.min_y, roi_rect.max_y), 0.0f, surface_height);
    const float max_y = std::clamp(std::max(roi_rect.min_y, roi_rect.max_y), 0.0f, surface_height);
    if(max_x - min_x >= 1.0f && max_y - min_y >= 1.0f)
    {
        draw_list->AddRect(ImVec2(min_x, min_y),
                           ImVec2(max_x, max_y),
                           IM_COL32(255, 255, 255, 255),
                           0.0f,
                           0,
                           1.0f);
    }

    const gs::stabilization::StabilizationMotionEstimate dbg_vis = gs::stabilization::getLatestMotionEstimate();
    gs::stabilization::drawStabilizationFeatureOverlay(draw_list,
                                                       roi_rect.min_x,
                                                       roi_rect.min_y,
                                                       roi_rect.max_x,
                                                       roi_rect.max_y,
                                                       frame_width,
                                                       frame_height,
                                                       roi_divisor,
                                                       dbg_vis);
}

//===================================================================================
//===================================================================================
// Builds an output->source affine matrix from translation and rotation around frame center.
StabilizationTransform buildAffineTransform(uint32_t width,
                                            uint32_t height,
                                            float dx,
                                            float dy,
                                            float angle_rad,
                                            float zoom)
{
    StabilizationTransform tf = {};
    if(width == 0 || height == 0)
    {
        return tf;
    }

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float cx = w * 0.5f;
    const float cy = h * 0.5f;
    const float zoom_clamped = std::clamp(zoom, 1.0f, 2.0f);
    const float inv_zoom = 1.0f / zoom_clamped;
    const float c = std::cos(angle_rad);
    const float s = std::sin(angle_rad);
    const float a00 = c * inv_zoom;
    const float a01 = -s * inv_zoom;
    const float a10 = s * inv_zoom;
    const float a11 = c * inv_zoom;
    tf.enabled = true;
    tf.width = static_cast<int>(width);
    tf.height = static_cast<int>(height);
    tf.m00 = a00;
    tf.m01 = a01;
    tf.m02 = cx + dx - a00 * cx - a01 * cy;
    tf.m10 = a10;
    tf.m11 = a11;
    tf.m12 = cy + dy - a10 * cx - a11 * cy;
    return tf;
}

//===================================================================================
//===================================================================================
// Clamps accumulated stabilization trajectory to bounded translation and rotation.
void clampTrajectoryToSafetyLimits(uint32_t width,
                                   uint32_t height,
                                   float max_offset_screen_fraction,
                                   float max_angle_deg,
                                   float& dx,
                                   float& dy,
                                   float& angle_rad)
{
    const float max_dx = static_cast<float>(width) * max_offset_screen_fraction;
    const float max_dy = static_cast<float>(height) * max_offset_screen_fraction;
    const float max_angle_rad = max_angle_deg * (3.1415926535f / 180.0f);
    dx = std::clamp(dx, -max_dx, max_dx);
    dy = std::clamp(dy, -max_dy, max_dy);
    angle_rad = std::clamp(angle_rad, -max_angle_rad, max_angle_rad);
}

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
        std::lock_guard<std::mutex> debug_lock(s_motion_estimate_mutex);
        s_latest_motion_estimate = {};
    }
    {
        std::lock_guard<std::mutex> trajectory_lock(s_render_trajectory_state_mutex);
        s_render_trajectory_state = {};
    }
    s_last_frame_id = 0;
    s_last_frame_width = 0;
    s_last_frame_height = 0;
    s_last_frame_stride = 0;
    s_last_frame_format = GS_VISION_IMAGE_FORMAT_RGB8;
    s_last_frame_id_valid = false;
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
                   bool frame_id_valid,
                   uint32_t frame_id)
{
    if(!isEnabled())
    {
        std::lock_guard<std::mutex> debug_lock(s_motion_estimate_mutex);
        s_latest_motion_estimate = {};
        return false;
    }

    const int bytes_per_pixel = bytesPerPixelForFormat(format);
    if(pixels == nullptr ||
       size == 0 ||
       width <= 0 ||
       height <= 0 ||
       bytes_per_pixel <= 0 ||
       stride < width * bytes_per_pixel ||
       size < static_cast<size_t>(stride) * static_cast<size_t>(height))
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(s_stabilization_mutex);

    if(frame_id_valid &&
       s_last_frame_id_valid &&
       frame_id == s_last_frame_id &&
       width == s_last_frame_width &&
       height == s_last_frame_height &&
       stride == s_last_frame_stride &&
       format == s_last_frame_format)
    {
        // Playback pause can feed the same decoded frame repeatedly. Keep previous
        // motion state so visualization continues to show the last estimate.
        std::lock_guard<std::mutex> debug_lock(s_motion_estimate_mutex);
        return s_latest_motion_estimate.valid;
    }
    s_last_frame_id = frame_id;
    s_last_frame_width = width;
    s_last_frame_height = height;
    s_last_frame_stride = stride;
    s_last_frame_format = format;
    s_last_frame_id_valid = frame_id_valid;

    StabilizationApi& api = loadStabilizationApiLocked();
    if(api.estimate_frame == nullptr ||
       api.prepare_frame_features == nullptr ||
       !ensureConfiguredLocked(api) ||
       api.stabilizer == nullptr)
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
        std::lock_guard<std::mutex> debug_lock(s_motion_estimate_mutex);
        gs::stabilization::StabilizationMotionEstimate state = {};
        state.valid = true;
        state.width = width;
        state.height = height;
        state.tracked_points = result.tracked_points;
        state.inliers = result.inliers;
        state.raw_dx = result.dx;
        state.raw_dy = result.dy;
        state.raw_angle = result.angle_radians;
        state.measured_dx = result.measured_dx;
        state.measured_dy = result.measured_dy;
        state.measured_angle = result.measured_angle_radians;
        state.compensated_dx = result.compensated_dx;
        state.compensated_dy = result.compensated_dy;
        state.compensated_angle = result.compensated_angle_radians;
        state.transform_m02 = result.transform_02;
        state.transform_m12 = result.transform_12;
        const int32_t min_inliers_for_fit = std::max(6, result.tracked_points / 5);
        state.weak_fit = (result.tracked_points < 8) || (result.inliers < min_inliers_for_fit);
        state.feature_old_xs.assign(result.feature_old_x, result.feature_old_x + result.feature_count);
        state.feature_old_ys.assign(result.feature_old_y, result.feature_old_y + result.feature_count);
        state.feature_new_xs.assign(result.feature_new_x, result.feature_new_x + result.feature_count);
        state.feature_new_ys.assign(result.feature_new_y, result.feature_new_y + result.feature_count);
        state.feature_confidence.assign(result.feature_confidence,
                                        result.feature_confidence + result.feature_count);
        state.feature_status.assign(result.feature_status, result.feature_status + result.feature_count);
        s_latest_motion_estimate = state;
    }

    recordStabilizationDurations(static_cast<uint32_t>(std::max(0.0f, result.feature_ms)),
                                 static_cast<uint32_t>(std::max(0.0f, result.motion_ms)));
    return result.stabilized != 0;
}

//===================================================================================
//===================================================================================
// Explicitly prepares Shi-Tomasi features for the next stabilization estimate.
bool prepareFrameFeatures(const uint8_t* pixels,
                          size_t size,
                          int width,
                          int height,
                          int stride,
                          GsVisionImageFormat format)
{
    if(!isEnabled())
    {
        return false;
    }
    const int bytes_per_pixel = bytesPerPixelForFormat(format);
    if(pixels == nullptr ||
       size == 0 ||
       width <= 0 ||
       height <= 0 ||
       bytes_per_pixel <= 0 ||
       stride < width * bytes_per_pixel ||
       size < static_cast<size_t>(stride) * static_cast<size_t>(height))
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(s_stabilization_mutex);
    StabilizationApi& api = loadStabilizationApiLocked();
    if(api.prepare_frame_features == nullptr ||
       !ensureConfiguredLocked(api) ||
       api.stabilizer == nullptr)
    {
        return false;
    }

    const GsVisionImage input = {
        pixels,
        width,
        height,
        stride,
        format
    };
    float feature_ms = 0.0f;
    if(api.prepare_frame_features(api.stabilizer, &input, &feature_ms) == 0)
    {
        return false;
    }

    s_stabilization_feature_last_ms.store(static_cast<uint32_t>(std::max(0.0f, feature_ms)));
    uint32_t current_feature_max = s_stabilization_feature_max_ms.load();
    const uint32_t feature_ms_u32 = static_cast<uint32_t>(std::max(0.0f, feature_ms));
    while(feature_ms_u32 > current_feature_max &&
          !s_stabilization_feature_max_ms.compare_exchange_weak(current_feature_max, feature_ms_u32))
    {
    }
    return true;
}

//===================================================================================
//===================================================================================
// Returns the latest per-frame motion estimate used by render-time stabilization.
StabilizationMotionEstimate getLatestMotionEstimate()
{
    std::lock_guard<std::mutex> debug_lock(s_motion_estimate_mutex);
    return s_latest_motion_estimate;
}

//===================================================================================
//===================================================================================
// Returns and resets stabilization timings accumulated since the previous call.
StabilizationStats consumeStats()
{
    StabilizationStats stats = {};
    stats.count = s_stabilization_count.exchange(0);
    stats.feature_last_ms = s_stabilization_feature_last_ms.exchange(0);
    stats.feature_max_ms = s_stabilization_feature_max_ms.exchange(0);
    stats.motion_last_ms = s_stabilization_motion_last_ms.exchange(0);
    stats.motion_max_ms = s_stabilization_motion_max_ms.exchange(0);
    if(stats.count == 0)
    {
        stats.feature_last_ms = 0;
        stats.feature_max_ms = 0;
        stats.motion_last_ms = 0;
        stats.motion_max_ms = 0;
    }
    return stats;
}

//===================================================================================
//===================================================================================
// Updates shared decoder trajectory state from latest motion estimate for one frame.
void updateRenderTrajectoryStateForFrame(uint32_t frame_id, uint32_t width, uint32_t height)
{
    const gs::stabilization::StabilizationMotionEstimate latest_motion_estimate = getLatestMotionEstimate();
    std::lock_guard<std::mutex> trajectory_lock(s_render_trajectory_state_mutex);
    if(!isEnabled())
    {
        s_render_trajectory_state = {};
        return;
    }

    const bool is_same_frame_id =
        s_render_trajectory_state.last_valid_transform_frame_id_valid &&
        s_render_trajectory_state.last_valid_transform_frame_id == frame_id;
    if(is_same_frame_id)
    {
        return;
    }

    if(latest_motion_estimate.valid)
    {
        float measured_dx = latest_motion_estimate.measured_dx;
        float measured_dy = latest_motion_estimate.measured_dy;
        float pair_dx = 0.0f;
        float pair_dy = 0.0f;
        if(gs::stabilization::computeTrackedFeaturePairTranslation(latest_motion_estimate, pair_dx, pair_dy))
        {
            measured_dx = pair_dx;
            measured_dy = pair_dy;
        }

        const std::chrono::steady_clock::time_point now_tp = std::chrono::steady_clock::now();
        const std::chrono::duration<float> frame_delta =
            s_render_trajectory_state.last_trajectory_update_tp == std::chrono::steady_clock::time_point{}
                ? std::chrono::duration<float>(1.0f / 30.0f)
                : (now_tp - s_render_trajectory_state.last_trajectory_update_tp);
        const float decay =
            gs::stabilization::computeTrajectoryDecayMultiplier(
                s_imageStabilizationState.stabilization_decay,
                frame_delta.count());
        s_render_trajectory_state.last_trajectory_update_tp = now_tp;

        constexpr float kMaxTrajectoryOffsetScreenFraction = 0.5f;
        constexpr float kMaxTrajectoryAngleDeg = 45.0f;
        s_render_trajectory_state.trajectory_accum_dx += measured_dx;
        s_render_trajectory_state.trajectory_accum_dy += measured_dy;
        s_render_trajectory_state.trajectory_accum_angle += latest_motion_estimate.measured_angle;
        gs::stabilization::clampTrajectoryToSafetyLimits(width,
                                                         height,
                                                         kMaxTrajectoryOffsetScreenFraction,
                                                         kMaxTrajectoryAngleDeg,
                                                         s_render_trajectory_state.trajectory_accum_dx,
                                                         s_render_trajectory_state.trajectory_accum_dy,
                                                         s_render_trajectory_state.trajectory_accum_angle);
        const float limit_delay_scale =
            gs::stabilization::computeLimitDelayScale(s_render_trajectory_state.trajectory_accum_dx,
                                                      s_render_trajectory_state.trajectory_accum_dy,
                                                      s_render_trajectory_state.trajectory_accum_angle,
                                                      width,
                                                      height,
                                                      kMaxTrajectoryOffsetScreenFraction,
                                                      kMaxTrajectoryAngleDeg,
                                                      s_imageStabilizationState.limit_release_boost);
        const float total_decay = decay * limit_delay_scale;
        s_render_trajectory_state.trajectory_accum_dx *= total_decay;
        s_render_trajectory_state.trajectory_accum_dy *= total_decay;
        s_render_trajectory_state.trajectory_accum_angle *= total_decay;
        gs::stabilization::clampTrajectoryToSafetyLimits(width,
                                                         height,
                                                         kMaxTrajectoryOffsetScreenFraction,
                                                         kMaxTrajectoryAngleDeg,
                                                         s_render_trajectory_state.trajectory_accum_dx,
                                                         s_render_trajectory_state.trajectory_accum_dy,
                                                         s_render_trajectory_state.trajectory_accum_angle);

        s_render_trajectory_state.last_valid_transform = gs::stabilization::buildAffineTransform(
            width,
            height,
            s_render_trajectory_state.trajectory_accum_dx,
            s_render_trajectory_state.trajectory_accum_dy,
            s_render_trajectory_state.trajectory_accum_angle,
            s_imageStabilizationState.zoom);
    }
    else
    {
        s_render_trajectory_state.last_valid_transform = {};
    }

    s_render_trajectory_state.last_valid_transform_frame_id = frame_id;
    s_render_trajectory_state.last_valid_transform_frame_id_valid = true;
}

//===================================================================================
//===================================================================================
// Returns the latest decoder trajectory-based render transform.
StabilizationTransform getRenderTrajectoryTransform()
{
    std::lock_guard<std::mutex> trajectory_lock(s_render_trajectory_state_mutex);
    return s_render_trajectory_state.last_valid_transform;
}

//===================================================================================
//===================================================================================
// Clears shared decoder trajectory state.
void resetRenderTrajectoryState()
{
    std::lock_guard<std::mutex> trajectory_lock(s_render_trajectory_state_mutex);
    s_render_trajectory_state = {};
}

} // namespace gs::stabilization
