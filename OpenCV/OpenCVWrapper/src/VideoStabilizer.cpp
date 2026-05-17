#include "gs_vision_opencv_wrapper.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <vector>

//===================================================================================
//===================================================================================
// Owns temporal state for ROI-based low-latency video stabilization.
struct GsVisionStabilizer
{
    GsVisionStabilizerConfig config = {};
    cv::Mat previous_gray_roi;
    cv::Mat current_gray;
    float trajectory_x = 0.0f;
    float trajectory_y = 0.0f;
    float trajectory_angle = 0.0f;
    int32_t frame_count = 0;
    float inc_ema_x = 0.0f;
    float inc_ema_y = 0.0f;
    float inc_ema_angle = 0.0f;
    float last_valid_dx = 0.0f;
    float last_valid_dy = 0.0f;
    float last_valid_angle = 0.0f;
    float compensated_out_x = 0.0f;
    float compensated_out_y = 0.0f;
    float compensated_out_angle = 0.0f;
    bool has_prev_measured = false;
    float prev_measured_dx = 0.0f;
    float prev_measured_dy = 0.0f;
    int32_t reject_hold_frames = 0;
    bool has_prev_filtered = false;
    float prev_filtered_dx = 0.0f;
    float prev_filtered_dy = 0.0f;
    float prev_filtered_angle = 0.0f;
    std::vector<cv::Point2f> cached_previous_points;
    float last_feature_ms = 0.0f;
    const uint8_t* last_estimate_input_data = nullptr;
    int32_t last_estimate_width = 0;
    int32_t last_estimate_height = 0;
    int32_t last_estimate_stride_bytes = 0;
    GsVisionImageFormat last_estimate_format = GS_VISION_IMAGE_FORMAT_RGB8;
};

namespace
{
using StabilizerClock = std::chrono::steady_clock;
constexpr int32_t kTrackingDownscale = 2;
constexpr float kTrackingPointScale = static_cast<float>(kTrackingDownscale);
constexpr float kTrackingPointOffset = (kTrackingPointScale - 1.0f) * 0.5f;

//===================================================================================
//===================================================================================
// Returns elapsed milliseconds since the provided timestamp.
float ElapsedMs(StabilizerClock::time_point start)
{
    return std::chrono::duration<float, std::milli>(StabilizerClock::now() - start).count();
}

//===================================================================================
//===================================================================================
// Moves current toward target using a bounded step magnitude.
float StepTowards(float current, float target, float max_step)
{
    const float delta = target - current;
    return current + std::clamp(delta, -max_step, max_step);
}

//===================================================================================
//===================================================================================
// Returns the default stabilization tuning used when callers omit config values.
GsVisionStabilizerConfig BuildDefaultConfig()
{
    GsVisionStabilizerConfig config = {};
    config.roi_divisor = 3.5f;
    config.zoom_factor = 0.75f;
    config.max_corners = 400;
    config.quality_level = 0.01f;
    config.min_distance = 20.0f;
    return config;
}

//===================================================================================
//===================================================================================
// Clamps config values to ranges that keep the OpenCV calls well-defined.
GsVisionStabilizerConfig NormalizeConfig(const GsVisionStabilizerConfig* input)
{
    GsVisionStabilizerConfig config = BuildDefaultConfig();
    if(input != nullptr)
    {
        if(input->roi_divisor > 1.1f)
        {
            config.roi_divisor = input->roi_divisor;
        }
        if(input->zoom_factor > 0.1f && input->zoom_factor <= 1.0f)
        {
            config.zoom_factor = input->zoom_factor;
        }
        if(input->max_corners > 0)
        {
            config.max_corners = input->max_corners;
        }
        if(input->quality_level > 0.0f && input->quality_level < 1.0f)
        {
            config.quality_level = input->quality_level;
        }
        if(input->min_distance > 0.0f)
        {
            config.min_distance = input->min_distance;
        }
    }

    // Enforce a strong anti-shake baseline even when legacy saved settings are weak.
    config.zoom_factor = std::clamp(std::min(config.zoom_factor, 0.60f), 0.1f, 1.0f);
    config.min_distance = std::clamp(std::min(config.min_distance, 20.0f), 1.0f, 200.0f);

    return config;
}

//===================================================================================
//===================================================================================
// Returns the number of bytes in one pixel for the wrapper image format.
int32_t BytesPerPixel(GsVisionImageFormat format)
{
    switch(format)
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
// Converts only the requested ROI to a half-resolution grayscale image for tracking.
bool ConvertRoiToDownscaledGray(const GsVisionImage& image, const cv::Rect& roi, cv::Mat& out_gray_roi)
{
    const int32_t bytes_per_pixel = BytesPerPixel(image.format);
    if(image.data == nullptr ||
       image.width <= 0 ||
       image.height <= 0 ||
       bytes_per_pixel == 0 ||
       image.stride_bytes < image.width * bytes_per_pixel ||
       roi.x < 0 ||
       roi.y < 0 ||
       roi.x + roi.width > image.width ||
       roi.y + roi.height > image.height)
    {
        return false;
    }

    const int32_t out_width = std::max(1, (roi.width + kTrackingDownscale - 1) / kTrackingDownscale);
    const int32_t out_height = std::max(1, (roi.height + kTrackingDownscale - 1) / kTrackingDownscale);
    out_gray_roi.create(out_height, out_width, CV_8UC1);

    const bool input_is_rgb =
        image.format == GS_VISION_IMAGE_FORMAT_RGB8 || image.format == GS_VISION_IMAGE_FORMAT_RGBA8;
    const bool input_has_alpha =
        image.format == GS_VISION_IMAGE_FORMAT_BGRA8 || image.format == GS_VISION_IMAGE_FORMAT_RGBA8;
    const int32_t color_channels = input_has_alpha ? 4 : 3;
    for(int32_t y = 0; y < out_height; ++y)
    {
        uint8_t* gray_row = out_gray_roi.ptr(y);
        const int32_t src_y0 = y * kTrackingDownscale;
        const int32_t block_height = std::min(kTrackingDownscale, roi.height - src_y0);
        for(int32_t x = 0; x < out_width; ++x)
        {
            const int32_t src_x0 = x * kTrackingDownscale;
            const int32_t block_width = std::min(kTrackingDownscale, roi.width - src_x0);
            uint32_t gray_sum = 0;
            uint32_t sample_count = 0;
            for(int32_t yy = 0; yy < block_height; ++yy)
            {
                const uint8_t* input_row =
                    image.data +
                    static_cast<size_t>(roi.y + src_y0 + yy) * static_cast<size_t>(image.stride_bytes) +
                    static_cast<size_t>(roi.x + src_x0) * static_cast<size_t>(bytes_per_pixel);
                for(int32_t xx = 0; xx < block_width; ++xx)
                {
                    switch(image.format)
                    {
                        case GS_VISION_IMAGE_FORMAT_GRAY8:
                            gray_sum += input_row[xx];
                            break;
                        case GS_VISION_IMAGE_FORMAT_BGR8:
                        case GS_VISION_IMAGE_FORMAT_RGB8:
                        case GS_VISION_IMAGE_FORMAT_BGRA8:
                        case GS_VISION_IMAGE_FORMAT_RGBA8:
                        {
                            const uint8_t* pixel = input_row + static_cast<size_t>(xx) * color_channels;
                            const uint8_t c0 = pixel[0];
                            const uint8_t g = pixel[1];
                            const uint8_t c2 = pixel[2];
                            const uint8_t r = input_is_rgb ? c0 : c2;
                            const uint8_t b = input_is_rgb ? c2 : c0;
                            gray_sum += (77u * r + 150u * g + 29u * b + 128u) >> 8;
                            break;
                        }
                        case GS_VISION_IMAGE_FORMAT_RGB565:
                        {
                            const uint8_t* pixel = input_row + static_cast<size_t>(xx) * 2u;
                            const uint16_t packed =
                                static_cast<uint16_t>(pixel[0]) | (static_cast<uint16_t>(pixel[1]) << 8);
                            const uint8_t r5 = static_cast<uint8_t>((packed >> 11) & 0x1f);
                            const uint8_t g6 = static_cast<uint8_t>((packed >> 5) & 0x3f);
                            const uint8_t b5 = static_cast<uint8_t>(packed & 0x1f);
                            const uint8_t r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
                            const uint8_t g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
                            const uint8_t b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
                            gray_sum += (77u * r + 150u * g + 29u * b + 128u) >> 8;
                            break;
                        }
                        default:
                            return false;
                    }
                    sample_count++;
                }
            }
            gray_row[x] = static_cast<uint8_t>((gray_sum + sample_count / 2u) / sample_count);
        }
    }

    return true;
}

//===================================================================================
//===================================================================================
// Maps a point from the half-resolution ROI tracker space back into full-frame pixels.
cv::Point2f TrackingPointToFullFrame(const cv::Point2f& point, const cv::Point2f& roi_offset)
{
    return cv::Point2f(point.x * kTrackingPointScale + kTrackingPointOffset + roi_offset.x,
                       point.y * kTrackingPointScale + kTrackingPointOffset + roi_offset.y);
}

//===================================================================================
//===================================================================================
// Builds the central ROI used for low-latency motion estimation.
cv::Rect BuildRoi(const cv::Size& size, float roi_divisor)
{
    const int32_t margin_x = static_cast<int32_t>(static_cast<float>(size.width) / roi_divisor);
    const int32_t margin_y = static_cast<int32_t>(static_cast<float>(size.height) / roi_divisor);
    const int32_t x = std::clamp(margin_x, 0, size.width - 1);
    const int32_t y = std::clamp(margin_y, 0, size.height - 1);
    const int32_t width = std::max(1, size.width - x * 2);
    const int32_t height = std::max(1, size.height - y * 2);
    return cv::Rect(x, y, width, height);
}

}

//===================================================================================
//===================================================================================
// Creates one stateful video stabilizer instance.
GsVisionStabilizer* gs_vision_stabilizer_create(const GsVisionStabilizerConfig* config)
{
    try
    {
        GsVisionStabilizer* stabilizer = new GsVisionStabilizer();
        stabilizer->config = NormalizeConfig(config);
        return stabilizer;
    }
    catch(...)
    {
        return nullptr;
    }
}

//===================================================================================
//===================================================================================
// Destroys a video stabilizer instance created by gs_vision_stabilizer_create.
void gs_vision_stabilizer_destroy(GsVisionStabilizer* stabilizer)
{
    delete stabilizer;
}

//===================================================================================
//===================================================================================
// Clears all temporal state retained by a video stabilizer instance.
void gs_vision_stabilizer_reset(GsVisionStabilizer* stabilizer)
{
    if(stabilizer == nullptr)
    {
        return;
    }

    const GsVisionStabilizerConfig config = stabilizer->config;
    *stabilizer = {};
    stabilizer->config = config;
}

//===================================================================================
//===================================================================================
// Estimates stabilization motion
int32_t gs_vision_stabilizer_estimate_frame(
    GsVisionStabilizer* stabilizer,
    const GsVisionImage* input,
    GsVisionStabilizerFrameResult* result)
{
    try
    {
        // Phase 1: validate input and convert the configured ROI to half-resolution grayscale for tracking.
        if(stabilizer == nullptr || input == nullptr)
        {
            return 0;
        }

        // Reuse two ROI buffers in ping-pong mode: previous owns last frame, current is rewritten now.
        std::swap(stabilizer->previous_gray_roi, stabilizer->current_gray);

        const cv::Rect roi = BuildRoi(cv::Size(input->width, input->height), stabilizer->config.roi_divisor);
        if(!ConvertRoiToDownscaledGray(*input, roi, stabilizer->current_gray))
        {
            return 0;
        }
        const cv::Mat& current_gray_roi = stabilizer->current_gray;
        stabilizer->last_estimate_input_data = input->data;
        stabilizer->last_estimate_width = input->width;
        stabilizer->last_estimate_height = input->height;
        stabilizer->last_estimate_stride_bytes = input->stride_bytes;
        stabilizer->last_estimate_format = input->format;

        if(stabilizer->previous_gray_roi.empty() ||
           stabilizer->previous_gray_roi.size() != current_gray_roi.size())
        {
            // Phase 2: estimate never bootstraps features. Caller must seed via
            // gs_vision_stabilizer_prepare_frame_features; gracefully report "not stabilized".
            stabilizer->cached_previous_points.clear();
            if(result != nullptr)
            {
                *result = {};
                result->feature_ms = stabilizer->last_feature_ms;
            }
            return 1;
        }

        std::vector<cv::Point2f> previous_points = stabilizer->cached_previous_points;
        if(previous_points.empty())
        {
            // Feature preparation may have been skipped or state may have been reset
            // between calls; do not synthesize motion from stale carry terms.
            if(result != nullptr)
            {
                *result = {};
                result->feature_ms = stabilizer->last_feature_ms;
            }
            return 1;
        }
        const StabilizerClock::time_point motion_start = StabilizerClock::now();

        // Phase 3: estimate raw inter-frame motion from tracked corners + robust affine fit.
        float dx = 0.0f;
        float dy = 0.0f;
        float angle = 0.0f;
        float measured_dx = 0.0f;
        float measured_dy = 0.0f;
        float measured_angle = 0.0f;
        int32_t tracked_points = 0;
        int32_t inlier_count = 0;

        if(!previous_points.empty())
        {
            std::vector<cv::Point2f> current_points;
            std::vector<uint8_t> status;
            std::vector<float> error;
            cv::calcOpticalFlowPyrLK(stabilizer->previous_gray_roi,
                                     current_gray_roi,
                                     previous_points,
                                     current_points,
                                     status,
                                     error,
                                     cv::Size(15, 15),
                                     3,
                                     cv::TermCriteria(cv::TermCriteria::EPS | cv::TermCriteria::COUNT, 10, 0.03));

            std::vector<cv::Point2f> previous_full_points;
            std::vector<cv::Point2f> current_full_points;
            std::vector<cv::Point2f> visualization_old_points;
            std::vector<cv::Point2f> visualization_new_points;
            std::vector<float> visualization_confidence;
            std::vector<int32_t> visualization_status;
            previous_full_points.reserve(previous_points.size());
            current_full_points.reserve(current_points.size());
            visualization_old_points.reserve(previous_points.size());
            visualization_new_points.reserve(previous_points.size());
            visualization_confidence.reserve(previous_points.size());
            visualization_status.reserve(previous_points.size());
            const cv::Point2f roi_offset(static_cast<float>(roi.x), static_cast<float>(roi.y));
            constexpr float kConfidenceErrorScale = 0.08f;
            for(size_t i = 0; i < status.size(); ++i)
            {
                // LK runs on the 2x downscaled ROI, but diagnostics and affine fitting
                // remain in full-frame pixels so downstream stabilization math is unchanged.
                const cv::Point2f old_full = TrackingPointToFullFrame(previous_points[i], roi_offset);
                visualization_old_points.push_back(old_full);
                if(status[i] == 0)
                {
                    visualization_new_points.push_back(old_full);
                    visualization_confidence.push_back(0.0f);
                    visualization_status.push_back(0);
                    continue;
                }
                const cv::Point2f new_full = TrackingPointToFullFrame(current_points[i], roi_offset);
                previous_full_points.push_back(old_full);
                current_full_points.push_back(new_full);
                visualization_new_points.push_back(new_full);
                const float err = i < error.size() ? std::max(0.0f, error[i]) : 0.0f;
                const float confidence = 1.0f / (1.0f + err * kConfidenceErrorScale);
                visualization_confidence.push_back(std::clamp(confidence, 0.0f, 1.0f));
                visualization_status.push_back(1);
            }

            tracked_points = static_cast<int32_t>(previous_full_points.size());
            if(previous_full_points.size() >= 3)
            {
                // Outlier rejection is done in displacement space before affine fitting:
                // 1) compute per-track motion (dx, dy), 2) take the median motion as the
                // dominant camera movement, 3) keep only tracks inside a fixed gate around
                // that median on both axes. This drops independently moving objects and bad
                // LK tracks so estimateAffinePartial2D sees a mostly global-motion set.
                std::vector<float> disp_x;
                std::vector<float> disp_y;
                disp_x.reserve(previous_full_points.size());
                disp_y.reserve(previous_full_points.size());
                for(size_t i = 0; i < previous_full_points.size(); ++i)
                {
                    disp_x.push_back(current_full_points[i].x - previous_full_points[i].x);
                    disp_y.push_back(current_full_points[i].y - previous_full_points[i].y);
                }
                const size_t mid = disp_x.size() / 2u;
                std::nth_element(disp_x.begin(), disp_x.begin() + mid, disp_x.end());
                std::nth_element(disp_y.begin(), disp_y.begin() + mid, disp_y.end());
                const float median_dx = disp_x[mid];
                const float median_dy = disp_y[mid];

                std::vector<cv::Point2f> previous_filtered;
                std::vector<cv::Point2f> current_filtered;
                previous_filtered.reserve(previous_full_points.size());
                current_filtered.reserve(current_full_points.size());
                constexpr float kMedianGatePx = 10.0f;
                for(size_t i = 0; i < previous_full_points.size(); ++i)
                {
                    const float ddx = (current_full_points[i].x - previous_full_points[i].x) - median_dx;
                    const float ddy = (current_full_points[i].y - previous_full_points[i].y) - median_dy;
                    if(std::fabs(ddx) <= kMedianGatePx && std::fabs(ddy) <= kMedianGatePx)
                    {
                        previous_filtered.push_back(previous_full_points[i]);
                        current_filtered.push_back(current_full_points[i]);
                    }
                }
                if(previous_filtered.size() >= 3)
                {
                    previous_full_points.swap(previous_filtered);
                    current_full_points.swap(current_filtered);
                    tracked_points = static_cast<int32_t>(previous_full_points.size());
                }

                cv::Mat inliers;
                cv::Mat measured_transform = cv::estimateAffinePartial2D(previous_full_points, current_full_points, inliers);
                if(!measured_transform.empty())
                {
                    dx = static_cast<float>(measured_transform.at<double>(0, 2));
                    dy = static_cast<float>(measured_transform.at<double>(1, 2));
                    angle = static_cast<float>(std::atan2(measured_transform.at<double>(1, 0), measured_transform.at<double>(0, 0)));
                    measured_dx = dx;
                    measured_dy = dy;
                    measured_angle = angle;
                    inlier_count = cv::countNonZero(inliers);
                }
            }

            if(result != nullptr)
            {
                constexpr int32_t kMaxResultFeatures = 512;
                const int32_t count =
                    std::min(static_cast<int32_t>(visualization_old_points.size()), kMaxResultFeatures);
                result->feature_count = count;
                for(int32_t i = 0; i < count; ++i)
                {
                    result->feature_old_x[i] = visualization_old_points[i].x;
                    result->feature_old_y[i] = visualization_old_points[i].y;
                    result->feature_new_x[i] = visualization_new_points[i].x;
                    result->feature_new_y[i] = visualization_new_points[i].y;
                    result->feature_confidence[i] = visualization_confidence[i];
                    result->feature_status[i] = visualization_status[i];
                }
            }
        }

        // Phase 4: harden raw motion against weak fits/spikes, then smooth for visual stability.
        // Weak RANSAC fits (few inliers vs tracked points) produce erratic affines; skip those samples.
        const int32_t kMinTrackedPoints = 8;
        const int32_t min_inliers_for_fit = std::max(6, tracked_points / 5);
        if(tracked_points < kMinTrackedPoints || inlier_count < min_inliers_for_fit)
        {
            // Brief feature-drop windows are common on vibration blur; keep a decayed last valid motion
            // instead of dropping to identity for one frame (that manifests as visible shake rebound).
            constexpr float kWeakFitCarry = 0.85f;
            dx = stabilizer->last_valid_dx * kWeakFitCarry;
            dy = stabilizer->last_valid_dy * kWeakFitCarry;
            angle = stabilizer->last_valid_angle * kWeakFitCarry;
        }
        else
        {
            // Even accepted fits can spike one frame; clamp incremental motion before the smoother.
            constexpr float kMaxTransPerFramePx = 16.0f;
            constexpr float kMaxAnglePerFrameRad = 0.12f;
            dx = std::clamp(dx, -kMaxTransPerFramePx, kMaxTransPerFramePx);
            dy = std::clamp(dy, -kMaxTransPerFramePx, kMaxTransPerFramePx);
            angle = std::clamp(angle, -kMaxAnglePerFrameRad, kMaxAnglePerFrameRad);
        }

        // Reject/hold on sudden measured-vector spikes so single bad frames cannot pollute trajectory.
        if(tracked_points >= kMinTrackedPoints)
        {
            const float inlier_ratio =
                tracked_points > 0 ? static_cast<float>(inlier_count) / static_cast<float>(tracked_points) : 0.0f;
            float measured_jump = 0.0f;
            if(stabilizer->has_prev_measured)
            {
                measured_jump = std::hypot(measured_dx - stabilizer->prev_measured_dx,
                                           measured_dy - stabilizer->prev_measured_dy);
            }
            const float measured_mag = std::hypot(measured_dx, measured_dy);
            const bool bad_vector =
                measured_jump > 18.0f || measured_mag > 28.0f || (inlier_ratio < 0.45f && measured_mag > 10.0f);
            stabilizer->prev_measured_dx = measured_dx;
            stabilizer->prev_measured_dy = measured_dy;
            stabilizer->has_prev_measured = true;
            if(bad_vector)
            {
                stabilizer->reject_hold_frames = 2;
            }
        }
        if(stabilizer->reject_hold_frames > 0)
        {
            // Keep continuity during reject windows; forcing identity here creates visible rebound.
            constexpr float kRejectCarry = 0.92f;
            dx = stabilizer->last_valid_dx * kRejectCarry;
            dy = stabilizer->last_valid_dy * kRejectCarry;
            angle = stabilizer->last_valid_angle * kRejectCarry;
            stabilizer->reject_hold_frames--;
        }
        else
        {
            stabilizer->last_valid_dx = dx;
            stabilizer->last_valid_dy = dy;
            stabilizer->last_valid_angle = angle;
        }

        // Clamp per-frame derivative of accepted motion to suppress one-frame spikes that pass RANSAC.
        constexpr float kMaxDeltaDxDyPerFrame = 3.0f;
        constexpr float kMaxDeltaAnglePerFrame = 0.020f;
        if(stabilizer->has_prev_filtered)
        {
            dx = StepTowards(stabilizer->prev_filtered_dx, dx, kMaxDeltaDxDyPerFrame);
            dy = StepTowards(stabilizer->prev_filtered_dy, dy, kMaxDeltaDxDyPerFrame);
            angle = StepTowards(stabilizer->prev_filtered_angle, angle, kMaxDeltaAnglePerFrame);
        }
        stabilizer->has_prev_filtered = true;
        stabilizer->prev_filtered_dx = dx;
        stabilizer->prev_filtered_dy = dy;
        stabilizer->prev_filtered_angle = angle;

        // Low-pass frame-to-frame motion so zero-mean vibration does not wiggle the warp (playback of
        // shake-only clips stays visually locked on the ROI); slow pans still integrate through the EMA.
        constexpr float kIncrementEmaAlpha = 0.22f;
        stabilizer->inc_ema_x = kIncrementEmaAlpha * dx + (1.0f - kIncrementEmaAlpha) * stabilizer->inc_ema_x;
        stabilizer->inc_ema_y = kIncrementEmaAlpha * dy + (1.0f - kIncrementEmaAlpha) * stabilizer->inc_ema_y;
        stabilizer->inc_ema_angle =
            kIncrementEmaAlpha * angle + (1.0f - kIncrementEmaAlpha) * stabilizer->inc_ema_angle;
        dx = stabilizer->inc_ema_x;
        dy = stabilizer->inc_ema_y;
        angle = stabilizer->inc_ema_angle;

        stabilizer->trajectory_x += dx;
        stabilizer->trajectory_y += dy;
        stabilizer->trajectory_angle += angle;

        // Phase 5: maintain a bounded long-term trajectory and derive bounded compensation output.
        const bool should_reanchor =
            stabilizer->previous_gray_roi.size() != current_gray_roi.size() ||
            inlier_count < 10 ||
            (stabilizer->frame_count % 240) == 0;
        if(should_reanchor)
        {
            stabilizer->trajectory_x = 0.0f;
            stabilizer->trajectory_y = 0.0f;
            stabilizer->trajectory_angle = 0.0f;
        }

        // Lock around the initial view (strong anti-shake): compensate cumulative trajectory directly.
        // Small leak prevents indefinite drift when optical flow has slow bias over long clips.
        constexpr float kTrajectoryLeak = 0.9985f;
        stabilizer->trajectory_x *= kTrajectoryLeak;
        stabilizer->trajectory_y *= kTrajectoryLeak;
        stabilizer->trajectory_angle *= kTrajectoryLeak;

        constexpr float kCompensationGain = 0.75f;
        constexpr float kMaxCompensatedTransPx = 24.0f;
        constexpr float kMaxCompensatedAngleRad = 0.14f;
        constexpr float kMaxCompensationStepPx = 2.8f;
        constexpr float kMaxCompensationStepRad = 0.015f;
        const float target_dx = std::clamp(-stabilizer->trajectory_x * kCompensationGain,
                                           -kMaxCompensatedTransPx,
                                           kMaxCompensatedTransPx);
        const float target_dy = std::clamp(-stabilizer->trajectory_y * kCompensationGain,
                                           -kMaxCompensatedTransPx,
                                           kMaxCompensatedTransPx);
        const float target_angle = std::clamp(-stabilizer->trajectory_angle * kCompensationGain,
                                              -kMaxCompensatedAngleRad,
                                              kMaxCompensatedAngleRad);
        stabilizer->compensated_out_x =
            StepTowards(stabilizer->compensated_out_x, target_dx, kMaxCompensationStepPx);
        stabilizer->compensated_out_y =
            StepTowards(stabilizer->compensated_out_y, target_dy, kMaxCompensationStepPx);
        stabilizer->compensated_out_angle =
            StepTowards(stabilizer->compensated_out_angle, target_angle, kMaxCompensationStepRad);
        dx = stabilizer->compensated_out_x;
        dy = stabilizer->compensated_out_y;
        angle = stabilizer->compensated_out_angle;
        const float compensated_dx = dx;
        const float compensated_dy = dy;
        const float compensated_angle = angle;

        // Phase 6: build the final 2x3 transform matrix (optionally composed with zoom).
        cv::Matx23d transform(std::cos(angle),
                              -std::sin(angle),
                              dx,
                              std::sin(angle),
                              std::cos(angle),
                              dy);
        if(stabilizer->config.zoom_factor < 0.999f)
        {
            const double scale = 1.0 / static_cast<double>(stabilizer->config.zoom_factor);
            const double center_x = static_cast<double>(input->width) * 0.5;
            const double center_y = static_cast<double>(input->height) * 0.5;
            cv::Matx23d zoom(scale, 0.0, (1.0 - scale) * center_x, 0.0, scale, (1.0 - scale) * center_y);
            transform = cv::Matx23d(zoom(0, 0) * transform(0, 0) + zoom(0, 1) * transform(1, 0),
                                    zoom(0, 0) * transform(0, 1) + zoom(0, 1) * transform(1, 1),
                                    zoom(0, 0) * transform(0, 2) + zoom(0, 1) * transform(1, 2) + zoom(0, 2),
                                    zoom(1, 0) * transform(0, 0) + zoom(1, 1) * transform(1, 0),
                                    zoom(1, 0) * transform(0, 1) + zoom(1, 1) * transform(1, 1),
                                    zoom(1, 0) * transform(0, 2) + zoom(1, 1) * transform(1, 2) + zoom(1, 2));
        }
        const float motion_ms = ElapsedMs(motion_start);

        stabilizer->frame_count++;

        // Phase 7: publish diagnostics, measured motion, and final compensated transform.
        if(result != nullptr)
        {
            result->stabilized = 1;
            result->tracked_points = tracked_points;
            result->inliers = inlier_count;
            result->dx = dx;
            result->dy = dy;
            result->angle_radians = angle;
            result->feature_ms = stabilizer->last_feature_ms;
            result->motion_ms = motion_ms;
            result->transform_02 = static_cast<float>(transform(0, 2));
            result->transform_12 = static_cast<float>(transform(1, 2));
            result->measured_dx = measured_dx;
            result->measured_dy = measured_dy;
            result->measured_angle_radians = measured_angle;
            result->compensated_dx = compensated_dx;
            result->compensated_dy = compensated_dy;
            result->compensated_angle_radians = compensated_angle;
        }

        return 1;
    }
    catch(const cv::Exception&)
    {
        return 0;
    }
    catch(const std::exception&)
    {
        return 0;
    }
    catch(...)
    {
        return 0;
    }
}

//===================================================================================
//===================================================================================
// Prepares feature tracking input by reusing or rebuilding the half-resolution ROI
// grayscale image, then detecting Shi-Tomasi corners and caching them as the
// previous-point set that the next stabilization estimate consumes.
int32_t gs_vision_stabilizer_prepare_frame_features(
    GsVisionStabilizer* stabilizer,
    const GsVisionImage* input,
    float* feature_ms)
{
    try
    {
        if(stabilizer == nullptr || input == nullptr)
        {
            return 0;
        }

        const cv::Rect roi = BuildRoi(cv::Size(input->width, input->height), stabilizer->config.roi_divisor);
        cv::Mat current_gray_roi;
        const bool can_reuse_estimate_gray =
            stabilizer->last_estimate_input_data == input->data &&
            stabilizer->last_estimate_width == input->width &&
            stabilizer->last_estimate_height == input->height &&
            stabilizer->last_estimate_stride_bytes == input->stride_bytes &&
            stabilizer->last_estimate_format == input->format &&
            !stabilizer->current_gray.empty() &&
            stabilizer->current_gray.size() == roi.size();
        if(can_reuse_estimate_gray)
        {
            current_gray_roi = stabilizer->current_gray;
        }
        else
        {
            if(!ConvertRoiToDownscaledGray(*input, roi, current_gray_roi))
            {
                return 0;
            }
        }

        const StabilizerClock::time_point feature_start = StabilizerClock::now();
        std::vector<cv::Point2f> points;
        // Detect strong Shi-Tomasi corners in the downscaled ROI. Scale the spacing
        // down so configured distances keep the same meaning in full-frame pixels.
        const double min_distance =
            std::max(1.0, static_cast<double>(stabilizer->config.min_distance) / kTrackingPointScale);
        cv::goodFeaturesToTrack(current_gray_roi,
                                points,
                                stabilizer->config.max_corners,
                                stabilizer->config.quality_level,
                                min_distance);
        stabilizer->last_feature_ms = ElapsedMs(feature_start);
        stabilizer->cached_previous_points = std::move(points);
        if(feature_ms != nullptr)
        {
            *feature_ms = stabilizer->last_feature_ms;
        }
        return 1;
    }
    catch(const cv::Exception&)
    {
        return 0;
    }
    catch(const std::exception&)
    {
        return 0;
    }
    catch(...)
    {
        return 0;
    }
}
  