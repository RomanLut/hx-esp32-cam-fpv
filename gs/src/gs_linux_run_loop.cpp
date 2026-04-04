#include "gs_linux_run_loop.h"

#include <chrono>
#include <string>

#include "flight_osd.h"
#include "gs_recordings_storage.h"
#include "gs_linux_render_helpers.h"
#include "gs_runtime_core.h"
#include "gs_runtime_config.h"
#include "gs_runtime_core.h"
#include "gs_runtime_frame_ui.h"
#include "gs_runtime_menu_ui.h"
#include "gs_runtime_platform_services.h"
#include "gs_top_overlay_shared.h"
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
        RuntimeFrameUiState frame_ui = {};
        frame_ui.overlay_stats_visible = s_groundstation_config.stats;
        gs::stats::FullscreenStatsSnapshot overlay_stats_snapshot = {};
        if (frame_ui.overlay_stats_visible)
        {
            GSStats last_gs_stats = {};
            {
                std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
                last_gs_stats = s_last_gs_stats;
            }
            const gs::core::FrameStatsState frame_stats = s_runtimeCore.session.copyFrameStats();
            overlay_stats_snapshot.fec_codec_n = config.dataChannel.fec_codec_n;
            overlay_stats_snapshot.current_quality = s_curr_quality;
            overlay_stats_snapshot.wifi_queue_max = s_wifi_queue_max;
            overlay_stats_snapshot.cpu_temp_c = static_cast<int>(s_RuntimePlatformServices->getCpuTemperatureCelsius() + 0.5f);
            overlay_stats_snapshot.air_stats = s_last_airStats;
            overlay_stats_snapshot.ground_stats = gs::stats::buildGroundStatsSnapshot(last_gs_stats);
            overlay_stats_snapshot.frame_stats = frame_stats.frame_stats;
            overlay_stats_snapshot.frame_parts_stats = frame_stats.frame_parts_stats;
            overlay_stats_snapshot.frame_time_stats = frame_stats.frame_time_stats;
            overlay_stats_snapshot.frame_quality_stats = frame_stats.frame_quality_stats;
            overlay_stats_snapshot.data_size_stats = s_dataSize_stats;
            overlay_stats_snapshot.queue_usage_stats = frame_stats.queue_usage_stats;
        }
        frame_ui.menu_footer.clear();
        frame_ui.screen_mode = s_groundstation_config.screenAspectRatio;
        frame_ui.vsync = s_groundstation_config.vsync;
        frame_ui.vr_mode = s_groundstation_config.vrMode;
        frame_ui.flight_osd_is_16x9 = s_decoder.isAspect16x9();

        bool ignore_keys = g_osdMenu.visible;

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("fullscreen", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        {
            const ImVec2 display_size = ImGui::GetIO().DisplaySize;
            s_flightOSD.draw(static_cast<int>(display_size.x),
                             static_cast<int>(display_size.y),
                             frame_ui.flight_osd_is_16x9 ? 16 : 4,
                             frame_ui.flight_osd_is_16x9 ? 9 : 3,
                             static_cast<int>(frame_ui.screen_mode),
                             frame_ui.vr_mode);

            //------------------------------------------------
            gs::imgui::TopOverlayData input = {};

            input.config = config;
            input.air_rssi_dbm = s_last_airStats.rssiDbm;
            input.air_temperature = s_last_airStats.temperature;
            input.air_overheat = s_last_airStats.overheatTrottling != 0;
            input.air_suspended = s_last_airStats.suspended == 1;
            input.has_gs_stats = true;
            input.gs_rssi_dbm0 = s_last_gs_stats.rssiDbm[0];
            input.gs_rssi_dbm1 = s_last_gs_stats.rssiDbm[1];
            input.is_ov5640 = s_isOV5640;
            input.is_dual = s_isDual;
            input.wifi_queue_percent = s_wifi_queue_max;
            input.wifi_queue_alert = s_wifi_ovf;
            input.throughput_total_bytes = s_total_data;
            input.use_megabit_total = true;
            input.video_fps = static_cast<int>(video_fps);
            input.video_fps_alert = had_loss;
            input.no_ping = s_noPing;
            input.sd_slow = s_SDSlow;
            input.air_record = s_air_record;
            input.gs_record = s_recordingsStorage->isRecording();
            input.hq_dvr = isHQDVRMode();
            input.gs_temp_celsius = s_RuntimePlatformServices->getCpuTemperatureCelsius();
            input.osd_font_error = s_flightOSD.isFontError();
            input.incompatible_firmware_time = s_incompatibleFirmwareTime;
            input.now = Clock::now();

            gs::imgui::drawTopOverlayStatus(input);
            //------------------------------------------------
            
            if (frame_ui.overlay_stats_visible)
            {
                gs::stats::drawFullscreenStatsPanel(overlay_stats_snapshot);
            }
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

        handleRenderHotkeys(config, ignore_keys);
        processPendingRestart(argv);
        processPendingWifiChannelChange();

        if (s_runtimeCore.session.syncConfigPacket(config))
        {
            pendingOsdFontReload();
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
