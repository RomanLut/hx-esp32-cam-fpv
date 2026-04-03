#include "gs_runtime_overlay_state.h"

#include <cstdio>

#include "core/osd_menu_common.h"
#include "gs_top_overlay_shared.h"

namespace
{

int formatLocalGSRSSI(int8_t rssi)
{
    if (rssi == 0)
    {
        return 0;
    }
    return std::max(-99, static_cast<int>(rssi));
}

} // namespace

bool hasRuntimeOverlayContent(const RuntimeOverlayState& state)
{
    return gs::imgui::hasTopOverlayContent(state.air_link_text,
                                           state.gs_link_text,
                                           state.throughput_text,
                                           state.resolution_text,
                                           state.no_ping,
                                           state.sd_slow,
                                           state.air_record,
                                           state.gs_record,
                                           state.hq_dvr,
                                           state.show_gs_temp,
                                           state.show_air_temp,
                                           state.overheat,
                                           state.incompatible_firmware,
                                           state.osd_font_error,
                                           state.suspended);
}

void drawRuntimeOverlayStatus(const RuntimeOverlayState& state)
{
    gs::imgui::drawTopOverlayStatus(state.air_link_text,
                                    state.gs_link_text,
                                    state.wifi_queue_percent,
                                    state.wifi_queue_alert,
                                    state.throughput_text,
                                    state.resolution_text,
                                    state.video_fps,
                                    state.video_fps_alert,
                                    state.no_ping,
                                    state.sd_slow,
                                    state.air_record,
                                    state.gs_record,
                                    state.hq_dvr,
                                    state.show_gs_temp,
                                    state.gs_temp_text,
                                    state.show_air_temp,
                                    state.air_temp_text,
                                    state.overheat,
                                    state.incompatible_firmware,
                                    state.osd_font_error,
                                    state.suspended);
}

RuntimeOverlayState buildRuntimeOverlayState(const RuntimeOverlayBuildInput& input)
{
    RuntimeOverlayState state;
    if (input.config == nullptr || input.air_stats == nullptr)
    {
        return state;
    }

    char chip_buf[64];
    std::snprintf(chip_buf, sizeof(chip_buf), "AIR:%d", -static_cast<int>(input.air_stats->rssiDbm));
    state.air_link_text = chip_buf;

    if (input.gs_stats != nullptr)
    {
        if (!input.is_dual)
        {
            std::snprintf(chip_buf, sizeof(chip_buf), "GS:%d", formatLocalGSRSSI(input.gs_stats->rssiDbm[0]));
            state.gs_link_text = chip_buf;
        }
        else
        {
            std::snprintf(chip_buf,
                          sizeof(chip_buf),
                          "GS:%d/%d",
                          formatLocalGSRSSI(input.gs_stats->rssiDbm[0]),
                          formatLocalGSRSSI(input.gs_stats->rssiDbm[1]));
            state.gs_link_text = chip_buf;
        }
    }

    state.wifi_queue_percent = input.wifi_queue_percent;
    state.wifi_queue_alert = input.wifi_queue_alert;
    if (input.use_megabit_total)
    {
        std::snprintf(chip_buf, sizeof(chip_buf), "%.1fMb", input.throughput_total_bytes * 8.0f / (1024.0f * 1024.0f));
    }
    else
    {
        std::snprintf(chip_buf, sizeof(chip_buf), "%.1fMb", input.throughput_mbps);
    }
    state.throughput_text = chip_buf;
    state.resolution_text = gs::menu::getResolutionSummary(*input.config, input.is_ov5640);
    state.video_fps = input.video_fps;
    state.video_fps_alert = input.video_fps_alert;
    state.no_ping = input.no_ping;
    state.sd_slow = input.sd_slow;
    state.air_record = input.air_record;
    state.gs_record = input.gs_record;
    state.hq_dvr = input.hq_dvr;
    if (input.gs_temp_celsius >= 80.0f)
    {
        std::snprintf(chip_buf, sizeof(chip_buf), "GS:%02dC", static_cast<int>(input.gs_temp_celsius + 0.5f));
        state.show_gs_temp = true;
        state.gs_temp_text = chip_buf;
    }
    if (input.air_stats->temperature >= 110)
    {
        std::snprintf(chip_buf, sizeof(chip_buf), "Air:%02dC", static_cast<int>(input.air_stats->temperature));
        state.show_air_temp = true;
        state.air_temp_text = chip_buf;
    }
    state.overheat = input.air_stats->overheatTrottling != 0;
    state.incompatible_firmware = input.now - input.incompatible_firmware_time < std::chrono::milliseconds(5000);
    state.osd_font_error = input.osd_font_error;
    state.suspended = input.air_stats->suspended == 1;
    return state;
}
