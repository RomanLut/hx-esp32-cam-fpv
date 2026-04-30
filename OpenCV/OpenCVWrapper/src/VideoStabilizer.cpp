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
    float estimate_x = 0.0f;
    float estimate_y = 0.0f;
    float estimate_angle = 0.0f;
    float covariance_x = 1.0f;
    float covariance_y = 1.0f;
    float covariance_angle = 1.0f;
    int32_t frame_count = 0;
};

namespace
{
using StabilizerClock = std::chrono::steady_clock;

//===================================================================================
//===================================================================================
// Returns elapsed milliseconds since the provided timestamp.
float ElapsedMs(StabilizerClock::time_point start)
{
    return std::chrono::duration<float, std::milli>(StabilizerClock::now() - start).count();
}

//===================================================================================
//===================================================================================
// Returns the default stabilization tuning used when callers omit config values.
GsVisionStabilizerConfig BuildDefaultConfig()
{
    GsVisionStabilizerConfig config = {};
    config.roi_divisor = 3.5f;
    config.zoom_factor = 0.9f;
    config.process_variance = 0.03f;
    config.measurement_variance = 2.0f;
    config.max_corners = 400;
    config.quality_level = 0.01f;
    config.min_distance = 30.0f;
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
        if(input->process_variance > 0.0f)
        {
            config.process_variance = input->process_variance;
        }
        if(input->measurement_variance > 0.0f)
        {
            config.measurement_variance = input->measurement_variance;
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
// Converts only the requested ROI to grayscale for motion-estimation-only mode.
bool ConvertRoiToGray(const GsVisionImage& image, const cv::Rect& roi, cv::Mat& out_gray_roi)
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

    out_gray_roi.create(roi.height, roi.width, CV_8UC1);
    const cv::Mat source(image.height,
                         image.width,
                         CV_8UC(bytes_per_pixel),
                         const_cast<uint8_t*>(image.data),
                         image.stride_bytes);
    switch(image.format)
    {
        case GS_VISION_IMAGE_FORMAT_GRAY8:
            source(roi).copyTo(out_gray_roi);
            break;
        case GS_VISION_IMAGE_FORMAT_BGR8:
        case GS_VISION_IMAGE_FORMAT_RGB8:
        {
            const bool input_is_rgb = image.format == GS_VISION_IMAGE_FORMAT_RGB8;
            for(int32_t y = 0; y < roi.height; ++y)
            {
                const uint8_t* input_row = source.ptr(roi.y + y) + static_cast<size_t>(roi.x) * 3u;
                uint8_t* gray_row = out_gray_roi.ptr(y);
                for(int32_t x = 0; x < roi.width; ++x)
                {
                    const uint8_t c0 = input_row[x * 3];
                    const uint8_t g = input_row[x * 3 + 1];
                    const uint8_t c2 = input_row[x * 3 + 2];
                    const uint8_t r = input_is_rgb ? c0 : c2;
                    const uint8_t b = input_is_rgb ? c2 : c0;
                    gray_row[x] = static_cast<uint8_t>((77u * r + 150u * g + 29u * b + 128u) >> 8);
                }
            }
            break;
        }
        case GS_VISION_IMAGE_FORMAT_RGB565:
            for(int32_t y = 0; y < roi.height; ++y)
            {
                const uint8_t* input_row = source.ptr(roi.y + y) + static_cast<size_t>(roi.x) * 2u;
                uint8_t* gray_row = out_gray_roi.ptr(y);
                for(int32_t x = 0; x < roi.width; ++x)
                {
                    const uint16_t pixel = static_cast<uint16_t>(input_row[x * 2]) |
                                           (static_cast<uint16_t>(input_row[x * 2 + 1]) << 8);
                    const uint8_t r5 = static_cast<uint8_t>((pixel >> 11) & 0x1f);
                    const uint8_t g6 = static_cast<uint8_t>((pixel >> 5) & 0x3f);
                    const uint8_t b5 = static_cast<uint8_t>(pixel & 0x1f);
                    const uint8_t r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
                    const uint8_t g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
                    const uint8_t b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
                    gray_row[x] = static_cast<uint8_t>((77u * r + 150u * g + 29u * b + 128u) >> 8);
                }
            }
            break;
        case GS_VISION_IMAGE_FORMAT_BGRA8:
        case GS_VISION_IMAGE_FORMAT_RGBA8:
        {
            cv::Mat roi_bgr;
            cv::cvtColor(source(roi), roi_bgr, image.format == GS_VISION_IMAGE_FORMAT_BGRA8 ? cv::COLOR_BGRA2BGR : cv::COLOR_RGBA2BGR);
            cv::cvtColor(roi_bgr, out_gray_roi, cv::COLOR_BGR2GRAY);
            break;
        }
        default:
            return false;
    }

    return true;
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

//===================================================================================
//===================================================================================
// Applies one scalar Kalman-style smoothing step to a cumulative trajectory axis.
float SmoothAxis(float measurement,
                 float& estimate,
                 float& covariance,
                 const GsVisionStabilizerConfig& config)
{
    const float predicted_estimate = estimate;
    const float predicted_covariance = covariance + config.process_variance;
    const float kalman_gain = predicted_covariance / (predicted_covariance + config.measurement_variance);
    estimate = predicted_estimate + kalman_gain * (measurement - predicted_estimate);
    covariance = (1.0f - kalman_gain) * predicted_covariance;
    return estimate;
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
// Estimates stabilization motion without producing a CPU-warped output frame.
int32_t gs_vision_stabilizer_estimate_frame(
    GsVisionStabilizer* stabilizer,
    const GsVisionImage* input,
    GsVisionStabilizerFrameResult* result)
{
    try
    {
        const StabilizerClock::time_point total_start = StabilizerClock::now();
        if(stabilizer == nullptr || input == nullptr)
        {
            return 0;
        }

        const cv::Rect roi = BuildRoi(cv::Size(input->width, input->height), stabilizer->config.roi_divisor);
        const StabilizerClock::time_point convert_start = StabilizerClock::now();
        if(!ConvertRoiToGray(*input, roi, stabilizer->current_gray))
        {
            return 0;
        }
        const float convert_ms = ElapsedMs(convert_start);
        const cv::Mat& current_gray_roi = stabilizer->current_gray;

        if(stabilizer->previous_gray_roi.empty() ||
           stabilizer->previous_gray_roi.size() != current_gray_roi.size())
        {
            const StabilizerClock::time_point store_start = StabilizerClock::now();
            current_gray_roi.copyTo(stabilizer->previous_gray_roi);
            const float store_ms = ElapsedMs(store_start);
            if(result != nullptr)
            {
                *result = {};
                result->convert_ms = convert_ms;
                result->store_ms = store_ms;
                result->total_ms = ElapsedMs(total_start);
                result->transform_00 = 1.0f;
                result->transform_11 = 1.0f;
            }
            return 1;
        }

        std::vector<cv::Point2f> previous_points;
        const StabilizerClock::time_point feature_start = StabilizerClock::now();
        cv::goodFeaturesToTrack(stabilizer->previous_gray_roi,
                                previous_points,
                                stabilizer->config.max_corners,
                                stabilizer->config.quality_level,
                                stabilizer->config.min_distance);
        const float feature_ms = ElapsedMs(feature_start);

        float dx = 0.0f;
        float dy = 0.0f;
        float angle = 0.0f;
        int32_t tracked_points = 0;
        int32_t inlier_count = 0;
        float optical_flow_ms = 0.0f;
        float affine_ms = 0.0f;

        if(!previous_points.empty())
        {
            std::vector<cv::Point2f> current_points;
            std::vector<uint8_t> status;
            std::vector<float> error;
            const StabilizerClock::time_point optical_flow_start = StabilizerClock::now();
            cv::calcOpticalFlowPyrLK(stabilizer->previous_gray_roi,
                                     current_gray_roi,
                                     previous_points,
                                     current_points,
                                     status,
                                     error,
                                     cv::Size(15, 15),
                                     3,
                                     cv::TermCriteria(cv::TermCriteria::EPS | cv::TermCriteria::COUNT, 10, 0.03));
            optical_flow_ms = ElapsedMs(optical_flow_start);

            std::vector<cv::Point2f> previous_full_points;
            std::vector<cv::Point2f> current_full_points;
            previous_full_points.reserve(previous_points.size());
            current_full_points.reserve(current_points.size());
            const cv::Point2f roi_offset(static_cast<float>(roi.x), static_cast<float>(roi.y));
            for(size_t i = 0; i < status.size(); ++i)
            {
                if(status[i] == 0)
                {
                    continue;
                }
                previous_full_points.push_back(previous_points[i] + roi_offset);
                current_full_points.push_back(current_points[i] + roi_offset);
            }

            tracked_points = static_cast<int32_t>(previous_full_points.size());
            if(previous_full_points.size() >= 3)
            {
                cv::Mat inliers;
                const StabilizerClock::time_point affine_start = StabilizerClock::now();
                cv::Mat measured_transform = cv::estimateAffinePartial2D(previous_full_points, current_full_points, inliers);
                affine_ms = ElapsedMs(affine_start);
                if(!measured_transform.empty())
                {
                    dx = static_cast<float>(measured_transform.at<double>(0, 2));
                    dy = static_cast<float>(measured_transform.at<double>(1, 2));
                    angle = static_cast<float>(std::atan2(measured_transform.at<double>(1, 0), measured_transform.at<double>(0, 0)));
                    inlier_count = cv::countNonZero(inliers);
                }
            }
        }

        stabilizer->trajectory_x += dx;
        stabilizer->trajectory_y += dy;
        stabilizer->trajectory_angle += angle;

        const float smooth_x = SmoothAxis(stabilizer->trajectory_x,
                                          stabilizer->estimate_x,
                                          stabilizer->covariance_x,
                                          stabilizer->config);
        const float smooth_y = SmoothAxis(stabilizer->trajectory_y,
                                          stabilizer->estimate_y,
                                          stabilizer->covariance_y,
                                          stabilizer->config);
        const float smooth_angle = SmoothAxis(stabilizer->trajectory_angle,
                                              stabilizer->estimate_angle,
                                              stabilizer->covariance_angle,
                                              stabilizer->config);

        dx += smooth_x - stabilizer->trajectory_x;
        dy += smooth_y - stabilizer->trajectory_y;
        angle += smooth_angle - stabilizer->trajectory_angle;

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

        const StabilizerClock::time_point store_start = StabilizerClock::now();
        current_gray_roi.copyTo(stabilizer->previous_gray_roi);
        const float store_ms = ElapsedMs(store_start);
        stabilizer->frame_count++;

        if(result != nullptr)
        {
            result->stabilized = 1;
            result->tracked_points = tracked_points;
            result->inliers = inlier_count;
            result->dx = dx;
            result->dy = dy;
            result->angle_radians = angle;
            result->total_ms = ElapsedMs(total_start);
            result->convert_ms = convert_ms;
            result->gray_ms = 0.0f;
            result->feature_ms = feature_ms;
            result->optical_flow_ms = optical_flow_ms;
            result->affine_ms = affine_ms;
            result->first_warp_ms = 0.0f;
            result->zoom_warp_ms = 0.0f;
            result->store_ms = store_ms;
            result->output_ms = 0.0f;
            result->transform_00 = static_cast<float>(transform(0, 0));
            result->transform_01 = static_cast<float>(transform(0, 1));
            result->transform_02 = static_cast<float>(transform(0, 2));
            result->transform_10 = static_cast<float>(transform(1, 0));
            result->transform_11 = static_cast<float>(transform(1, 1));
            result->transform_12 = static_cast<float>(transform(1, 2));
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
