#pragma once

#include <string>

#include "../../components/common/Clock.h"
#include "gs_runtime_state.h"
#include "packets.h"

namespace gs::imgui
{
//===================================================================================
//===================================================================================
// Carries top overlay data used only for drawing the status chips.
struct TopOverlayData
{
    Ground2Air_Config_Packet config = {};
    bool air_stats_valid = true;
    int air_rssi_dbm = 0;
    int air_temperature = 0;
    bool air_overheat = false;
    bool air_suspended = false;
    bool has_gs_stats = false;
    int8_t gs_rssi_dbm0 = 0;
    int8_t gs_rssi_dbm1 = 0;
    bool is_ov5640 = false;
    bool is_ov3660 = false;
    bool is_esp32 = false;
    bool is_dual = false;
    int wifi_queue_percent = 0;
    bool wifi_queue_alert = false;
    float throughput_mbps = 0.0f;
    int video_fps = 0;
    bool video_fps_alert = false;
    bool image_stabilization_enabled = false;
    bool no_ping = false;
    std::string transport_message;
    bool interference = false;
    bool sd_slow = false;
    bool air_record = false;
    bool gs_record = false;
    bool hq_dvr = false;
    int gs_thermal_status = 0;
    float gs_temp_celsius = 0.0f;
    int battery_percent = -1;   // -1 = unknown (Android-only)
    bool osd_font_error = false;
    Clock::time_point incompatible_firmware_time = Clock::time_point{};
    Clock::time_point now = Clock::time_point::max();
};

void drawTopOverlayStatus(const TopOverlayData& input, float overlay_width);
}
