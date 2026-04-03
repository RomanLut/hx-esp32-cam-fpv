#pragma once

#include <string>

#include "Clock.h"
#include "gs_stats.h"
#include "core/stats_panel_shared.h"
#include "packets.h"
#include "stats.h"

struct RuntimeOverlayState
{
    std::string air_link_text;
    std::string gs_link_text;
    int wifi_queue_percent = 0;
    bool wifi_queue_alert = false;
    std::string throughput_text;
    std::string resolution_text;
    int video_fps = 0;
    bool video_fps_alert = false;
    bool no_ping = false;
    bool sd_slow = false;
    bool air_record = false;
    bool gs_record = false;
    bool hq_dvr = false;
    bool show_gs_temp = false;
    std::string gs_temp_text;
    bool show_air_temp = false;
    std::string air_temp_text;
    bool overheat = false;
    bool incompatible_firmware = false;
    bool osd_font_error = false;
    bool suspended = false;
    bool stats_visible = false;
    gs::stats::FullscreenStatsSnapshot stats_snapshot = {};
};

struct RuntimeOverlayBuildInput
{
    const Ground2Air_Config_Packet* config = nullptr;
    const AirStats* air_stats = nullptr;
    const GSStats* gs_stats = nullptr;
    bool is_ov5640 = false;
    bool is_dual = false;
    int wifi_queue_percent = 0;
    bool wifi_queue_alert = false;
    int throughput_total_bytes = 0;
    float throughput_mbps = 0.0f;
    bool use_megabit_total = false;
    int video_fps = 0;
    bool video_fps_alert = false;
    bool no_ping = false;
    bool sd_slow = false;
    bool air_record = false;
    bool gs_record = false;
    bool hq_dvr = false;
    float gs_temp_celsius = 0.0f;
    bool osd_font_error = false;
    Clock::time_point incompatible_firmware_time = Clock::time_point::min();
    Clock::time_point now = Clock::time_point::min();
};

bool hasRuntimeOverlayContent(const RuntimeOverlayState& state);
void drawRuntimeOverlayStatus(const RuntimeOverlayState& state);
RuntimeOverlayState buildRuntimeOverlayState(const RuntimeOverlayBuildInput& input);
