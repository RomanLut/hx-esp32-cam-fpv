#include "gs_top_overlay_shared.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "gs_runtime_state.h"
#include "core/osd_menu_common.h"
#include "core/osd_menu_imgui_shared.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "utils/utils.h"

namespace gs::imgui
{
namespace
{

constexpr float kOverlayChipGap = 6.0f;
constexpr float kOverlayBannerGap = 6.0f;

//===================================================================================
//===================================================================================
// Describes one top overlay chip before it is drawn.
struct OverlayChipSpec
{
    std::string text;
    bool alert = false;
    float width = 0.0f;
};

//===================================================================================
//===================================================================================
// Draws the top overlay chips, wrapping to the next row when the next chip would overflow.
float drawOverlayChipStrip(const std::vector<OverlayChipSpec>& chips, float start_y, float overlay_width)
{
    if (GImGui == nullptr || GImGui->CurrentWindow == nullptr || GImGui->Font == nullptr)
    {
        // Android surface reattach can briefly leave the overlay draw path with
        // no active ImGui window/font while the renderer rebuilds state.
        return 0.0f;
    }

    const float osd_scale = gs::menu::imgui::calcOsdScale(ImGui::GetIO().DisplaySize.y);
    const float resolved_height = std::max(20.0f, ImGui::GetIO().DisplaySize.y * 0.04f);
    const float row_gap = kOverlayChipGap * osd_scale;
    const ImVec2 window_pos = ImGui::GetWindowPos();
    // Content-region/window-size helpers are unreliable here because this fullscreen
    // overlay uses absolute drawing and can be replayed into VR eye viewports.
    const float available_width = std::max(0.0f, overlay_width);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    float x = 0.0f;
    float y = start_y;
    bool drew_chip = false;

    for (size_t i = 0; i < chips.size(); ++i)
    {
        const auto& chip = chips[i];
        if (chip.text.empty())
        {
            continue;
        }

        const ImVec2 text_size = ImGui::CalcTextSize(chip.text.c_str());
        const float chip_width = chip.width > 0.0f ? chip.width * osd_scale : std::max(44.0f, 16.0f + text_size.x);
        if (x > 0.0f && available_width > 0.0f && x + chip_width > available_width)
        {
            x = 0.0f;
            y += resolved_height + row_gap;
        }

        const ImVec4 bg = chip.alert ? ImVec4(0.54f, 0.29f, 0.29f, 0.80f)
                                     : ImVec4(0.42f, 0.42f, 0.42f, 0.80f);

        // The fullscreen overlay window is shared with OSD/menu drawing, so draw
        // chips with absolute coordinates instead of relying on ImGui item cursor state.
        const ImVec2 chip_min(window_pos.x + x, window_pos.y + y);
        const ImVec2 chip_max(chip_min.x + chip_width, chip_min.y + resolved_height);
        const ImVec2 text_pos(chip_min.x + std::max(0.0f, (chip_width - text_size.x) * 0.5f),
                              chip_min.y + std::max(0.0f, (resolved_height - text_size.y) * 0.5f));
        draw_list->AddRectFilled(chip_min, chip_max, ImGui::ColorConvertFloat4ToU32(bg));
        draw_list->AddText(text_pos, ImGui::GetColorU32(ImGuiCol_Text), chip.text.c_str());

        x += chip_width + row_gap;
        drew_chip = true;
    }

    return drew_chip ? (y - start_y) + resolved_height : 0.0f;
}

} // namespace

//===================================================================================
//===================================================================================
// Builds and draws the runtime top overlay status chips.
void drawTopOverlayStatus(const TopOverlayData& input, float overlay_width)
{
    if (GImGui == nullptr || GImGui->CurrentWindow == nullptr || GImGui->Font == nullptr)
    {
        return;
    }

    std::vector<OverlayChipSpec> chips;
    char buf[64];
    std::string air_link_text;
    std::string gs_link_text;
    std::string throughput_text;
    std::string gs_temp_text;
    std::string air_temp_text;
    bool show_gs_temp = false;
    bool show_air_temp = false;

    std::snprintf(buf, sizeof(buf), "AIR:%d", -input.air_rssi_dbm);
    air_link_text = buf;

    if (input.has_gs_stats)
    {
        if (!input.is_dual)
        {
            std::snprintf(buf, sizeof(buf), "GS:%d", formatGSRSSI(input.gs_rssi_dbm0));
            gs_link_text = buf;
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "GS:%d/%d", formatGSRSSI(input.gs_rssi_dbm0), formatGSRSSI(input.gs_rssi_dbm1));
            gs_link_text = buf;
        }
    }

    std::snprintf(buf, sizeof(buf), "%.1fMb", input.throughput_mbps);
    throughput_text = buf;

    if (input.gs_temp_celsius >= 80.0f)
    {
        std::snprintf(buf, sizeof(buf), "GS:%02dC", static_cast<int>(input.gs_temp_celsius + 0.5f));
        show_gs_temp = true;
        gs_temp_text = buf;
    }
    if (input.air_temperature >= 110)
    {
        std::snprintf(buf, sizeof(buf), "Air:%02dC", input.air_temperature);
        show_air_temp = true;
        air_temp_text = buf;
    }

    if (!air_link_text.empty()) chips.push_back({air_link_text, false, 128.0f});
    if (!gs_link_text.empty())
    {
        chips.push_back({gs_link_text, false, gs_link_text.find('/') == std::string::npos ? 113.0f : 183.0f});
    }

    std::snprintf(buf, sizeof(buf), "%d%%", input.wifi_queue_percent);
    chips.push_back({buf, input.wifi_queue_alert, 55.0f});

    if (!throughput_text.empty()) chips.push_back({throughput_text, false, 90.0f});
    const std::string resolution_text = gs::menu::getResolutionSummary(input.config, input.is_ov5640);
    if (!resolution_text.empty()) chips.push_back({resolution_text, false, 0.0f});

    std::snprintf(buf, sizeof(buf), "%02d", input.video_fps);
    chips.push_back({buf, input.video_fps_alert, 45.0f});

    if (input.battery_percent >= 0)
    {
        std::snprintf(buf, sizeof(buf), "BAT:%d%%", input.battery_percent);
        chips.push_back({buf, input.battery_percent < 30, 0.0f});
    }

    if (input.no_ping) chips.push_back({"NO PING!", true, 0.0f});
    if (input.interference) chips.push_back({"CHANNEL CONGESTED!", true, 0.0f});
    if (input.sd_slow) chips.push_back({"SD SLOW!", true, 0.0f});
    if (input.air_record) chips.push_back({"AIR", true, 0.0f});
    if (input.gs_record) chips.push_back({"GS", true, 0.0f});
    if (input.hq_dvr) chips.push_back({"HQ DVR", true, 0.0f});
    if (show_gs_temp && !gs_temp_text.empty()) chips.push_back({gs_temp_text, true, 110.0f});
    if (input.gs_thermal_status == 3) chips.push_back({"GS HOT", true, 0.0f});
    if (input.gs_thermal_status >= 4) chips.push_back({"GS OVERHEAT!", true, 0.0f});
    if (show_air_temp && !air_temp_text.empty()) chips.push_back({air_temp_text, true, 137.0f});
    if (input.air_overheat) chips.push_back({"OVERHEAT!", true, 0.0f});
    if (input.now - input.incompatible_firmware_time < std::chrono::milliseconds(5000)) chips.push_back({"Incompatible Air Unit firmware. Please update!", true, 0.0f});
    if (input.osd_font_error) chips.push_back({"Displayport OSD Font Unexpected Format!", true, 0.0f});
    if (input.air_suspended) chips.push_back({"OFF", true, 0.0f});

    const float main_row_height = drawOverlayChipStrip(chips, 0.0f, overlay_width);
    if (!input.transport_message.empty())
    {
        drawOverlayChipStrip({{input.transport_message, true, 0.0f}}, main_row_height + kOverlayBannerGap, overlay_width);
    }
}
}
