#include "core/osd_menu_imgui_shared.h"

#include <algorithm>
#include <cmath>

namespace gs::menu::imgui
{

namespace
{

constexpr float kLinuxMenuRefWidth = 1280.0f;
constexpr float kLinuxMenuRefHeight = 720.0f;
constexpr float kLinuxMenuWindowWidth = 500.0f;
constexpr float kLinuxMenuWindowHeight = 600.0f;
constexpr float kLinuxMenuButtonWidth = 442.0f;
constexpr float kLinuxMenuButtonHeight = 35.0f;
constexpr float kLinuxMenuItemIndent = (kLinuxMenuWindowWidth - kLinuxMenuButtonWidth) * 0.5f;
constexpr float kLinuxMenuItemGap = 4.0f;
constexpr float kLinuxMenuGapLarge = 20.0f;
constexpr float kLinuxMenuGapSmall = 8.0f;

float computeScale(float surface_width, float surface_height, bool scale_to_surface)
{
    if (!scale_to_surface || surface_width <= 0.0f || surface_height <= 0.0f)
    {
        return 1.0f;
    }

    return std::min(surface_width / kLinuxMenuRefWidth, surface_height / kLinuxMenuRefHeight);
}

void pushButtonStyle(const ImVec4& color)
{
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.02f, 0.5f));
}

void popButtonStyle()
{
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

} // namespace

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

void beginMenuWindow(const char* window_name, const MenuFrameLayout& layout, ImGuiWindowFlags extra_flags)
{
    ImGui::SetNextWindowPos(ImVec2(layout.window_x, layout.window_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(layout.window_width, layout.window_height), ImGuiCond_Always);
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

void endMenuWindow()
{
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void drawMenuTitle(const char* caption, const MenuFrameLayout& layout)
{
    pushButtonStyle(ImColor(97, 137, 105));
    ImGui::Button(caption, ImVec2(layout.title_width, layout.button_height));
    popButtonStyle();
}

bool drawMenuItem(const char* caption, const MenuFrameLayout& layout, bool selected)
{
    ImGui::Indent();
    pushButtonStyle(selected ? ImColor(77, 137, 205) : ImColor(37, 51, 88));
    const bool clicked = ImGui::Button(caption, ImVec2(layout.item_width, layout.button_height));
    popButtonStyle();
    ImGui::Unindent();
    return clicked;
}

void drawMenuStatus(const char* caption, const MenuFrameLayout& layout)
{
    pushButtonStyle(ImColor(48, 48, 48));
    ImGui::Button(caption, ImVec2(layout.status_width, layout.button_height));
    popButtonStyle();
}

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

void drawLargeGap(const MenuFrameLayout& layout)
{
    if (layout.large_gap > 0.0f)
    {
        ImGui::Dummy(ImVec2(0.0f, layout.large_gap));
    }
}

void drawSmallGap(const MenuFrameLayout& layout)
{
    if (layout.small_gap > 0.0f)
    {
        ImGui::Dummy(ImVec2(0.0f, layout.small_gap));
    }
}

} // namespace gs::menu::imgui
