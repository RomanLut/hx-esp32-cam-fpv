#include "gs_linux_render_helpers.h"

#include <cstring>
#include <unistd.h>

#include "imgui.h"
#include "flight_osd.h"
#include "gs_playback_manager.h"
#include "gs_recordings_storage.h"
#include "gs_runtime_input.h"
#include "gs_linux_runtime.h"
#include "gs_runtime_osd_font_storage.h"
#include "gs_runtime_platform_services.h"
#include "gs_runtime_core.h"
#include "gs_runtime_core.h"
#include "gs_camera_calibration_shared.h"
#include "core/osd_menu_common.h"
#include "core/osd_menu_controller.h"
#include "Log.h"

//===================================================================================
//===================================================================================
// Handles Linux-only render hotkeys and delegates shared hotkeys to common helpers.
void handleRenderHotkeys(Ground2Air_Config_Packet& config, bool ignore_keys)
{
    if (gs::calibration::handleCalibrationKeysFromImGui())
    {
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_S))
    {
        s_groundstation_config.stats = !s_groundstation_config.stats;
    }

    if (!ignore_keys && s_playbackManager != nullptr && s_playbackManager->status().active)
    {
        if (gs::runtime::handlePlaybackKeysFromImGui(s_playbackManager,
                                                     []()
                                                     {
                                                         gs::menu::g_osdMenuController.openPlaybackMenu();
                                                     },
                                                     []()
                                                     {
                                                         gs::menu::g_osdMenuController.openPlaybackDeleteMenuForActivePlayback();
                                                     }))
        {
            return;
        }
    }

    if (!ignore_keys && gs::runtime::handleResolutionCycleKeysFromImGui(config))
    {
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_1, false))
    {
        config.misc.profile1_btn++;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_2, false))
    {
        config.misc.profile2_btn++;
    }

    if (!ignore_keys)
    {
        const OSDMenuId active_menu_id = gs::menu::g_osdMenuController.currentMenuId();
        const bool playback_active = s_playbackManager != nullptr && s_playbackManager->status().active;
        const bool in_playback_menu = gs::menu::g_osdMenuController.isVisible() &&
            (active_menu_id == OSDMenuId::Playback || active_menu_id == OSDMenuId::PlaybackDelete);
        if (!in_playback_menu && !playback_active)
        {
            gs::runtime::handleRecordingKeysFromImGui(config, "keyboard_g");
        }
    }

    if (!ignore_keys && ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        s_RuntimePlatformServices->exitApp();
    }
}

void processPendingRestart(char* argv[])
{
    if (bRestart)
    {
        if (Clock::now() - restart_tp >= std::chrono::milliseconds(100))
        {
            bRestart = false;
            execv(argv[0], argv);
        }
    }
}

void processPendingWifiChannelChange()
{
    if (Clock::now() > s_change_channel)
    {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s_runtimeCore.last_packet_tp).count() < 300)
        {
            s_change_channel = Clock::now() + std::chrono::milliseconds(3000);
        }
        else
        {
            s_change_channel = Clock::now() + std::chrono::hours(10000);
            s_transport->setChannel(s_groundstation_config.wifi_channel);
        }
    }
}
