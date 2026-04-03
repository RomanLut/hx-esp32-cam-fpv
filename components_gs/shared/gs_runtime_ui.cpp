#include "gs_runtime_ui.h"

#include <chrono>
#include <cstdio>

#include "gs_runtime_platform_services.h"
#include "gs_runtime_core.h"
#include "gs_storage_status_shared.h"
#include "gs_runtime_state.h"
#include "core/osd_menu_common.h"
#include "imgui.h"

void drawRuntimeDebugSettingsWindow(Ground2Air_Config_Packet& config, const RuntimeUiContext& context)
{
    char buf[256];
    sprintf(buf, "RSSI:%d FPS:%1.0f/%d %dKB/S %d%%..%d%% AQ:%d %s/%s###HAL",
            ((int)s_last_gs_stats.rssiDbm[0] + (int)s_last_gs_stats.rssiDbm[1]) / 2,
            video_fps,
            s_lost_frame_count,
            s_total_data / 1024,
            s_wifi_queue_min,
            s_wifi_queue_max,
            s_curr_quality,
            gs::menu::getWifiRateLabel(s_curr_wifi_rate),
            gs::menu::getWifiRateLabel(config.dataChannel.wifi_rate));

    static const float slider_width = 480.0f;

    ImGui::SetNextWindowCollapsed(true, ImGuiCond_Once);
    ImGui::Begin(buf);
    {
        {
            int value = config.dataChannel.wifi_power;
            ImGui::SetNextItemWidth(slider_width);
            ImGui::SliderInt("WIFI Power", &value, 0, 20);
            config.dataChannel.wifi_power = value;
        }
        {
            int value = (int)config.dataChannel.wifi_rate;
            ImGui::SetNextItemWidth(slider_width);
            ImGui::SliderInt("WIFI Rate", &value, (int)WIFI_Rate::RATE_B_2M_CCK, (int)WIFI_Rate::RATE_N_72M_MCS7_S);
            if (config.dataChannel.wifi_rate != (WIFI_Rate)value)
            {
                config.dataChannel.wifi_rate = (WIFI_Rate)value;
                commitGround2AirConfig(config);
            }
        }
        {
            const bool pending = context.wifi_channel_apply_pending;
            ImGui::BeginDisabled(pending);
            if (ImGui::Button("APPLY"))
            {
                context.applyWifiChannel(config);
            }
            ImGui::EndDisabled();

            int ch = s_groundstation_config.wifi_channel;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(slider_width - 93);
            ImGui::SliderInt("WIFI Channel", &s_groundstation_config.wifi_channel, 1, 13);
            if (ch != s_groundstation_config.wifi_channel)
            {
                s_settingsStorage.saveGroundStationConfig();
            }
        }
        {
            int value = config.dataChannel.fec_codec_n;
            ImGui::SetNextItemWidth(slider_width);
            ImGui::SliderInt("FEC_N", &value, FEC_K + 1, FEC_N);
            if (config.dataChannel.fec_codec_n != (int8_t)value)
            {
                config.dataChannel.fec_codec_n = (int8_t)value;
                commitGround2AirConfig(config);
            }
        }

        if (s_isOV5640)
        {
            if (ImGui::Checkbox("ov5640HiFPS", &config.camera.ov5640HighFPS))
            {
                commitGround2AirConfig(config);
            }
        }
        else
        {
            if (ImGui::Checkbox("ov2640HiFPS", &config.camera.ov2640HighFPS))
            {
                commitGround2AirConfig(config);
            }
        }
        {
            ImGui::SameLine();
            int value = (int)config.camera.resolution;
            ImGui::SetNextItemWidth(slider_width - 198);
            ImGui::SliderInt("Resolution", &value, 0, 11);
            if (config.camera.resolution != (Resolution)value)
            {
                config.camera.resolution = (Resolution)value;
                commitGround2AirConfig(config);
            }
        }
        {
            int value = (int)config.camera.fps_limit;
            ImGui::SetNextItemWidth(slider_width);
            ImGui::SliderInt("FPS Limit", &value, 0, 100);
            config.camera.fps_limit = (uint8_t)value;
        }
        {
            int value = config.camera.quality;
            ImGui::SetNextItemWidth(slider_width);
            ImGui::SliderInt("Quality(0-auto)", &value, 0, 63);
            config.camera.quality = value;
        }

        ImGui::Checkbox("AGC", &config.camera.agc);
        ImGui::SameLine();
        ImGui::Checkbox("AEC", &config.camera.aec);
        if (config.camera.aec && !s_isOV5640)
        {
            ImGui::SameLine();
            ImGui::Checkbox("AEC DSP", &config.camera.aec2);
        }
        ImGui::SameLine();
        {
            bool prev = config.camera.vflip;
            ImGui::Checkbox("VFLIP", &config.camera.vflip);
            if (prev != config.camera.vflip)
            {
                commitGround2AirConfig(config);
            }
        }
        ImGui::SameLine();
        ImGui::Checkbox("HMIRROR", &config.camera.hmirror);

        if (!config.camera.agc)
        {
            int value = config.camera.agc_gain;
            ImGui::SetNextItemWidth(slider_width);
            ImGui::SliderInt("AGC Gain", &value, 0, 30);
            config.camera.agc_gain = (int8_t)value;
        }
        else
        {
            int value = config.camera.gainceiling;
            ImGui::SetNextItemWidth(slider_width);
            ImGui::SliderInt("GainCeiling", &value, 0, 6);
            config.camera.gainceiling = (uint8_t)value;
        }

        if (config.camera.aec)
        {
            int value = config.camera.ae_level;
            ImGui::SetNextItemWidth(slider_width);
            ImGui::SliderInt("AEC Level", &value, -2, 2);
            if (config.camera.ae_level != (int8_t)value)
            {
                config.camera.ae_level = (int8_t)value;
                commitGround2AirConfig(config);
            }
        }
        else
        {
            int value = config.camera.aec_value;
            ImGui::SetNextItemWidth(slider_width);
            ImGui::SliderInt("AEC Value", &value, 0, 1200);
            config.camera.aec_value = (uint16_t)value;
        }

        {
            int value = config.camera.brightness;
            ImGui::SetNextItemWidth(slider_width);
            ImGui::SliderInt("Brightness", &value, -2, 2);
            if (config.camera.brightness != (int8_t)value)
            {
                config.camera.brightness = (int8_t)value;
                commitGround2AirConfig(config);
            }
        }

        {
            int value = config.camera.contrast;
            ImGui::SetNextItemWidth(slider_width);
            ImGui::SliderInt("Contrast", &value, -2, 2);
            if (config.camera.contrast != (int8_t)value)
            {
                config.camera.contrast = (int8_t)value;
                commitGround2AirConfig(config);
            }
        }

        {
            int value = config.camera.saturation;
            ImGui::SetNextItemWidth(slider_width);
            ImGui::SliderInt("Saturation", &value, -2, 2);
            if (config.camera.saturation != (int8_t)value)
            {
                config.camera.saturation = (int8_t)value;
                commitGround2AirConfig(config);
            }
        }

        {
            int value = config.camera.sharpness;
            ImGui::SetNextItemWidth(slider_width);
            ImGui::SliderInt("Sharpness(3-auto)", &value, -2, 3);
            if (config.camera.sharpness != (int8_t)value)
            {
                config.camera.sharpness = (int8_t)value;
                commitGround2AirConfig(config);
            }
        }

        {
            int ch = (int)s_groundstation_config.screenAspectRatio;
            ImGui::SetNextItemWidth(slider_width);
            ImGui::SliderInt("Letterbox", &ch, 0, 5);
            if (ch != (int)s_groundstation_config.screenAspectRatio)
            {
                s_groundstation_config.screenAspectRatio = (ScreenAspectRatio)ch;
                s_settingsStorage.saveGroundStationConfig();
            }
        }

        if (context.restart_required)
        {
            ImGui::Text("*Restart to apply!");
        }

        if (ImGui::Button("Profile 500ms"))
        {
            config.misc.profile1_btn++;
        }
        ImGui::SameLine();
        if (ImGui::Button("Profile 3s"))
        {
            config.misc.profile2_btn++;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("VSync", &s_groundstation_config.vsync))
        {
            s_RuntimePlatformServices->setVsync(s_groundstation_config.vsync);
            s_settingsStorage.saveGroundStationConfig();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Stats", &s_groundstation_config.stats);

        if (ImGui::Button("Air Record"))
        {
            config.misc.air_record_btn++;
        }

        ImGui::SameLine();
        if (ImGui::Button("GS Record"))
        {
            if (context.toggleGsRecording)
            {
                context.toggleGsRecording();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Restart"))
        {
            if (context.requestRestart)
            {
                context.requestRestart();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Exit"))
        {
            s_RuntimePlatformServices->exitApp();
        }

        ImGui::Text("%.3f ms/frame (%.1f FPS) %.1f VFPS", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate, video_fps);
        const AirStorageStatusView air_storage = {
            s_SDDetected,
            s_SDError,
            s_SDSlow,
            s_SDFreeSpaceGB16,
            s_SDTotalSpaceGB16,
        };
        const GroundStorageStatus ground_storage = s_RuntimePlatformServices->getGroundStorageStatus();
        ImGui::Text("%s",
                    formatAirStorageStatusLine(
                        air_storage,
                        "Detected",
                        "Not detected",
                        s_isOV5640 ? "OV5640" : "OV2640")
                        .c_str());
        ImGui::Text("%s", formatGroundStorageStatusLine(ground_storage).c_str());
    }
    ImGui::End();
}

RuntimeFrameUiState buildLinuxRuntimeFrameUiState(const Ground2Air_Config_Packet& config, const RuntimeUiContext& context)
{
    RuntimeFrameUiState state = {};
    RuntimeOverlayBuildInput input = {};
    input.config = &config;
    input.air_stats = &s_last_airStats;
    input.gs_stats = &s_last_gs_stats;
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
    input.gs_record = s_groundstation_config.record;
    input.hq_dvr = isHQDVRMode();
    input.gs_temp_celsius = s_RuntimePlatformServices->getCpuTemperatureCelsius();
    input.osd_font_error = context.osd_font_error;
    input.incompatible_firmware_time = s_incompatibleFirmwareTime;
    input.now = Clock::now();
    state.overlay = buildRuntimeOverlayState(input);
    state.overlay.stats_visible = s_groundstation_config.stats;
    if (state.overlay.stats_visible)
    {
        GSStats last_gs_stats;
        {
            std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
            last_gs_stats = s_last_gs_stats;
        }
        const gs::core::FrameStatsState frame_stats = s_runtimeCore.session.copyFrameStats();
        gs::stats::FullscreenStatsBuildInput stats_input = {};
        stats_input.fec_codec_n = config.dataChannel.fec_codec_n;
        stats_input.current_quality = s_curr_quality;
        stats_input.wifi_queue_max = s_wifi_queue_max;
        stats_input.cpu_temp_c = static_cast<int>(s_RuntimePlatformServices->getCpuTemperatureCelsius() + 0.5f);
        stats_input.air_stats = s_last_airStats;
        stats_input.ground_stats = gs::stats::buildGroundStatsSnapshot(last_gs_stats);
        stats_input.frame_stats = frame_stats.frame_stats;
        stats_input.frame_parts_stats = frame_stats.frame_parts_stats;
        stats_input.frame_time_stats = frame_stats.frame_time_stats;
        stats_input.frame_quality_stats = frame_stats.frame_quality_stats;
        stats_input.data_size_stats = s_dataSize_stats;
        stats_input.queue_usage_stats = frame_stats.queue_usage_stats;
        state.overlay.stats_snapshot = gs::stats::buildFullscreenStatsSnapshot(stats_input);
    }
    state.menu_footer.clear();
    state.screen_mode = static_cast<int>(s_groundstation_config.screenAspectRatio);
    state.vsync = s_groundstation_config.vsync;
    state.vr_mode = s_groundstation_config.vrMode;
    return state;
}

void drawRuntimeFrameUi(const RuntimeFrameUiState& state, const RuntimeUiContext& context)
{
    if (context.drawFlightOsd)
    {
        context.drawFlightOsd();
    }
    drawRuntimeFrameUiContent(state);
}
