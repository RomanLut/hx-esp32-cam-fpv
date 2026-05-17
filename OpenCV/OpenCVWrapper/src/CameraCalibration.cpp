#include "gs_vision_opencv_wrapper.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <exception>
#include <string>
#include <vector>

namespace
{
    thread_local std::string g_lastError;

    //===================================================================================
    //===================================================================================
    // Stores the grayscale image and dimensions used by calibration.
    struct GrayImageView
    {
        cv::Mat gray;
        int32_t width = 0;
        int32_t height = 0;
    };

    //===================================================================================
    //===================================================================================
    // Records the latest error message for callers that cannot use exceptions.
    void SetLastError(const std::string& message)
    {
        g_lastError = message;
    }

    //===================================================================================
    //===================================================================================
    // Clears the thread-local error message before a wrapper API call starts.
    void ClearLastError()
    {
        g_lastError.clear();
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
            default:
                return 0;
        }
    }

    //===================================================================================
    //===================================================================================
    // Converts the caller's raw image view into grayscale data used for corner detection.
    bool ConvertToGray(const GsVisionImage& image, GrayImageView& outImage)
    {
        if(image.data == nullptr)
        {
            SetLastError("Image data is null.");
            return false;
        }

        if(image.width <= 0 || image.height <= 0)
        {
            SetLastError("Image dimensions must be positive.");
            return false;
        }

        const int32_t bytesPerPixel = BytesPerPixel(image.format);
        if(bytesPerPixel == 0)
        {
            SetLastError("Unsupported image format.");
            return false;
        }

        if(image.stride_bytes < image.width * bytesPerPixel)
        {
            SetLastError("Image stride is smaller than the minimum row size.");
            return false;
        }

        const cv::Mat source(image.height, image.width, CV_8UC(bytesPerPixel), const_cast<uint8_t*>(image.data), image.stride_bytes);
        switch(image.format)
        {
            case GS_VISION_IMAGE_FORMAT_GRAY8:
                outImage.gray = source.clone();
                break;
            case GS_VISION_IMAGE_FORMAT_BGR8:
                cv::cvtColor(source, outImage.gray, cv::COLOR_BGR2GRAY);
                break;
            case GS_VISION_IMAGE_FORMAT_RGB8:
                cv::cvtColor(source, outImage.gray, cv::COLOR_RGB2GRAY);
                break;
            case GS_VISION_IMAGE_FORMAT_BGRA8:
                cv::cvtColor(source, outImage.gray, cv::COLOR_BGRA2GRAY);
                break;
            case GS_VISION_IMAGE_FORMAT_RGBA8:
                cv::cvtColor(source, outImage.gray, cv::COLOR_RGBA2GRAY);
                break;
            default:
                SetLastError("Unsupported image format.");
                return false;
        }

        outImage.width = image.width;
        outImage.height = image.height;
        return true;
    }

    //===================================================================================
    //===================================================================================
    // Validates calibration arguments before OpenCV receives them.
    bool ValidateCalibrationArguments(
        const GsVisionImage* images,
        int32_t imageCount,
        const GsVisionChessboardCalibrationConfig* config,
        GsVisionCameraCalibrationResult* result)
    {
        if(images == nullptr)
        {
            SetLastError("Image array is null.");
            return false;
        }

        if(imageCount <= 0)
        {
            SetLastError("Image count must be positive.");
            return false;
        }

        if(config == nullptr)
        {
            SetLastError("Calibration config is null.");
            return false;
        }

        if(result == nullptr)
        {
            SetLastError("Calibration result is null.");
            return false;
        }

        if(config->inner_corners_per_row <= 1 || config->inner_corners_per_column <= 1)
        {
            SetLastError("Chessboard must have at least 2 inner corners in each direction.");
            return false;
        }

        if(config->square_size <= 0.0f)
        {
            SetLastError("Chessboard square size must be positive.");
            return false;
        }

        return true;
    }

    //===================================================================================
    //===================================================================================
    // Builds one row of object-space chessboard points for OpenCV calibration.
    std::vector<cv::Point3f> BuildChessboardObjectPoints(const GsVisionChessboardCalibrationConfig& config)
    {
        std::vector<cv::Point3f> objectPoints;
        objectPoints.reserve(static_cast<size_t>(config.inner_corners_per_row * config.inner_corners_per_column));

        for(int32_t y = 0; y < config.inner_corners_per_column; ++y)
        {
            for(int32_t x = 0; x < config.inner_corners_per_row; ++x)
            {
                objectPoints.emplace_back(
                    static_cast<float>(x) * config.square_size,
                    static_cast<float>(y) * config.square_size,
                    0.0f);
            }
        }

        return objectPoints;
    }
}

//===================================================================================
//===================================================================================
// Reports details for the most recent wrapper call made on the current thread.
const char* gs_vision_get_last_error(void)
{
    return g_lastError.c_str();
}

//===================================================================================
//===================================================================================
// Calculates camera matrix and k1/k2/k3/p1/p2 coefficients from chessboard images.
int32_t gs_vision_calibrate_camera_from_chessboard_images(
    const GsVisionImage* images,
    int32_t image_count,
    const GsVisionChessboardCalibrationConfig* config,
    GsVisionCameraCalibrationResult* result)
{
    ClearLastError();

    try
    {
        if(!ValidateCalibrationArguments(images, image_count, config, result))
        {
            return 0;
        }

        *result = {};

        const cv::Size boardSize(config->inner_corners_per_row, config->inner_corners_per_column);
        const std::vector<cv::Point3f> singleObjectPoints = BuildChessboardObjectPoints(*config);
        std::vector<std::vector<cv::Point2f>> imagePoints;
        std::vector<std::vector<cv::Point3f>> objectPoints;
        cv::Size calibrationImageSize;

        for(int32_t imageIndex = 0; imageIndex < image_count; ++imageIndex)
        {
            GrayImageView grayImage;
            if(!ConvertToGray(images[imageIndex], grayImage))
            {
                return 0;
            }

            const cv::Size currentSize(grayImage.width, grayImage.height);
            if(calibrationImageSize.empty())
            {
                calibrationImageSize = currentSize;
            }
            else if(calibrationImageSize != currentSize)
            {
                SetLastError("All calibration images must have the same dimensions.");
                return 0;
            }

            std::vector<cv::Point2f> corners;
            const bool found = cv::findChessboardCorners(
                grayImage.gray,
                boardSize,
                corners,
                cv::CALIB_CB_ADAPTIVE_THRESH | cv::CALIB_CB_NORMALIZE_IMAGE);

            if(!found)
            {
                continue;
            }

            cv::cornerSubPix(
                grayImage.gray,
                corners,
                cv::Size(11, 11),
                cv::Size(-1, -1),
                cv::TermCriteria(cv::TermCriteria::EPS | cv::TermCriteria::COUNT, 30, 0.001));

            imagePoints.push_back(corners);
            objectPoints.push_back(singleObjectPoints);
        }

        if(imagePoints.empty())
        {
            SetLastError("No chessboard corners were detected in the provided images.");
            return 0;
        }

        cv::Mat cameraMatrix = cv::Mat::eye(3, 3, CV_64F);
        cv::Mat distortionCoefficients = cv::Mat::zeros(8, 1, CV_64F);
        std::vector<cv::Mat> rotationVectors;
        std::vector<cv::Mat> translationVectors;

        const double reprojectionError = cv::calibrateCamera(
            objectPoints,
            imagePoints,
            calibrationImageSize,
            cameraMatrix,
            distortionCoefficients,
            rotationVectors,
            translationVectors);

        result->reprojection_error = reprojectionError;
        result->images_used = static_cast<int32_t>(imagePoints.size());
        result->image_width = calibrationImageSize.width;
        result->image_height = calibrationImageSize.height;
        result->fx = cameraMatrix.at<double>(0, 0);
        result->fy = cameraMatrix.at<double>(1, 1);
        result->cx = cameraMatrix.at<double>(0, 2);
        result->cy = cameraMatrix.at<double>(1, 2);
        result->k1 = distortionCoefficients.at<double>(0);
        result->k2 = distortionCoefficients.at<double>(1);
        result->p1 = distortionCoefficients.at<double>(2);
        result->p2 = distortionCoefficients.at<double>(3);
        result->k3 = distortionCoefficients.at<double>(4);
        return 1;
    }
    catch(const cv::Exception& exception)
    {
        SetLastError(exception.what());
        return 0;
    }
    catch(const std::exception& exception)
    {
        SetLastError(exception.what());
        return 0;
    }
    catch(...)
    {
        SetLastError("Unknown calibration error.");
        return 0;
    }
}
