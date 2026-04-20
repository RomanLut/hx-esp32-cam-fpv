#pragma once

#include "imgui.h"
#include <algorithm>

namespace gs::menu::imgui
{

constexpr float kOsdRefHeight = 720.0f;

//===================================================================================
//===================================================================================
// Returns the OSD UI scale coefficient for the given surface height.
// Values above 1.0 upscale the UI on high-resolution screens;
// values below 1.0 shrink it on small screens.
inline float calcOsdScale(float surface_height)
{
    return std::max(1.0f, surface_height / kOsdRefHeight);
}


//===================================================================================
//===================================================================================
// Scaled layout parameters for an ImGui OSD menu window and its items.
struct MenuFrameLayout
{
    float scale = 1.0f;
    float window_x = 0.0f;
    float window_y = 0.0f;
    float window_width = 500.0f;
    float window_height = 600.0f;
    float title_width = 500.0f;
    float item_width = 442.0f;
    float status_width = 500.0f;
    float button_height = 35.0f;
    float item_indent = 29.0f;
    float item_gap_y = 4.0f;
    float large_gap = 20.0f;
    float small_gap = 8.0f;
};

MenuFrameLayout buildMenuFrameLayout(float surface_width,
                                     float surface_height,
                                     bool scale_to_surface,
                                     float item_indent);
void beginMenuWindow(const char* window_name, const MenuFrameLayout& layout, ImGuiWindowFlags extra_flags = 0);
void endMenuWindow();
void drawMenuTitle(const char* caption, const MenuFrameLayout& layout);
bool drawMenuItem(const char* caption, const MenuFrameLayout& layout, bool selected);
void drawMenuStatus(const char* caption, const MenuFrameLayout& layout);
void drawMenuFooterRight(const char* caption, const MenuFrameLayout& layout);
void drawLargeGap(const MenuFrameLayout& layout);
void drawSmallGap(const MenuFrameLayout& layout);
void drawScrollbar(float x, float y_start, float track_height, int selected_item, int total_items, int visible_items, float width);

} // namespace gs::menu::imgui
