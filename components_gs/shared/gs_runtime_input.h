#pragma once

#include "core/osd_menu_common.h"
#include "packets.h"
#include "gs_camera_calibration_shared.h"
#include "gs_playback_manager.h"
#include "gs_recordings_storage.h"
#include "gs_runtime_config.h"
#include "imgui.h"

namespace gs::runtime
{

//===================================================================================
//===================================================================================
// Handles shared ground and air recording keys from the current ImGui frame.
inline bool handleRecordingKeysFromImGui(Ground2Air_Config_Packet& config, const char* gs_record_reason)
{
    if (gs::calibration::isActive())
    {
        return gs::calibration::handleCalibrationKeysFromImGui();
    }

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
// Handles shared left/right resolution cycling from the current ImGui frame.
inline bool handleResolutionCycleKeysFromImGui(Ground2Air_Config_Packet& config)
{
    bool reset_resolution = false;

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
    {
        bool found = false;
        for (int i = 0; i < gs::menu::getResolutionCycleSize(); i++)
        {
            if (config.camera.resolution == gs::menu::getResolutionCycleValue(i))
            {
                if (i != 0)
                {
                    config.camera.resolution = gs::menu::getResolutionCycleValue(i - 1);
                    commitGround2AirConfig(config);
                }
                found = true;
                break;
            }
        }
        reset_resolution |= !found;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
    {
        bool found = false;
        for (int i = 0; i < gs::menu::getResolutionCycleSize(); i++)
        {
            if (config.camera.resolution == gs::menu::getResolutionCycleValue(i))
            {
                if (i != gs::menu::getResolutionCycleSize() - 1)
                {
                    config.camera.resolution = gs::menu::getResolutionCycleValue(i + 1);
                    commitGround2AirConfig(config);
                }
                found = true;
                break;
            }
        }
        reset_resolution |= !found;
    }

    if (reset_resolution)
    {
        config.camera.resolution = gs::menu::getDefaultCyclingResolution();
        commitGround2AirConfig(config);
    }

    return reset_resolution ||
           ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
           ImGui::IsKeyPressed(ImGuiKey_RightArrow);
}

//===================================================================================
//===================================================================================
// Handles shared playback controls from the current ImGui frame.
template <typename OpenPlaybackMenu, typename OpenPlaybackDeleteMenu>
bool handlePlaybackKeysFromImGui(PlaybackManager* playback_manager,
                                 OpenPlaybackMenu open_playback_menu,
                                 OpenPlaybackDeleteMenu open_playback_delete_menu)
{
    if (playback_manager == nullptr || !playback_manager->status().active)
    {
        return false;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_R, false))
    {
        open_playback_delete_menu();
        return true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_G, false))
    {
        return true;
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

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
    {
        if (playback_manager->status().speed_multiplier != 0)
            playback_manager->togglePaused();
        else
            playback_manager->stepForward();
        return true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
        ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
    {
        playback_manager->stopPlayback();
        open_playback_menu();
        return true;
    }

    return false;
}

} // namespace gs::runtime
