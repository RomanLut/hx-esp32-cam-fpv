#pragma once

#include <string>

#include "../../components/common/Clock.h"
#include "gs_runtime_state.h"
#include "packets.h"

namespace gs::imgui
{
//===================================================================================
//===================================================================================
// Carries runtime data used for drawing the top status chips and link gauges.
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
    bool rc_period_warning = false;
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
    uint32_t video_received_packet_count = 0;
    uint32_t video_last_packet_index = 0;
    uint8_t video_fec_k = FEC_K;
    uint8_t video_fec_n = FEC_N;
    uint16_t gs_out_packet_rate = 0;
    uint16_t air_in_packet_rate = 0;
    Clock::time_point incompatible_firmware_time = Clock::time_point{};
    // The renderer can draw before the first runtime sync. Start with a real
    // steady-clock value so time-based samplers are not poisoned by a future
    // sentinel that makes every subsequent elapsed duration negative.
    Clock::time_point now = Clock::now();
};

void drawTopOverlayStatus(const TopOverlayData& input, float overlay_width);
void drawLinkQualityGauges(const TopOverlayData& input, float overlay_width, float overlay_height);
}
