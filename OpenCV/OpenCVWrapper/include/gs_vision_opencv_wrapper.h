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
    int32_t stabilized;
    int32_t tracked_points;
    int32_t inliers;
    float dx;
    float dy;
    float angle_radians;
    float total_ms;
    float convert_ms;
    float gray_ms;
    float feature_ms;
    float optical_flow_ms;
    float affine_ms;
    float first_warp_ms;
    float zoom_warp_ms;
    float store_ms;
    float output_ms;
    float transform_00;
    float transform_01;
    float transform_02;
    float transform_10;
    float transform_11;
    float transform_12;
    float measured_dx;
    float measured_dy;
    float measured_angle_radians;
    float compensated_dx;
    float compensated_dy;
    float compensated_angle_radians;
    float feature_old_x[512];
    float feature_old_y[512];
    float feature_new_x[512];
    float feature_new_y[512];
    float feature_confidence[512];
    int32_t feature_status[512];
    int32_t feature_count;
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

#ifdef __cplusplus
}
#endif
