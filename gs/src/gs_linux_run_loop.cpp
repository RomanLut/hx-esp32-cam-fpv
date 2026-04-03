#include "gs_linux_run_loop.h"

#include <chrono>
#include <string>

#include "flight_osd.h"
#include "gs_linux_recording.h"
#include "gs_linux_render_helpers.h"
#include "gs_runtime_config.h"
#include "gs_runtime_core.h"
#include "gs_runtime_menu_ui.h"
#include "gs_runtime_ui.h"
#include "imgui.h"
#include "osd_menu.h"
#include "gs_linux_runtime.h"

void initializeLinuxOsd()
{
    const std::string font_name = s_flightOSD.selectedFontName();
    s_flightOSD.loadFont(font_name.c_str());
}

void registerLinuxRenderCallback(Ground2Air_Config_Packet& config, char* argv[])
{
    auto render_callback = [&config, argv]
    {
        RuntimeUiContext runtime_ui = {};
        runtime_ui.wifi_channel_apply_pending = s_change_channel < Clock::now() + std::chrono::hours(1);
        runtime_ui.restart_required = bRestartRequired;
        runtime_ui.osd_font_error = s_flightOSD.isFontError();
        runtime_ui.applyWifiChannel = []
        (Ground2Air_Config_Packet& config)
        {
            applyWifiChannelToSession(config);
            s_change_channel = Clock::now() + std::chrono::milliseconds(3000);
        };
        runtime_ui.drawFlightOsd = []
        {
            const ImVec2 display_size = ImGui::GetIO().DisplaySize;
            const bool is_16x9 = s_decoder.isAspect16x9();
            s_flightOSD.draw(static_cast<int>(display_size.x),
                             static_cast<int>(display_size.y),
                             is_16x9 ? 16 : 4,
                             is_16x9 ? 9 : 3,
                             static_cast<int>(s_groundstation_config.screenAspectRatio),
                             s_groundstation_config.vrMode);
        };
        runtime_ui.toggleGsRecording = []
        {
            toggleGSRecording(0, 0, "debug_ui_button");
        };
        runtime_ui.requestRestart = []
        {
            restart_tp = Clock::now();
            bRestart = true;
        };
        const RuntimeFrameUiState frame_ui = buildLinuxRuntimeFrameUiState(config, runtime_ui);

        bool ignore_keys = g_osdMenu.visible;

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("fullscreen", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        {
            drawRuntimeFrameUi(frame_ui, runtime_ui);
        }
        ImGui::End();
        ImGui::PopStyleVar();

        RuntimeMenuUiState menu_ui = {};
        menu_ui.visible = g_osdMenu.isVisible();
        menu_ui.vr_mode = s_groundstation_config.vrMode;
        menu_ui.touch_nav_enabled = false;
        menu_ui.surface_width = ImGui::GetIO().DisplaySize.x;
        menu_ui.surface_height = ImGui::GetIO().DisplaySize.y;
        drawRuntimeMenuUi(menu_ui, [&config]
        {
            g_osdMenu.draw(config);
        });

        if (s_debugWindowVisisble)
        {
            drawRuntimeDebugSettingsWindow(config, runtime_ui);
        }

        handleRenderHotkeys(config, ignore_keys);
        processPendingRestart(argv);
        processPendingWifiChannelChange();

        if (finishRenderConfigFrame(config))
        {
            s_reload_osd_font = true;
        }

        processPendingOsdFontReload(config);
    };

    s_hal->add_render_callback(render_callback);
}

void processLinuxFrameTick(size_t& video_frame_count,
                             Clock::time_point& last_stats_tp,
                             Clock::time_point& last_tp,
                             ImGuiIO& io)
{
    s_decoder.unlock_output();
    size_t count = s_decoder.lock_output();
    if (count == 0)
    {
        s_decoder.wait_for_output(std::chrono::milliseconds(8));
        count = s_decoder.lock_output();
    }

    video_frame_count += count;
    s_hal->set_video_channel(s_decoder.get_video_texture_id());

    s_hal->process();

    if (Clock::now() - last_stats_tp >= std::chrono::milliseconds(1000))
    {
        last_stats_tp = Clock::now();
        video_fps = video_frame_count;
        s_lost_frame_count = s_runtimeCore.session.consumeLostFrameCount();
        had_loss = s_lost_frame_count != 0;
        video_frame_count = 0;
    }

    Clock::time_point now = Clock::now();
    Clock::duration dt = now - last_tp;
    last_tp = now;
    io.DeltaTime = std::chrono::duration_cast<std::chrono::duration<float>>(dt).count();
}
