#pragma once

#include <string>

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
                          bool suspended);

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
                          bool suspended);
}
