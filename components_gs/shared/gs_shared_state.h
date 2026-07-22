#pragma once

#include <string>

#include "../../components/common/Clock.h"
#include "core/gs_session_core.h"
#include "core/transport_kind.h"
#include "packets.h"

enum class ScreenAspectRatio : int
{
    STRETCH = 0,
    LETTERBOX = 1,
    ASPECT5X4 = 2,
    ASPECT4X3 = 3,
    ASPECT16X9 = 4,
    ASPECT16X10 = 5
};

//===================================================================================
//===================================================================================
// Holds the air-synchronized lens correction state used for GS preview and rendering.
struct LensCorrectionState
{
    bool enabled = false;
    int image_width = 0;
    int image_height = 0;
    double fx = 0.0;
    double fy = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    double k1 = 0.0;
    double k2 = 0.0;
    double k3 = 0.0;
    double p1 = 0.0;
    double p2 = 0.0;
};

//===================================================================================
//===================================================================================
// Holds GS image stabilization enable state, RC control channel, and OpenCV tracking parameters.
struct ImageStabilizationState
{
    bool enabled = false;
    bool debug = false;
    uint8_t rc_channel = 0;
    float roi_divisor = 6.0f;
    float zoom = 1.15f;
    float stabilization_decay = 0.05f;
    float limit_release_boost = 0.5f;
};

//===================================================================================
//===================================================================================
// Holds lightweight shader postprocessing controls for decoded MJPEG video.
struct PostprocessingState
{
    enum class PipelineMode : uint8_t
    {
        RGB565 = 0,
        RGB888 = 1,
    };

    bool jpeg_deblocking_enabled = true;
    uint8_t adaptive_dithering_level = 2; // 0 off, 1 low, 2 medium, 3 high
    PipelineMode pipeline_mode = PipelineMode::RGB888;
};

//===================================================================================
//===================================================================================
// Holds ground station runtime settings shared by platform-specific front ends.
struct TGroundstationConfig
{
    int socket_fd;
    int wifi_channel;  // 1...13
    uint8_t wifiBand = DEFAULT_GS_WIFI_BAND; // GS_WIFI_BAND_*
    ScreenAspectRatio screenAspectRatio;
    int txPower; //MIN_TX_POWER...MAX_TX_POWER
    bool stats;
    bool vrMode = false;
    bool vsync = true;
    bool screenFlipV = false;
    float screenZoom = 1.0f;
    float screenVrSeparation = 0.0f; // -30%...+30%, default 0
    float screenVrDistance = 1.5f; // meters, 1.0...3.0, used by Oculus Quest VR quad layer
    bool screenVrCurved = false; // false=flat quad, true=cylinder layer (Oculus)
    float screenVrCurvatureAngleDeg = 55.0f; // 30...85 degrees, central angle of cylinder
    uint8_t screenVrPassthroughLevel = 0; // 0=Off, 1..7 = 2%,5%,10%,20%,50%,75%,100% opacity (Oculus passthrough)
    float screenVrTiltDeg = 0.0f; // -45...+45 degrees, pitch of virtual screen (Oculus)
    std::string txInterface = "";
    std::string apfpvInterface = "";
    // Telemetry UART selection for the GS-side serial bridge.
    //   "none" = do not initialize any UART
    //   "auto" = let the platform pick (current default behavior)
    //   else   = platform-specific identifier (Linux: /dev/tty* path; Android: "productName (VID:PID)")
    std::string telemetryUart = "auto";
    gs::core::TransportKind transportKind = gs::core::TransportKind::RawBroadcast;
    uint16_t deviceId;
    uint16_t apfpvPreferredCameraId = 0;
    uint8_t GPIOKeysLayout = 0;
};

extern TGroundstationConfig s_groundstation_config;
extern LensCorrectionState s_lensCorrectionState;
extern ImageStabilizationState s_imageStabilizationState;
extern PostprocessingState s_postprocessingState;
extern Clock::time_point& s_last_packet_tp;
