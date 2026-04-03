#include "gs_top_overlay_shared.h"

#include <cstdio>
#include <vector>

#include "gs_imgui_overlay_shared.h"

namespace gs::imgui
{
bool hasTopOverlayContent(const std::string& air_link_text,
                          const std::string& gs_link_text,
                          const std::string& throughput_text,
                          const std::string& resolution_text,
                          bool no_ping,
                          bool sd_slow,
                          bool air_record,
                          bool gs_record,
                          bool hq_dvr,
                          bool show_gs_temp,
                          bool show_air_temp,
                          bool overheat,
                          bool incompatible_firmware,
                          bool osd_font_error,
                          bool suspended)
{
    return !air_link_text.empty() || !gs_link_text.empty() || !throughput_text.empty() ||
           !resolution_text.empty() || no_ping || sd_slow || air_record ||
           gs_record || hq_dvr || show_gs_temp || show_air_temp ||
           overheat || incompatible_firmware || osd_font_error || suspended;
}

void drawTopOverlayStatus(const std::string& air_link_text,
                          const std::string& gs_link_text,
                          int wifi_queue_percent,
                          bool wifi_queue_alert,
                          const std::string& throughput_text,
                          const std::string& resolution_text,
                          int video_fps,
                          bool video_fps_alert,
                          bool no_ping,
                          bool sd_slow,
                          bool air_record,
                          bool gs_record,
                          bool hq_dvr,
                          bool show_gs_temp,
                          const std::string& gs_temp_text,
                          bool show_air_temp,
                          const std::string& air_temp_text,
                          bool overheat,
                          bool incompatible_firmware,
                          bool osd_font_error,
                          bool suspended)
{
    std::vector<OverlayChipSpec> chips;
    char buf[32];

    if (!air_link_text.empty()) chips.push_back({air_link_text, false, 128.0f});
    if (!gs_link_text.empty())
    {
        chips.push_back({gs_link_text, false, gs_link_text.find('/') == std::string::npos ? 113.0f : 183.0f});
    }

    std::snprintf(buf, sizeof(buf), "%d%%", wifi_queue_percent);
    chips.push_back({buf, wifi_queue_alert, 55.0f});

    if (!throughput_text.empty()) chips.push_back({throughput_text, false, 90.0f});
    if (!resolution_text.empty()) chips.push_back({resolution_text, false, 0.0f});

    std::snprintf(buf, sizeof(buf), "%02d", video_fps);
    chips.push_back({buf, video_fps_alert, 45.0f});

    if (no_ping) chips.push_back({"NO PING!", true, 0.0f});
    if (sd_slow) chips.push_back({"SD SLOW!", true, 0.0f});
    if (air_record) chips.push_back({"AIR", true, 0.0f});
    if (gs_record) chips.push_back({"GS", true, 0.0f});
    if (hq_dvr) chips.push_back({"HQ DVR", true, 0.0f});
    if (show_gs_temp && !gs_temp_text.empty()) chips.push_back({gs_temp_text, true, 110.0f});
    if (show_air_temp && !air_temp_text.empty()) chips.push_back({air_temp_text, true, 137.0f});
    if (overheat) chips.push_back({"OVERHEAT!", true, 0.0f});
    if (incompatible_firmware) chips.push_back({"Incompatible Air Unit firmware. Please update!", true, 0.0f});
    if (osd_font_error) chips.push_back({"Displayport OSD Font Unexpected Format!", true, 0.0f});
    if (suspended) chips.push_back({"OFF", true, 0.0f});

    drawOverlayChipStrip(chips, 0.0f, 0.0f, 0.0f, 0.0f);
}
}
