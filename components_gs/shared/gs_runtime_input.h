#pragma once

#include "packets.h"
#include "gs_playback_manager.h"
#include "gs_recordings_storage.h"
#include "imgui.h"

namespace gs::runtime
{

//===================================================================================
//===================================================================================
// Handles shared ground and air recording keys from the current ImGui frame.
inline bool handleRecordingKeysFromImGui(Ground2Air_Config_Packet& config, const char* gs_record_reason)
{
    if (ImGui::IsKeyPressed(ImGuiKey_R, false))
    {
        config.misc.air_record_btn++;
        return true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_G, false))
    {
        if (s_recordingsStorage != nullptr)
        {
            s_recordingsStorage->toggleRecording(0, 0, gs_record_reason);
        }
        return true;
    }

    return false;
}

//===================================================================================
//===================================================================================
// Handles shared playback controls from the current ImGui frame.
template <typename OpenPlaybackMenu>
bool handlePlaybackKeysFromImGui(PlaybackManager* playback_manager, OpenPlaybackMenu open_playback_menu)
{
    if (playback_manager == nullptr || !playback_manager->status().active)
    {
        return false;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))
    {
        playback_manager->decreaseSpeed();
        return true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
    {
        playback_manager->increaseSpeed();
        return true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
    {
        playback_manager->togglePaused();
        return true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
        ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
    {
        playback_manager->stopPlayback();
        open_playback_menu();
        return true;
    }

    return false;
}

} // namespace gs::runtime
