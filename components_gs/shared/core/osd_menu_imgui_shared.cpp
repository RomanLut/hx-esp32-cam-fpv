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
    ImGui::Button(caption, ImVec2(layout.status_width, layout.button_height));
    popButtonStyle();
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
// Draws an 8px-wide vertical scrollbar to the right of the clipped menu item list.
// Track uses the navy menu background color; thumb uses the blue active-item color.
void drawScrollbar(float x, float y_start, float track_height,
                   int selected_item, int total_items, int visible_items, float width)
{
    if (total_items <= visible_items || track_height <= 0.0f)
        return;

    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    const float y_end = y_start + track_height;

    draw_list->AddRectFilled(ImVec2(x, y_start), ImVec2(x + width, y_end),
                             IM_COL32(37, 51, 88, 255));

    const float thumb_height = track_height * (float)visible_items / (float)total_items;
    const float scrollable = (float)(total_items - visible_items);
    const float t = (float)std::clamp(selected_item, 0, (int)scrollable) / scrollable;
    const float thumb_y = y_start + t * (track_height - thumb_height);

    draw_list->AddRectFilled(ImVec2(x, thumb_y), ImVec2(x + width, thumb_y + thumb_height),
                             IM_COL32(77, 137, 205, 255));
}

} // namespace gs::menu::imgui
