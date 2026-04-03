#include "gs_linux_render_helpers.h"

#include <cstring>
#include <unistd.h>

#include "imgui.h"
#include "linux_osd.h"
#include "gs_linux_recording.h"
#include "gs_linux_runtime.h"
#include "gs_osd_font_shared.h"
#include "gs_runtime_osd_font_storage.h"
#include "gs_runtime_core.h"
#include "core/osd_menu_common.h"
#include "osd_menu.h"

Ground2Air_Config_Packet beginRenderConfigFrame()
{
    return s_runtimeCore.session.copyConfigPacket();
}

bool finishRenderConfigFrame(Ground2Air_Config_Packet& config)
{
    return s_runtimeCore.session.syncConfigPacket(config);
}

void handleRenderHotkeys(Ground2Air_Config_Packet& config, bool ignore_keys)
{
    if (ImGui::IsKeyPressed(ImGuiKey_D) || ImGui::IsKeyPressed(ImGuiKey_MouseMiddle))
    {
        s_debugWindowVisisble = !s_debugWindowVisisble;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_S))
    {
        s_groundstation_config.stats = !s_groundstation_config.stats;
    }

    bool reset_resolution = false;
    if (!ignore_keys && ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
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

    if (!ignore_keys && ImGui::IsKeyPressed(ImGuiKey_RightArrow))
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

    if (!ignore_keys && ImGui::IsKeyPressed(ImGuiKey_R, false))
    {
        config.misc.air_record_btn++;
    }

    if (!ignore_keys && ImGui::IsKeyPressed(ImGuiKey_G, false))
    {
        toggleGSRecording(0, 0, "keyboard_g");
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Space) || (!ignore_keys && ImGui::IsKeyPressed(ImGuiKey_Escape)))
    {
        exitApp();
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
            s_transport.setChannel(s_groundstation_config.wifi_channel);
        }
    }
}

void processPendingOsdFontReload(const Ground2Air_Config_Packet& config)
{
    if (s_reload_osd_font == true)
    {
        s_reload_osd_font = false;
        const std::vector<std::string>& font_names = s_OSDFontStorage->osdFontsList();
        const std::optional<std::string> font_name =
            findOsdFontNameByCrc(font_names, config.misc.osdFontCRC32);
        if (!font_name.has_value())
        {
            g_osd.loadFont(getDefaultOsdFontName().c_str());
            return;
        }

        if (g_osd.currentFontName() != *font_name)
        {
            g_osd.loadFont(font_name->c_str());
        }
    }
}
