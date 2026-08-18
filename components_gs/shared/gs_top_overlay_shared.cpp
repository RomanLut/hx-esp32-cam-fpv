#include "gs_top_overlay_shared.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "gs_link_quality_sampler.h"
#include "gs_runtime_state.h"
#include "gs_shared_state.h"
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
constexpr float kTopOverlayHorizontalMargin = 3.0f;
constexpr float kTopOverlayVerticalMargin = 3.0f;
constexpr float kLinkGaugeWidth = 100.0f;
constexpr float kLinkGaugeHeight = 16.0f;
constexpr float kLinkGaugeBorderWidth = 3.0f;
constexpr float kLinkGaugeInnerGap = 1.0f;
constexpr float kLinkGaugeScreenMargin = 3.0f;

//===================================================================================
//===================================================================================
// Describes one top overlay chip before it is drawn.
struct OverlayChipSpec
{
    std::string text;
    bool alert = false;
    float width = 0.0f;
    bool warning = false;
};

//===================================================================================
//===================================================================================
// Draws inset top overlay chips, wrapping to the next row when the next chip would overflow.
float drawOverlayChipStrip(const std::vector<OverlayChipSpec>& chips,
                           float start_y,
                           float overlay_width,
                           float horizontal_margin)
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
    const float left_x = std::max(0.0f, horizontal_margin);
    const float available_width = std::max(0.0f, overlay_width - (left_x * 2.0f));
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    float x = left_x;
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
        if (x > left_x && available_width > 0.0f && (x - left_x) + chip_width > available_width)
        {
            x = left_x;
            y += resolved_height + row_gap;
        }

        const ImVec4 bg = chip.warning ? ImVec4(0.95f, 0.73f, 0.05f, 0.92f)
                                      : chip.alert ? ImVec4(0.54f, 0.29f, 0.29f, 0.80f)
                                                   : ImVec4(0.42f, 0.42f, 0.42f, 0.80f);

        // The fullscreen overlay window is shared with OSD/menu drawing, so draw
        // chips with absolute coordinates instead of relying on ImGui item cursor state.
        const ImVec2 chip_min(window_pos.x + x, window_pos.y + y);
        const ImVec2 chip_max(chip_min.x + chip_width, chip_min.y + resolved_height);
        const ImVec2 text_pos(chip_min.x + std::max(0.0f, (chip_width - text_size.x) * 0.5f),
                              chip_min.y + std::max(0.0f, (resolved_height - text_size.y) * 0.5f));
        draw_list->AddRectFilled(chip_min, chip_max, ImGui::ColorConvertFloat4ToU32(bg));
        const ImU32 text_color = chip.warning ? IM_COL32(20, 20, 20, 255)
                                              : ImGui::GetColorU32(ImGuiCol_Text);
        draw_list->AddText(text_pos, text_color, chip.text.c_str());

        x += chip_width + row_gap;
        drew_chip = true;
    }

    return drew_chip ? (y - start_y) + resolved_height : 0.0f;
}

//===================================================================================
//===================================================================================
// Draws one transparent link-quality gauge with a white border and squared lime fill response.
void drawLinkQualityGauge(float x, float y, float quality, bool fill_from_right)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    // The host window may carry a non-zero WindowPadding, which shrinks its clip
    // rect by half the padding on each side and eats the outer border pixels of a
    // gauge sitting at the screen margin. Gauges are positioned in absolute screen
    // coordinates, so draw them against the full viewport instead.
    draw_list->PushClipRectFullScreen();
    const ImU32 border_color = IM_COL32(255, 255, 255, 255);
    // Four filled bars produce exact 3 px edges. A stroked ImGui rectangle is
    // centered on its path and anti-aliasing can make its clipped right edge thinner.
    draw_list->AddRectFilled(ImVec2(x, y),
                             ImVec2(x + kLinkGaugeWidth, y + kLinkGaugeBorderWidth),
                             border_color);
    draw_list->AddRectFilled(ImVec2(x, y + kLinkGaugeHeight - kLinkGaugeBorderWidth),
                             ImVec2(x + kLinkGaugeWidth, y + kLinkGaugeHeight),
                             border_color);
    draw_list->AddRectFilled(ImVec2(x, y + kLinkGaugeBorderWidth),
                             ImVec2(x + kLinkGaugeBorderWidth,
                                    y + kLinkGaugeHeight - kLinkGaugeBorderWidth),
                             border_color);
    draw_list->AddRectFilled(ImVec2(x + kLinkGaugeWidth - kLinkGaugeBorderWidth,
                                    y + kLinkGaugeBorderWidth),
                             ImVec2(x + kLinkGaugeWidth,
                                    y + kLinkGaugeHeight - kLinkGaugeBorderWidth),
                             border_color);

    const float inset = kLinkGaugeBorderWidth + kLinkGaugeInnerGap;
    const float inner_left = x + inset;
    const float inner_right = x + kLinkGaugeWidth - inset;
    const float inner_top = y + inset;
    const float inner_bottom = y + kLinkGaugeHeight - inset;
    const float clamped_quality = std::clamp(quality, 0.0f, 1.0f);
    const float displayed_quality = clamped_quality * clamped_quality;
    const float fill_width = std::max(0.0f, inner_right - inner_left) * displayed_quality;
    if (fill_width <= 0.0f || inner_bottom <= inner_top)
    {
        draw_list->PopClipRect();
        return;
    }

    const float fill_left = fill_from_right ? inner_right - fill_width : inner_left;
    const float fill_right = fill_from_right ? inner_right : inner_left + fill_width;
    draw_list->AddRectFilled(ImVec2(fill_left, inner_top),
                             ImVec2(fill_right, inner_bottom),
                             IM_COL32(0, 255, 0, 255));
    draw_list->PopClipRect();
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

    if (input.spectator) chips.push_back({"SPECTATOR", false, 135.0f, true});

    if (input.air_stats_valid)
    {
        std::snprintf(buf, sizeof(buf), "AIR:%d", -input.air_rssi_dbm);
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "AIR:?");
    }
    air_link_text = buf;

    if (input.has_gs_stats)
    {
        if (!input.air_stats_valid)
        {
            gs_link_text = input.is_dual ? "GS:?/?" : "GS:?";
        }
        else if (!input.is_dual)
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
    if (input.air_stats_valid && input.air_temperature >= 110)
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

    if (input.air_stats_valid)
    {
        std::snprintf(buf, sizeof(buf), "%d%%", input.wifi_queue_percent);
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "?");
    }
    chips.push_back({buf, input.wifi_queue_alert, 55.0f});

    if (!throughput_text.empty()) chips.push_back({throughput_text, false, 90.0f});
    const std::string resolution_text = gs::menu::getResolutionSummary(
        input.config,
        input.is_ov5640,
        input.is_ov3660,
        input.is_esp32);
    if (!resolution_text.empty()) chips.push_back({resolution_text, false, 0.0f});

    std::snprintf(buf, sizeof(buf), "%02d", input.video_fps);
    chips.push_back({buf, input.video_fps_alert, 45.0f});
    if (input.rc_period_warning) chips.push_back({"RC!", true, 0.0f});
    if (input.image_stabilization_enabled) chips.push_back({"STAB", false, 65.0f});

    if (input.battery_percent >= 0)
    {
        std::snprintf(buf, sizeof(buf), "BAT:%d%%", input.battery_percent);
        chips.push_back({buf, input.battery_percent < 30, 0.0f});
    }

    // A spectator does not own the camera's control link, so a missing pong is
    // expected and must not be presented as a link failure.
    if (input.no_ping && !input.spectator) chips.push_back({"NO PING!", true, 0.0f});
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

    const float additional_margin = static_cast<float>(std::clamp<int>(s_groundstation_config.osdMargin, 0, 32));
    const float horizontal_margin = kTopOverlayHorizontalMargin + additional_margin;
    const float top_margin = kTopOverlayVerticalMargin + additional_margin;
    const float main_row_height = drawOverlayChipStrip(chips, top_margin, overlay_width, horizontal_margin);
    if (!input.transport_message.empty())
    {
        drawOverlayChipStrip({{input.transport_message, true, 0.0f}},
                             top_margin + main_row_height + kOverlayBannerGap,
                             overlay_width,
                             horizontal_margin);
    }
}

//===================================================================================
//===================================================================================
// Draws the optional video and RC link-quality gauges in the bottom corners.
void drawLinkQualityGauges(const TopOverlayData& input, float overlay_width, float overlay_height)
{
    if (GImGui == nullptr || GImGui->CurrentWindow == nullptr || GImGui->Font == nullptr)
    {
        return;
    }

    static LinkQualitySampler sampler;
    sampler.update(input);

    const float additional_margin = static_cast<float>(std::clamp<int>(s_groundstation_config.osdMargin, 0, 32));
    const float screen_margin = kLinkGaugeScreenMargin + additional_margin;
    const float y = std::max(screen_margin,
                             overlay_height - kLinkGaugeHeight - screen_margin);
    if (s_groundstation_config.osdVideoLqGauge)
    {
        drawLinkQualityGauge(screen_margin, y, sampler.videoQuality(), false);
    }
    if (s_groundstation_config.osdRcLqGauge)
    {
        const float x = std::max(screen_margin,
                                 overlay_width - kLinkGaugeWidth - screen_margin);
        drawLinkQualityGauge(x, y, sampler.rcQuality(), true);
    }
}
}
