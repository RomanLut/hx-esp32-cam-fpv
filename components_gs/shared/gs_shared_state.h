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
// Holds reusable GS lens correction intrinsics, coefficients, and enable state.
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
    uint8_t rc_channel = 0;
    float roi_divisor = 3.5f;
    float zoom_factor = 0.9f;
    float process_var = 0.03f;
    float measurement_var = 2.0f;
    int max_corners = 400;
    float quality_level = 0.01f;
    float min_distance = 30.0f;
};

//===================================================================================
//===================================================================================
// Holds lightweight shader postprocessing controls for decoded MJPEG video.
struct PostprocessingState
{
    bool jpeg_deblocking_enabled = true;
    uint8_t adaptive_dithering_level = 2; // 0 off, 1 low, 2 medium, 3 high
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
    std::string txInterface = "";
    std::string apfpvInterface = "";
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
