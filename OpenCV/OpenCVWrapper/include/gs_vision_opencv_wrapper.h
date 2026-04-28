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
    GS_VISION_IMAGE_FORMAT_RGBA8 = 5
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

#ifdef __cplusplus
}
#endif
