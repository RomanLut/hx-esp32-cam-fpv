#pragma once

#include <stdint.h>

#if defined(_WIN32)
#if defined(OPENCV_WRAPPER_BUILDING_LIBRARY)
#define GS_VISION_API __declspec(dllexport)
#else
#define GS_VISION_API __declspec(dllimport)
#endif
#else
#define GS_VISION_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C"
{
#endif

//===================================================================================
//===================================================================================
// Describes the byte layout used when passing frames into the OpenCV wrapper.
typedef enum GsVisionImageFormat
{
    GS_VISION_IMAGE_FORMAT_GRAY8 = 1,
    GS_VISION_IMAGE_FORMAT_BGR8 = 2,
    GS_VISION_IMAGE_FORMAT_RGB8 = 3,
    GS_VISION_IMAGE_FORMAT_BGRA8 = 4,
    GS_VISION_IMAGE_FORMAT_RGBA8 = 5,
    GS_VISION_IMAGE_FORMAT_RGB565 = 6
} GsVisionImageFormat;

//===================================================================================
//===================================================================================
// Provides a raw image view without transferring ownership of its memory.
typedef struct GsVisionImage
{
    const uint8_t* data;
    int32_t width;
    int32_t height;
    int32_t stride_bytes;
    GsVisionImageFormat format;
} GsVisionImage;

//===================================================================================
//===================================================================================
// Configures chessboard calibration using the number of inner corners per row and column.
typedef struct GsVisionChessboardCalibrationConfig
{
    int32_t inner_corners_per_row;
    int32_t inner_corners_per_column;
    float square_size;
} GsVisionChessboardCalibrationConfig;

//===================================================================================
//===================================================================================
// Stores OpenCV camera calibration output in a stable ABI-friendly layout.
typedef struct GsVisionCameraCalibrationResult
{
    double reprojection_error;
    int32_t images_used;
    int32_t image_width;
    int32_t image_height;
    double fx;
    double fy;
    double cx;
    double cy;
    double k1;
    double k2;
    double k3;
    double p1;
    double p2;
} GsVisionCameraCalibrationResult;

//===================================================================================
//===================================================================================
// Configures low-latency ROI-based video stabilization.
typedef struct GsVisionStabilizerConfig
{
    float roi_divisor;
    float zoom_factor;
    int32_t max_corners;
    float quality_level;
    float min_distance;
} GsVisionStabilizerConfig;

//===================================================================================
//===================================================================================
// Reports details from the most recent stabilization frame.
typedef struct GsVisionStabilizerFrameResult
{
    int32_t stabilized;           // 1 if a valid motion estimate was produced; 0 on the first (bootstrap) frame or on error
    int32_t tracked_points;       // optical-flow points that survived LK tracking and median-gate outlier rejection
    int32_t inliers;              // RANSAC inlier count from the robust affine fit; low values indicate a weak or rejected frame

    // Final compensated warp values ready to be applied to the frame.
    // These are the smoothed, trajectory-corrected outputs after EMA, spike rejection, and step-rate limiting.
    float dx;                     // compensated X translation in pixels (positive = shift right)
    float dy;                     // compensated Y translation in pixels (positive = shift down)
    float angle_radians;          // compensated rotation in radians

    float feature_ms;             // milliseconds spent in the last explicit Shi-Tomasi prepare call
    float motion_ms;              // milliseconds spent on optical flow, RANSAC fit, and the full smoothing pipeline

    // Final 2x3 affine matrix translation column, zoom-composed with the compensated warp.
    // Pass these directly into warpAffine or equivalent; they subsume both stabilization and zoom.
    float transform_02;           // element [0][2] of the 2x3 affine matrix: final X translation after zoom composition
    float transform_12;           // element [1][2] of the 2x3 affine matrix: final Y translation after zoom composition

    // Raw inter-frame motion from estimateAffinePartial2D, before any smoothing, clamping, or trajectory accumulation.
    // Useful for diagnosing sensor drift or sudden camera movement.
    float measured_dx;            // raw frame-to-frame X translation in pixels
    float measured_dy;            // raw frame-to-frame Y translation in pixels
    float measured_angle_radians; // raw frame-to-frame rotation in radians

    // Output of the trajectory-compensation stage: negated + gain-scaled cumulative trajectory,
    // step-rate-limited but not yet zoom-composed. Represents how much the warp is actively countering accumulated drift.
    float compensated_dx;            // anti-shake X correction in pixels before zoom composition
    float compensated_dy;            // anti-shake Y correction in pixels before zoom composition
    float compensated_angle_radians; // anti-shake rotation correction in radians before zoom composition

    // Per-feature visualization data for all corners seen by LK optical flow this frame (up to feature_count entries).
    // Coordinates are in full-frame pixel space (ROI offset already added back).
    float feature_old_x[512];    // X position of each feature in the previous frame
    float feature_old_y[512];    // Y position of each feature in the previous frame
    float feature_new_x[512];    // X position of each feature in the current frame; echoes old position if tracking was lost
    float feature_new_y[512];    // Y position of each feature in the current frame; echoes old position if tracking was lost
    float feature_confidence[512]; // per-feature quality in [0, 1] derived from LK reprojection error; 0 means lost
    int32_t feature_status[512]; // 1 if LK tracking succeeded for this feature, 0 if tracking was lost
    int32_t feature_count;       // number of valid entries in the feature arrays above
} GsVisionStabilizerFrameResult;

//===================================================================================
//===================================================================================
// Opaque state handle for the video stabilizer.
typedef struct GsVisionStabilizer GsVisionStabilizer;

//===================================================================================
//===================================================================================
// Reports details for the most recent wrapper call made on the current thread.
GS_VISION_API const char* gs_vision_get_last_error(void);

//===================================================================================
//===================================================================================
// Calculates camera matrix and k1/k2/k3/p1/p2 coefficients from chessboard images.
GS_VISION_API int32_t gs_vision_calibrate_camera_from_chessboard_images(
    const GsVisionImage* images,
    int32_t image_count,
    const GsVisionChessboardCalibrationConfig* config,
    GsVisionCameraCalibrationResult* result);

//===================================================================================
//===================================================================================
// Creates one stateful video stabilizer instance.
GS_VISION_API GsVisionStabilizer* gs_vision_stabilizer_create(const GsVisionStabilizerConfig* config);

//===================================================================================
//===================================================================================
// Destroys a video stabilizer instance created by gs_vision_stabilizer_create.
GS_VISION_API void gs_vision_stabilizer_destroy(GsVisionStabilizer* stabilizer);

//===================================================================================
//===================================================================================
// Clears all temporal state retained by a video stabilizer instance.
GS_VISION_API void gs_vision_stabilizer_reset(GsVisionStabilizer* stabilizer);

//===================================================================================
//===================================================================================
// Estimates stabilization motion without producing a CPU-warped output frame.
GS_VISION_API int32_t gs_vision_stabilizer_estimate_frame(
    GsVisionStabilizer* stabilizer,
    const GsVisionImage* input,
    GsVisionStabilizerFrameResult* result);

//===================================================================================
//===================================================================================
// Explicitly prepares Shi-Tomasi points on the provided frame for the next estimate call.
GS_VISION_API int32_t gs_vision_stabilizer_prepare_frame_features(
    GsVisionStabilizer* stabilizer,
    const GsVisionImage* input,
    float* feature_ms);

#ifdef __cplusplus
}
#endif
