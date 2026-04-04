#include "gs_runtime_menu_ui.h"

#include "gs_video_layout_shared.h"
#include "imgui.h"

namespace
{

ImVec4 toImGuiColor(float r, float g, float b, float a)
{
    return ImVec4(r, g, b, a);
}

void emitMenuKey(ImGuiKey key)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(key, true);
    io.AddKeyEvent(key, false);
}

} // namespace

void drawRuntimeMenuUi(const RuntimeMenuUiState& state, const std::function<void()>& draw_menu)
{
    if (!state.visible)
    {
        if (draw_menu)
        {
            draw_menu();
        }
        return;
    }

    const float surface_width = state.surface_width > 0.0f ? state.surface_width : ImGui::GetIO().DisplaySize.x;
    const float surface_height = state.surface_height > 0.0f ? state.surface_height : ImGui::GetIO().DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(surface_width, surface_height), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBackground |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!state.touch_nav_enabled)
    {
        flags |= ImGuiWindowFlags_NoInputs;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::Begin("RUNTIME_MENU_OVERLAY", nullptr, flags);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(ImVec2(0.0f, 0.0f),
                             ImVec2(surface_width, surface_height),
                             ImGui::ColorConvertFloat4ToU32(toImGuiColor(0.0f, 0.0f, 0.0f, 0.40f)));

    ImGui::End();
    ImGui::PopStyleVar(2);

    if (draw_menu)
    {
        draw_menu();
    }

    if (state.touch_nav_enabled)
    {
        const float layout_width = state.vr_mode ? (surface_width * 0.5f) : surface_width;
        const gs::render::NavPadLayout nav = gs::render::buildTouchNavPadLayout(static_cast<int>(layout_width),
                                                                                static_cast<int>(surface_height));
        const ImVec2 nav_size(nav.size, nav.size);
        const ImVec4 active_bg = toImGuiColor(0.16f, 0.20f, 0.26f, 0.92f);
        const ImVec4 back_bg = toImGuiColor(0.22f, 0.18f, 0.18f, 0.92f);
        const ImVec4 enter_bg = toImGuiColor(0.18f, 0.27f, 0.18f, 0.92f);

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(surface_width, surface_height), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::Begin("RUNTIME_MENU_NAV_PAD",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBackground);

        auto draw_nav_pad = [&](float origin_x, bool interactive)
        {
            auto draw_button = [&](const char* label, float x, float y, const ImVec4& bg, ImGuiKey key)
            {
                ImGui::SetCursorPos(ImVec2(origin_x + x, y));
                ImGui::PushStyleColor(ImGuiCol_Button, bg);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
                if (ImGui::Button(label, nav_size) && interactive)
                {
                    emitMenuKey(key);
                }
                ImGui::PopStyleColor(3);
            };

            draw_button("UP", nav.center_x, nav.up_y, active_bg, ImGuiKey_UpArrow);
            draw_button("LEFT", nav.left_x, nav.mid_y, back_bg, ImGuiKey_LeftArrow);
            draw_button("RIGHT", nav.right_x, nav.mid_y, enter_bg, ImGuiKey_RightArrow);
            draw_button("DOWN", nav.center_x, nav.down_y, active_bg, ImGuiKey_DownArrow);
        };

        draw_nav_pad(0.0f, true);
        if (state.vr_mode)
        {
            draw_nav_pad(layout_width, false);
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}
