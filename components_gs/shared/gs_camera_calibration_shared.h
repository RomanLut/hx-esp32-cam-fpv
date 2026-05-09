#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "../../OpenCV/OpenCVWrapper/include/gs_vision_opencv_wrapper.h"
#include "gs_shared_state.h"
#include "imgui.h"

namespace gs::calibration
{

constexpr int kRequiredCalibrationFrames = 10;

//===================================================================================
//===================================================================================
// Stores one captured frame and pixel format for camera calibration.
struct CalibrationFrame
{
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    int stride = 0;
    GsVisionImageFormat format = GS_VISION_IMAGE_FORMAT_RGB8;
};

//===================================================================================
//===================================================================================
// Tracks calibration mode state and captured frames.
struct CalibrationState
{
    bool active = false;
    bool capture_requested = false;
    bool finish_requested = false;
    bool completed_successfully = false;
    std::string status_message;
    std::vector<CalibrationFrame> frames;
};

bool isActive();
bool wantsLensCorrectionDisabled();
int capturedFrameCount();
void begin();
void cancel();
void requestCapture();
bool handleCalibrationKeysFromImGui();
void captureReadyFrame(const uint8_t* pixels,
                       size_t size,
                       int width,
                       int height,
                       int stride,
                       GsVisionImageFormat format);
void captureReadyRgbFrame(const uint8_t* pixels,
                          size_t size,
                          int width,
                          int height,
                          int stride);
void captureReadyRgb565Frame(const uint8_t* pixels,
                             size_t size,
                             int width,
                             int height,
                             int stride);
bool consumeFinishRequest(bool& completed_successfully);
bool applyCapturedCalibrationToLensCorrection(LensCorrectionState& state);
void drawCalibrationOverlay(float width, float height);

} // namespace gs::calibration
