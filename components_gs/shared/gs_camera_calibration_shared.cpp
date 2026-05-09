#include "gs_camera_calibration_shared.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "Log.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "../../OpenCV/OpenCVWrapper/include/gs_vision_opencv_wrapper.h"

namespace
{
std::mutex s_calibration_mutex;
gs::calibration::CalibrationState s_calibration_state;

using GetLastErrorFn = const char* (*)(void);
using CalibrateFn = int32_t (*)(
    const GsVisionImage* images,
    int32_t image_count,
    const GsVisionChessboardCalibrationConfig* config,
    GsVisionCameraCalibrationResult* result);

//===================================================================================
//===================================================================================
// Holds the lazily loaded OpenCV wrapper API.
struct OpenCvWrapperApi
{
#if defined(_WIN32)
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif
    GetLastErrorFn get_last_error = nullptr;
    CalibrateFn calibrate = nullptr;
    bool attempted = false;
    std::string error;
};

OpenCvWrapperApi s_opencv_api;

//===================================================================================
//===================================================================================
// Rounds a coefficient to the precision shown by the coefficients menu.
double roundCalibrationCoefficient(double value)
{
    constexpr double scale = 1000000.0;
    return std::round(value * scale) / scale;
}

//===================================================================================
//===================================================================================
// Adds one possible OpenCV wrapper library name or path to the search list.
void addCandidatePath(std::vector<std::string>& candidates, const char* path)
{
    if (path != nullptr && path[0] != 0)
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
T loadOpenCvSymbol(OpenCvWrapperApi& api, const char* name)
{
#if defined(_WIN32)
    return reinterpret_cast<T>(GetProcAddress(api.handle, name));
#else
    return reinterpret_cast<T>(dlsym(api.handle, name));
#endif
}

//===================================================================================
//===================================================================================
// Loads the OpenCV wrapper on demand so missing prebuilts do not break startup.
OpenCvWrapperApi& loadOpenCvWrapper()
{
    if (s_opencv_api.attempted)
    {
        return s_opencv_api;
    }

    s_opencv_api.attempted = true;
    const std::vector<std::string> candidates = buildOpenCvWrapperCandidates();
    for (const std::string& candidate : candidates)
    {
#if defined(_WIN32)
        s_opencv_api.handle = LoadLibraryA(candidate.c_str());
#else
        s_opencv_api.handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (s_opencv_api.handle == nullptr)
        {
            continue;
        }

        s_opencv_api.get_last_error = loadOpenCvSymbol<GetLastErrorFn>(
            s_opencv_api,
            "gs_vision_get_last_error");
        s_opencv_api.calibrate = loadOpenCvSymbol<CalibrateFn>(
            s_opencv_api,
            "gs_vision_calibrate_camera_from_chessboard_images");

        if (s_opencv_api.get_last_error != nullptr && s_opencv_api.calibrate != nullptr)
        {
            return s_opencv_api;
        }

        s_opencv_api.error = "OpenCV wrapper is missing calibration symbols.";
        return s_opencv_api;
    }

    s_opencv_api.error = "OpenCV wrapper library was not found.";
    return s_opencv_api;
}

//===================================================================================
//===================================================================================
// Returns bytes-per-pixel for formats accepted by calibration capture.
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
// Finishes calibration mode, returning to the Lens Correction menu on the next draw.
void finishCalibrationLocked(bool completed_successfully, const std::string& status_message)
{
    s_calibration_state.active = false;
    s_calibration_state.capture_requested = false;
    s_calibration_state.finish_requested = true;
    s_calibration_state.completed_successfully = completed_successfully;
    s_calibration_state.status_message = status_message;
}

//===================================================================================
//===================================================================================
// Appends a captured frame and finishes when the required count is reached.
void appendCapturedFrameLocked(gs::calibration::CalibrationFrame&& frame)
{
    if (!s_calibration_state.active ||
        s_calibration_state.frames.size() >= gs::calibration::kRequiredCalibrationFrames)
    {
        return;
    }

    s_calibration_state.frames.push_back(std::move(frame));
    s_calibration_state.capture_requested = false;

    if (s_calibration_state.frames.size() >= gs::calibration::kRequiredCalibrationFrames)
    {
        finishCalibrationLocked(true, "Captured calibration frames.");
    }
}

//===================================================================================
//===================================================================================
// Draws one centered text line using the current ImGui font scale.
void drawCenteredLine(ImDrawList* draw_list,
                      const char* text,
                      float width,
                      float center_y,
                      ImU32 color)
{
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    draw_list->AddText(ImVec2((width - text_size.x) * 0.5f, center_y), color, text);
}
}

namespace gs::calibration
{

//===================================================================================
//===================================================================================
// Returns true while the full-screen calibration prompt owns capture and cancel input.
bool isActive()
{
    std::lock_guard<std::mutex> lock(s_calibration_mutex);
    return s_calibration_state.active;
}

//===================================================================================
//===================================================================================
// Returns true when render-time lens correction must be bypassed.
bool wantsLensCorrectionDisabled()
{
    return isActive();
}

//===================================================================================
//===================================================================================
// Returns the number of currently captured calibration frames.
int capturedFrameCount()
{
    std::lock_guard<std::mutex> lock(s_calibration_mutex);
    return static_cast<int>(s_calibration_state.frames.size());
}

//===================================================================================
//===================================================================================
// Starts camera calibration capture mode and clears prior captured frames.
void begin()
{
    std::lock_guard<std::mutex> lock(s_calibration_mutex);
    s_calibration_state = {};
    s_calibration_state.active = true;
}

//===================================================================================
//===================================================================================
// Cancels calibration mode without applying coefficients.
void cancel()
{
    std::lock_guard<std::mutex> lock(s_calibration_mutex);
    if (s_calibration_state.active)
    {
        finishCalibrationLocked(false, "Calibration cancelled.");
    }
}

//===================================================================================
//===================================================================================
// Requests capture of the next decoded frame that is ready for drawing.
void requestCapture()
{
    std::lock_guard<std::mutex> lock(s_calibration_mutex);
    if (s_calibration_state.active &&
        s_calibration_state.frames.size() < kRequiredCalibrationFrames)
    {
        s_calibration_state.capture_requested = true;
    }
}

//===================================================================================
//===================================================================================
// Handles calibration-specific capture and cancel keys before normal recording hotkeys.
bool handleCalibrationKeysFromImGui()
{
    if (!isActive())
    {
        return false;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        cancel();
        return true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_R, false) ||
        ImGui::IsKeyPressed(ImGuiKey_G, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
    {
        requestCapture();
        return true;
    }

    return true;
}

//===================================================================================
//===================================================================================
// Captures one frame with explicit format when calibration mode waits for capture.
void captureReadyFrame(const uint8_t* pixels,
                       size_t size,
                       int width,
                       int height,
                       int stride,
                       GsVisionImageFormat format)
{
    const int bytes_per_pixel = bytesPerPixelForFormat(format);
    if (pixels == nullptr ||
        width <= 0 ||
        height <= 0 ||
        bytes_per_pixel <= 0 ||
        stride < width * bytes_per_pixel)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(s_calibration_mutex);
    if (!s_calibration_state.active || !s_calibration_state.capture_requested)
    {
        return;
    }

    const size_t required_size = static_cast<size_t>(stride) * static_cast<size_t>(height);
    if (size < required_size)
    {
        return;
    }

    CalibrationFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.format = format;
    frame.pixels.resize(static_cast<size_t>(frame.stride) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y)
    {
        std::memcpy(frame.pixels.data() + static_cast<size_t>(y) * frame.stride,
                    pixels + static_cast<size_t>(y) * stride,
                    static_cast<size_t>(frame.stride));
    }

    appendCapturedFrameLocked(std::move(frame));
}

//===================================================================================
//===================================================================================
// Captures an RGB24 frame when calibration mode is waiting for the next drawn frame.
void captureReadyRgbFrame(const uint8_t* pixels,
                          size_t size,
                          int width,
                          int height,
                          int stride)
{
    captureReadyFrame(pixels, size, width, height, stride, GS_VISION_IMAGE_FORMAT_RGB8);
}

//===================================================================================
//===================================================================================
// Captures an RGB565 frame when calibration mode is waiting for the next drawn frame.
void captureReadyRgb565Frame(const uint8_t* pixels,
                             size_t size,
                             int width,
                             int height,
                             int stride)
{
    captureReadyFrame(pixels, size, width, height, stride, GS_VISION_IMAGE_FORMAT_RGB565);
}

//===================================================================================
//===================================================================================
// Consumes the request to return from calibration mode to the Lens Correction menu.
bool consumeFinishRequest(bool& completed_successfully)
{
    std::lock_guard<std::mutex> lock(s_calibration_mutex);
    if (!s_calibration_state.finish_requested)
    {
        return false;
    }

    completed_successfully = s_calibration_state.completed_successfully;
    s_calibration_state.finish_requested = false;
    return true;
}

//===================================================================================
//===================================================================================
// Runs OpenCV calibration using captured frame formats and copies coefficients into lens correction state.
bool applyCapturedCalibrationToLensCorrection(LensCorrectionState& state)
{
    std::vector<CalibrationFrame> frames;
    {
        std::lock_guard<std::mutex> lock(s_calibration_mutex);
        frames = s_calibration_state.frames;
    }

    if (frames.empty())
    {
        LOGW("Camera calibration requested without captured frames.");
        return false;
    }

    std::vector<GsVisionImage> images;
    images.reserve(frames.size());
    for (const CalibrationFrame& frame : frames)
    {
        images.push_back({
            frame.pixels.data(),
            frame.width,
            frame.height,
            frame.stride,
            frame.format
        });
    }

    OpenCvWrapperApi& api = loadOpenCvWrapper();
    if (api.calibrate == nullptr)
    {
        LOGE("Camera calibration unavailable: {}", api.error);
        return false;
    }

    // The first UI pass uses the common 9x6 inner-corner calibration board.
    // The wrapper accepts normalized object units, so square size can be 1.
    const GsVisionChessboardCalibrationConfig config = {
        9,
        6,
        1.0f
    };
    GsVisionCameraCalibrationResult result = {};
    if (api.calibrate(images.data(),
                      static_cast<int32_t>(images.size()),
                      &config,
                      &result) == 0)
    {
        const char* wrapper_error = api.get_last_error != nullptr ? api.get_last_error() : nullptr;
        LOGE("Camera calibration failed: {}", wrapper_error != nullptr ? wrapper_error : "unknown error");
        return false;
    }

    state.enabled = true;
    state.image_width = result.image_width;
    state.image_height = result.image_height;
    state.fx = result.fx;
    state.fy = result.fy;
    state.cx = result.cx;
    state.cy = result.cy;
    state.k1 = roundCalibrationCoefficient(result.k1);
    state.k2 = roundCalibrationCoefficient(result.k2);
    state.k3 = roundCalibrationCoefficient(result.k3);
    state.p1 = roundCalibrationCoefficient(result.p1);
    state.p2 = roundCalibrationCoefficient(result.p2);
    LOGI("Camera calibration applied frames={} used={} reprojection_error={:.6f} fx={:.3f} fy={:.3f} cx={:.3f} cy={:.3f} k1={:.6f} k2={:.6f} k3={:.6f} p1={:.6f} p2={:.6f}",
         static_cast<int>(frames.size()),
         static_cast<int>(result.images_used),
         result.reprojection_error,
         state.fx,
         state.fy,
         state.cx,
         state.cy,
         state.k1,
         state.k2,
         state.k3,
         state.p1,
         state.p2);
    return true;
}

//===================================================================================
//===================================================================================
// Draws the full-screen centered calibration instructions.
void drawCalibrationOverlay(float width, float height)
{
    if (!isActive())
    {
        return;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (draw_list == nullptr || width <= 0.0f || height <= 0.0f)
    {
        return;
    }

    const int frames = capturedFrameCount();
    const ImU32 dim = IM_COL32(0, 0, 0, 128);
    const ImU32 text = IM_COL32(255, 255, 255, 255);
    draw_list->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(width, height), dim);

    const float line_height = ImGui::GetTextLineHeightWithSpacing();
    const float start_y = (height - line_height * 10.0f) * 0.5f;
    std::array<std::string, 10> lines = {
        "Camera Calibration",
        "",
        "Place the calibration chessboard",
        "at varying angles and positions within the frame.",
        "Make sure the entire board is visible in each frame.",
        "Board should occupy more than 40% of screen.",
        "Make sure to place chessboard near the screen edges also.",
        "Press Enter or any REC button to capture a frame.",
        "",
        "Frames captured: " + std::to_string(frames) + "/" + std::to_string(kRequiredCalibrationFrames)
    };

    for (size_t i = 0; i < lines.size(); ++i)
    {
        drawCenteredLine(draw_list,
                         lines[i].c_str(),
                         width,
                         start_y + static_cast<float>(i) * line_height,
                         text);
    }
}

} // namespace gs::calibration
