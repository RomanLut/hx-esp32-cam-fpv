#include "core/osd_menu_imgui_shared.h"

#include <algorithm>
#include <cmath>

namespace gs::menu::imgui
{

namespace
{

constexpr float kLinuxMenuWindowWidth = 500.0f;
constexpr float kLinuxMenuWindowHeight = 600.0f;
constexpr float kLinuxMenuButtonWidth = 442.0f;
constexpr float kLinuxMenuButtonHeight = 35.0f;
constexpr float kLinuxMenuItemIndent = (kLinuxMenuWindowWidth - kLinuxMenuButtonWidth) * 0.5f;
constexpr float kLinuxMenuItemGap = 4.0f;
constexpr float kLinuxMenuGapLarge = 20.0f;
constexpr float kLinuxMenuGapSmall = 8.0f;

//===================================================================================
//===================================================================================
// Computes the UI scale factor to fit the reference menu size onto the surface.
float computeScale(float surface_width, float surface_height, bool scale_to_surface)
{
    if (!scale_to_surface || surface_height <= 0.0f)
    {
        return 1.0f;
    }

    return calcOsdScale(surface_height);
}

//===================================================================================
//===================================================================================
// Pushes a solid-color button style (normal, hovered, and active states all the same)
// with left-aligned text onto the ImGui style stack.
void pushButtonStyle(const ImVec4& color)
{
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.02f, 0.5f));
}

//===================================================================================
//===================================================================================
// Pops the button style pushed by pushButtonStyle from the ImGui style stack.
void popButtonStyle()
{
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

//===================================================================================
//===================================================================================
// Returns the status/graph width that fits between both menu-window padding edges.
float getVisibleStatusWidth(const MenuFrameLayout& layout)
{
    return std::max(0.0f, std::min(layout.status_width, ImGui::GetContentRegionAvail().x));
}

} // namespace

//===================================================================================
//===================================================================================
// Computes a fully scaled MenuFrameLayout for the given surface dimensions,
// positioning the window centered horizontally and slightly below vertical center.
MenuFrameLayout buildMenuFrameLayout(float surface_width,
                                     float surface_height,
                                     bool scale_to_surface,
                                     float item_indent)
{
    MenuFrameLayout layout;
    layout.scale = computeScale(surface_width, surface_height, scale_to_surface);
    layout.window_width = kLinuxMenuWindowWidth * layout.scale;
    layout.window_height = kLinuxMenuWindowHeight * layout.scale;
    layout.title_width = kLinuxMenuButtonWidth * layout.scale;
    layout.item_width = kLinuxMenuButtonWidth * layout.scale;
    layout.status_width = kLinuxMenuWindowWidth * layout.scale;
    layout.button_height = kLinuxMenuButtonHeight * layout.scale;
    layout.item_indent = (item_indent > 0.0f ? item_indent : kLinuxMenuItemIndent) * layout.scale;
    layout.item_gap_y = kLinuxMenuItemGap * layout.scale;
    layout.large_gap = ((surface_height > 480.0f) ? kLinuxMenuGapLarge : 0.0f) * layout.scale;
    layout.small_gap = kLinuxMenuGapSmall * layout.scale;

    const float offset = (surface_height == 576.0f ? 100.0f : 120.0f) * (scale_to_surface ? layout.scale : 1.0f);
    layout.window_x = std::floor((surface_width - layout.window_width) * 0.5f);
    layout.window_y = std::floor((surface_height - layout.window_height) * 0.5f + offset);
    return layout;
}

//===================================================================================
//===================================================================================
// Begins an ImGui menu window positioned and sized according to the layout,
// with standard OSD flags (no resize, no move, no title bar, no background).
void beginMenuWindow(const char* window_name, const MenuFrameLayout& layout, ImGuiWindowFlags extra_flags)
{
    ImGui::SetNextWindowPos(ImVec2(layout.window_x, layout.window_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(layout.window_width, layout.window_height), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, layout.item_indent);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, layout.item_gap_y));
    ImGui::Begin(window_name,
                 nullptr,
                 ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoBackground |
                     extra_flags);
}

//===================================================================================
//===================================================================================
// Ends the ImGui menu window and pops the style vars pushed by beginMenuWindow.
void endMenuWindow()
{
    ImGui::End();
    ImGui::PopStyleVar(2);
}

//===================================================================================
//===================================================================================
// Draws a non-interactive title button at the top of the menu in green.
void drawMenuTitle(const char* caption, const MenuFrameLayout& layout)
{
    pushButtonStyle(ImColor(97, 137, 105));
    ImGui::Button(caption, ImVec2(layout.title_width, layout.button_height));
    popButtonStyle();
}

//===================================================================================
//===================================================================================
// Draws a clickable menu item button, highlighted in blue when selected.
// Returns true if the button was clicked.
bool drawMenuItem(const char* caption, const MenuFrameLayout& layout, bool selected)
{
    ImGui::Indent();
    pushButtonStyle(selected ? ImColor(77, 137, 205) : ImColor(37, 51, 88));
    const bool clicked = ImGui::Button(caption, ImVec2(layout.item_width, layout.button_height));
    popButtonStyle();
    ImGui::Unindent();
    return clicked;
}

//===================================================================================
//===================================================================================
// Draws a non-interactive status bar button in dark grey across the full window width.
void drawMenuStatus(const char* caption, const MenuFrameLayout& layout)
{
    pushButtonStyle(ImColor(48, 48, 48));
    ImGui::Button(caption, ImVec2(getVisibleStatusWidth(layout), layout.button_height));
    popButtonStyle();
}

//===================================================================================
//===================================================================================
// Draws a non-interactive status bar button in red across the full window width.
void drawMenuStatusError(const char* caption, const MenuFrameLayout& layout)
{
    pushButtonStyle(ImColor(176, 44, 44));
    ImGui::Button(caption, ImVec2(getVisibleStatusWidth(layout), layout.button_height));
    popButtonStyle();
}

//===================================================================================
//===================================================================================
// Draws a fixed-position per-channel packet histogram and marks the active bar below it.
void drawMenuPacketHistogram(const std::vector<float>& packet_rates,
                             int current_channel_index,
                             const MenuFrameLayout& layout)
{
    if (packet_rates.empty())
    {
        return;
    }

    const float line_margin = std::max(0.0f, layout.button_height - layout.item_gap_y);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + line_margin);

    const float maximum_rate = *std::max_element(packet_rates.begin(), packet_rates.end());
    // Keep five percent of dynamic headroom so the tallest bar remains visually
    // separated from the top edge of the histogram background.
    const float graph_maximum_rate = std::max(1.0f, maximum_rate * 1.05f);
    const float graph_width = getVisibleStatusWidth(layout);
    ImGui::PlotHistogram("##search_channel_packet_counts",
                         packet_rates.data(),
                         static_cast<int>(packet_rates.size()),
                         0,
                         nullptr,
                         0.0f,
                         graph_maximum_rate,
                         ImVec2(graph_width, 84.0f * layout.scale));

    if (current_channel_index < 0 || current_channel_index >= static_cast<int>(packet_rates.size()))
    {
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 graph_min = ImGui::GetItemRectMin();
    const ImVec2 graph_max = ImGui::GetItemRectMax();
    const float inner_width = graph_max.x - graph_min.x - style.FramePadding.x * 2.0f;
    const float bar_width = inner_width / static_cast<float>(packet_rates.size());
    const float marker_center_x = graph_min.x + style.FramePadding.x +
                                  (static_cast<float>(current_channel_index) + 0.5f) * bar_width;
    const float marker_top_y = graph_max.y + 2.0f * layout.scale;
    const float marker_half_width = 5.0f * layout.scale;
    const float marker_height = 8.0f * layout.scale;

    ImGui::GetWindowDrawList()->AddTriangleFilled(
        ImVec2(marker_center_x, marker_top_y),
        ImVec2(marker_center_x - marker_half_width, marker_top_y + marker_height),
        ImVec2(marker_center_x + marker_half_width, marker_top_y + marker_height),
        IM_COL32_WHITE);
    ImGui::Dummy(ImVec2(0.0f, marker_height));
}

//===================================================================================
//===================================================================================
// Draws a right-aligned text label at the bottom of the menu window.
// Does nothing if caption is null or empty.
void drawMenuFooterRight(const char* caption, const MenuFrameLayout& layout)
{
    if (caption == nullptr || caption[0] == 0)
    {
        return;
    }

    const ImVec2 text_size = ImGui::CalcTextSize(caption);
    const float footer_x = std::max(0.0f, layout.window_width - 8.0f * layout.scale - text_size.x);
    const float footer_y = std::max(0.0f, layout.window_height - layout.button_height + (layout.button_height - text_size.y) * 0.5f);
    ImGui::SetCursorPos(ImVec2(footer_x, footer_y));
    ImGui::TextUnformatted(caption);
}

//===================================================================================
//===================================================================================
// Inserts a large vertical gap between menu sections, if applicable for the surface height.
void drawLargeGap(const MenuFrameLayout& layout)
{
    if (layout.large_gap > 0.0f)
    {
        ImGui::Dummy(ImVec2(0.0f, layout.large_gap));
    }
}

//===================================================================================
//===================================================================================
// Inserts a small vertical gap between closely related menu items.
void drawSmallGap(const MenuFrameLayout& layout)
{
    if (layout.small_gap > 0.0f)
    {
        ImGui::Dummy(ImVec2(0.0f, layout.small_gap));
    }
}

//===================================================================================
//===================================================================================
// Draws a 256-pixel white luma histogram in imaginary menu-item slots 9 through 12.
void drawImageHistogram(const std::array<uint32_t, 256>& bins, const MenuFrameLayout& layout)
{
    constexpr float kHistogramWidth = 256.0f;
    // Seven real items plus the intentionally empty imaginary item 8 place the
    // histogram at item 9 instead of directly after (or over) item 7.
    constexpr int kMenuRowsBeforeHistogram = 8;

    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 window_padding = ImGui::GetStyle().WindowPadding;
    const float row_step = layout.button_height + layout.item_gap_y;
    const float histogram_height = 4.0f * layout.button_height + 3.0f * layout.item_gap_y;
    const float x = window_pos.x + window_padding.x + layout.item_indent +
                    (layout.item_width - kHistogramWidth) * 0.5f;
    const float y = window_pos.y + window_padding.y + layout.button_height + layout.item_gap_y +
                    static_cast<float>(kMenuRowsBeforeHistogram) * row_step;
    const float bottom = y + histogram_height;
    const uint32_t peak = *std::max_element(bins.begin(), bins.end());

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    draw_list->AddRect(ImVec2(x - 1.0f, y - 1.0f),
                       ImVec2(x + kHistogramWidth, bottom),
                       white);

    if (peak == 0)
    {
        return;
    }

    for (std::size_t bin = 0; bin < bins.size(); ++bin)
    {
        const float bar_height = histogram_height *
                                 static_cast<float>(bins[bin]) /
                                 static_cast<float>(peak);
        const float bar_x = x + static_cast<float>(bin);
        draw_list->AddLine(ImVec2(bar_x, bottom), ImVec2(bar_x, bottom - bar_height), white);
    }
}

//===================================================================================
//===================================================================================
// Draws an 8px-wide vertical scrollbar to the right of the clipped menu item list.
// Track uses the navy menu background color; thumb uses the blue active-item color.
void drawScrollbar(float x, float y_start, float track_height,
                   int selected_item, int total_items, int visible_items, float width,
                   bool force_show)
{
    if (track_height <= 0.0f)
        return;
    if (total_items <= visible_items && !force_show)
        return;

    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    const float y_end = y_start + track_height;

    draw_list->AddRectFilled(ImVec2(x, y_start), ImVec2(x + width, y_end),
                             IM_COL32(37, 51, 88, 255));

    float thumb_y = y_start;
    float thumb_height = track_height;
    if (total_items > visible_items)
    {
        thumb_height = track_height * (float)visible_items / (float)total_items;
        const float scrollable = (float)(total_items - visible_items);
        const float t = (float)std::clamp(selected_item, 0, (int)scrollable) / scrollable;
        thumb_y = y_start + t * (track_height - thumb_height);
    }

    draw_list->AddRectFilled(ImVec2(x, thumb_y), ImVec2(x + width, thumb_y + thumb_height),
                             IM_COL32(77, 137, 205, 255));
}

} // namespace gs::menu::imgui
