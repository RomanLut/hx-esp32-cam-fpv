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
// Holds the reusable GS lens correction coefficients and enable state.
struct LensCorrectionState
{
    bool enabled = false;
    double k1 = 0.0;
    double k2 = 0.0;
    double k3 = 0.0;
    double p1 = 0.0;
    double p2 = 0.0;
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
extern Clock::time_point& s_last_packet_tp;
