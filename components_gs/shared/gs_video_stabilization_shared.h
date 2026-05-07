#pragma once

#include <cstdint>
#include <chrono>
#include <vector>
#include "imgui.h"
#include "../../OpenCV/OpenCVWrapper/include/gs_vision_opencv_wrapper.h"

enum class ScreenAspectRatio : int;

namespace gs::stabilization
{

//===================================================================================
//===================================================================================
// Aggregates stabilization timings consumed once per stats window.
struct StabilizationStats
{
    uint32_t count = 0;
    uint32_t min_ms = 0;
    uint32_t max_ms = 0;
};

//===================================================================================
//===================================================================================
// Holds the render-time affine stabilization transform in source pixel coordinates.
struct StabilizationTransform
{
    bool enabled = false;
    int width = 0;
    int height = 0;
    float m00 = 1.0f;
    float m01 = 0.0f;
    float m02 = 0.0f;
    float m10 = 0.0f;
    float m11 = 1.0f;
    float m12 = 0.0f;
};

//===================================================================================
//===================================================================================
// Carries raw per-frame motion-estimation diagnostics from the OpenCV wrapper.
struct StabilizationMotionEstimate
{
    bool valid = false;
    uint32_t sequence = 0;
    int width = 0;
    int height = 0;
    int tracked_points = 0;
    int inliers = 0;
    float raw_dx = 0.0f;
    float raw_dy = 0.0f;
    float raw_angle = 0.0f;
    float measured_dx = 0.0f;
    float measured_dy = 0.0f;
    float measured_angle = 0.0f;
    float compensated_dx = 0.0f;
    float compensated_dy = 0.0f;
    float compensated_angle = 0.0f;
    float transform_m02 = 0.0f;
    float transform_m12 = 0.0f;
    bool weak_fit = false;
    std::vector<float> feature_old_xs;
    std::vector<float> feature_old_ys;
    std::vector<float> feature_new_xs;
    std::vector<float> feature_new_ys;
    std::vector<float> feature_confidence;
    std::vector<int32_t> feature_status;
};

//===================================================================================
//===================================================================================
// Stores decoder-side trajectory accumulators and last valid render transform.
struct RenderTrajectoryState
{
    StabilizationTransform last_valid_transform = {};
    bool last_valid_transform_frame_id_valid = false;
    uint32_t last_valid_transform_frame_id = 0;
    float trajectory_accum_dx = 0.0f;
    float trajectory_accum_dy = 0.0f;
    float trajectory_accum_angle = 0.0f;
    std::chrono::steady_clock::time_point last_trajectory_update_tp = {};
};

//===================================================================================
//===================================================================================
// Converts configured 30 FPS stabilization decay into a decay multiplier for an arbitrary frame interval.
float computeTrajectoryDecayMultiplier(float decay_per_30fps_frame, float frame_delta_seconds);

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
                             float limit_release_boost);

//===================================================================================
//===================================================================================
// Estimates one weighted translation from tracked old/new feature points.
bool computeTrackedFeaturePairTranslation(const StabilizationMotionEstimate& latest_motion_estimate,
                                          float& out_dx,
                                          float& out_dy);

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
                                     const StabilizationMotionEstimate& motion_state);

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
                                 float zoom);

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
                                            int frame_height);

//===================================================================================
//===================================================================================
// Builds an output->source affine matrix from translation and rotation around frame center.
StabilizationTransform buildAffineTransform(uint32_t width,
                                            uint32_t height,
                                            float dx,
                                            float dy,
                                            float angle_rad,
                                            float zoom);

//===================================================================================
//===================================================================================
// Clamps accumulated stabilization trajectory to bounded translation and rotation.
void clampTrajectoryToSafetyLimits(uint32_t width,
                                   uint32_t height,
                                   float max_offset_screen_fraction,
                                   float max_angle_deg,
                                   float& dx,
                                   float& dy,
                                   float& angle_rad);

//===================================================================================
//===================================================================================
// Returns true when video stabilization is enabled for this process.
bool isEnabled();

//===================================================================================
//===================================================================================
// Resets temporal stabilization state so the next frame becomes a new reference.
void reset();

//===================================================================================
//===================================================================================
// Estimates stabilization transform for a caller-owned frame without copying.
bool estimateFrame(const uint8_t* pixels,
                   size_t size,
                   int width,
                   int height,
                   int stride,
                   GsVisionImageFormat format,
                   bool frame_id_valid,
                   uint32_t frame_id);

//===================================================================================
//===================================================================================
// Returns the latest per-frame motion estimate used by render-time stabilization.
StabilizationMotionEstimate getLatestMotionEstimate();

//===================================================================================
//===================================================================================
// Returns and resets stabilization timings accumulated since the previous call.
StabilizationStats consumeStats();

//===================================================================================
//===================================================================================
// Updates shared decoder trajectory state from latest motion estimate for one frame.
void updateRenderTrajectoryStateForFrame(uint32_t frame_id, uint32_t width, uint32_t height);

//===================================================================================
//===================================================================================
// Returns the latest decoder trajectory-based render transform.
StabilizationTransform getRenderTrajectoryTransform();

//===================================================================================
//===================================================================================
// Clears shared decoder trajectory state.
void resetRenderTrajectoryState();

} // namespace gs::stabilization

