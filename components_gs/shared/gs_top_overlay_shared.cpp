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
// Draws the top overlay chips in a single horizontal strip. Returns the row height.
float drawOverlayChipStrip(const std::vector<OverlayChipSpec>& chips, float start_y)
{
    if (GImGui == nullptr || GImGui->CurrentWindow == nullptr || GImGui->Font == nullptr)
    {
        // Android surface reattach can briefly leave the overlay draw path with
        // no active ImGui window/font while the renderer rebuilds state.
        return 0.0f;
    }

    const float osd_scale = gs::menu::imgui::calcOsdScale(ImGui::GetIO().DisplaySize.y);
    const float resolved_height = std::max(20.0f, ImGui::GetIO().DisplaySize.y * 0.04f);
    float x = 0.0f;

    for (size_t i = 0; i < chips.size(); ++i)
    {
        const auto& chip = chips[i];
        if (chip.text.empty())
        {
            continue;
        }

        const ImVec2 text_size = ImGui::CalcTextSize(chip.text.c_str());
        const float chip_width = chip.width > 0.0f ? chip.width * osd_scale : std::max(44.0f, 16.0f + text_size.x);
        const ImVec4 bg = chip.alert ? ImVec4(0.54f, 0.29f, 0.29f, 0.80f)
                                     : ImVec4(0.42f, 0.42f, 0.42f, 0.80f);

        ImGui::SetCursorPos(ImVec2(x, start_y));
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Button, bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
        ImGui::Button(chip.text.c_str(), ImVec2(chip_width, resolved_height));
        ImGui::PopStyleColor(3);
        ImGui::PopID();

        x += chip_width + kOverlayChipGap * osd_scale;
    }

    return resolved_height;
}

} // namespace

//===================================================================================
//===================================================================================
// Builds and draws the runtime top overlay status chips.
void drawTopOverlayStatus(const TopOverlayData& input)
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

    if (input.no_ping) chips.push_back({"NO PING!", true, 0.0f});
    if (input.interference) chips.push_back({"CHANNEL CONGESTED!", true, 0.0f});
    if (input.sd_slow) chips.push_back({"SD SLOW!", true, 0.0f});
    if (input.air_record) chips.push_back({"AIR", true, 0.0f});
    if (input.gs_record) chips.push_back({"GS", true, 0.0f});
    if (input.hq_dvr) chips.push_back({"HQ DVR", true, 0.0f});
    if (show_gs_temp && !gs_temp_text.empty()) chips.push_back({gs_temp_text, true, 110.0f});
    if (show_air_temp && !air_temp_text.empty()) chips.push_back({air_temp_text, true, 137.0f});
    if (input.air_overheat) chips.push_back({"OVERHEAT!", true, 0.0f});
    if (input.now - input.incompatible_firmware_time < std::chrono::milliseconds(5000)) chips.push_back({"Incompatible Air Unit firmware. Please update!", true, 0.0f});
    if (input.osd_font_error) chips.push_back({"Displayport OSD Font Unexpected Format!", true, 0.0f});
    if (input.air_suspended) chips.push_back({"OFF", true, 0.0f});

    const float main_row_height = drawOverlayChipStrip(chips, 0.0f);
    if (!input.transport_message.empty())
    {
        drawOverlayChipStrip({{input.transport_message, true, 0.0f}}, main_row_height + kOverlayBannerGap);
    }
}
}
