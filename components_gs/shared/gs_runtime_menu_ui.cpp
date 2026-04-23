#include "gs_runtime_menu_ui.h"

#include "core/osd_menu_imgui_shared.h"
#include "gs_playback_manager.h"
#include "gs_video_layout_shared.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{

ImVec4 toImGuiColor(float r, float g, float b, float a)
{
    return ImVec4(r, g, b, a);
}

struct TouchKeyRepeatState
{
    double next_repeat_time = 0.0;
};

void emitMenuKey(ImGuiKey key)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(key, true);
    io.AddKeyEvent(key, false);
}

void handleTouchKeyRepeat(ImGuiKey key)
{
    static std::unordered_map<ImGuiID, TouchKeyRepeatState> repeat_states;

    const ImGuiID item_id = ImGui::GetItemID();
    TouchKeyRepeatState& repeat_state = repeat_states[item_id];
    const ImGuiIO& io = ImGui::GetIO();
    const double now = ImGui::GetTime();

    if (ImGui::IsItemActivated())
    {
        emitMenuKey(key);
        repeat_state.next_repeat_time = now + io.KeyRepeatDelay;
        return;
    }

    if (ImGui::IsItemActive())
    {
        if (now >= repeat_state.next_repeat_time)
        {
            emitMenuKey(key);
            repeat_state.next_repeat_time = now + io.KeyRepeatRate;
        }
        return;
    }

    repeat_state.next_repeat_time = 0.0;
}

} // namespace

//===================================================================================
//===================================================================================
// Draws the active recording playback progress bar and timing/error label.
void drawPlaybackProgressOverlay(float overlay_width, float surface_height)
{
    if (s_playbackManager == nullptr)
    {
        return;
    }

    const PlaybackStatus status = s_playbackManager->status();
    if (!status.active || overlay_width <= 0.0f || surface_height <= 0.0f)
    {
        return;
    }

    const float scale = gs::menu::imgui::calcOsdScale(surface_height);
    const float bar_width = std::min(overlay_width * 0.76f, 920.0f * scale);
    const float bar_height = std::max(4.0f, 5.0f * scale);
    const float x = std::floor((overlay_width - bar_width) * 0.5f);
    const float y = std::floor(surface_height - 70.0f * scale);
    const float progress = status.total_frames > 0
        ? std::clamp(static_cast<float>(status.current_frame) / static_cast<float>(status.total_frames), 0.0f, 1.0f)
        : 0.0f;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImU32 dark_blue = IM_COL32(37, 51, 88, 255);
    const ImU32 blue = IM_COL32(77, 137, 205, 255);
    const ImU32 text_color = IM_COL32(255, 255, 255, 255);

    draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + bar_width, y + bar_height), dark_blue);
    draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + bar_width * progress, y + bar_height), blue);

    const char* label = status.broken ? "Broken Video Sequence" : status.message.c_str();
    draw_list->AddText(ImVec2(x, y + bar_height + 8.0f * scale), text_color, label);
}

//===================================================================================
//===================================================================================
// Draws the dimmed fullscreen menu background behind the runtime menu content.
void drawRuntimeMenuOverlay(const RuntimeMenuUiState& state)
{
    if (!state.visible)
    {
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
}

//===================================================================================
//===================================================================================
// Draws the runtime touch navigation pad above the menu when touch input is enabled.
void drawRuntimeMenuTouchNav(const RuntimeMenuUiState& state)
{
    if (!state.visible || !state.touch_nav_enabled)
    {
        return;
    }

    const float surface_width = state.surface_width > 0.0f ? state.surface_width : ImGui::GetIO().DisplaySize.x;
    const float surface_height = state.surface_height > 0.0f ? state.surface_height : ImGui::GetIO().DisplaySize.y;
    const float layout_width = state.vr_mode ? (surface_width * 0.5f) : surface_width;
    const gs::render::NavPadLayout nav = gs::render::buildTouchNavPadLayout(static_cast<int>(layout_width),
                                                                            static_cast<int>(surface_height));
    const ImVec2 nav_size(nav.size, nav.size);
    const ImVec4 active_bg = toImGuiColor(0.16f, 0.20f, 0.26f, 0.92f);
    const ImVec4 back_bg = toImGuiColor(0.22f, 0.18f, 0.18f, 0.92f);
    const ImVec4 right_bg = toImGuiColor(0.18f, 0.27f, 0.18f, 0.92f);

    auto draw_nav_pad_window = [&](const char* window_name, float origin_x, bool interactive)
    {
        ImGui::SetNextWindowPos(ImVec2(origin_x, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(layout_width, surface_height), ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::Begin(window_name,
                     nullptr,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBackground);

        auto draw_button = [&](const char* label, float x, float y, const ImVec4& bg, ImGuiKey key)
        {
            ImGui::SetCursorPos(ImVec2(x, y));
            ImGui::PushStyleColor(ImGuiCol_Button, bg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
            ImGui::Button(label, nav_size);
            if (interactive)
            {
                // Touch nav buttons synthesize short key pulses. They are not
                // held as real ImGui keys, so repeat must be generated while
                // the drawn button remains active.
                handleTouchKeyRepeat(key);
            }
            ImGui::PopStyleColor(3);
        };

        draw_button("UP", nav.center_x, nav.up_y, active_bg, ImGuiKey_UpArrow);
        draw_button("LEFT", nav.left_x, nav.mid_y, back_bg, ImGuiKey_LeftArrow);
        draw_button("RIGHT", nav.right_x, nav.mid_y, right_bg, ImGuiKey_RightArrow);
        draw_button("DOWN", nav.center_x, nav.down_y, active_bg, ImGuiKey_DownArrow);

        // The center cell stays visually empty; renderer tap dispatch treats the same region as Enter.
        ImGui::SetCursorPos(ImVec2(nav.center_x, nav.mid_y));
        ImGui::InvisibleButton("CENTER_ENTER", nav_size);
        if (interactive)
        {
            handleTouchKeyRepeat(ImGuiKey_Enter);
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
    };

    draw_nav_pad_window("RUNTIME_MENU_NAV_PAD", 0.0f, true);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(layout_width, surface_height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::Begin("RUNTIME_MENU_REC_BUTTONS",
                 nullptr,
                 ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground);

    {
        const ImVec4 rec_active_bg = toImGuiColor(0.55f, 0.10f, 0.10f, 0.92f);
        const ImVec4 rec_idle_bg   = toImGuiColor(0.20f, 0.20f, 0.20f, 0.92f);

        auto draw_rec_button = [&](const char* label, float x, float y, bool recording)
        {
            const ImVec4& bg = recording ? rec_active_bg : rec_idle_bg;
            ImGui::SetCursorPos(ImVec2(x, y));
            ImGui::PushStyleColor(ImGuiCol_Button, bg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
            ImGui::Button(label, nav_size);
            ImGui::PopStyleColor(3);
        };

        draw_rec_button("AIR\nREC", nav.margin, nav.mid_y,  state.air_recording);
        draw_rec_button("GS\nREC",  nav.margin, nav.down_y, state.gs_recording);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}
