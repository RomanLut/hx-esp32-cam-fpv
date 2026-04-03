#pragma once

#include <string>

#include "Clock.h"
#include "core/gs_session_core.h"
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
    std::string txInterface = "";
    uint16_t deviceId;
    uint8_t GPIOKeysLayout = 0;
};

extern TGroundstationConfig s_groundstation_config;
extern Clock::time_point& s_last_packet_tp;
