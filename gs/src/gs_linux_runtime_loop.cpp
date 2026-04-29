#include "gs_linux_runtime_loop.h"

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <string>
#include <thread>
#include <unistd.h>

#include "linux_raw_broadcast_transport.h"
#include "PI_HAL.h"
#include "cpu_temp.h"
#include "flight_osd.h"
#include "shared/frame_packets_debug.h"
#include "gpio_buttons.h"
#include "gs_linux_render_helpers.h"
#include "gs_linux_runtime.h"
#include "gs_linux_runtime_sink.h"
#include "gs_linux_startup.h"
#include "gs_runtime_core.h"
#include "gs_runtime_event_pipeline.h"
#include "gs_recordings_storage.h"
#include "gs_playback_manager.h"
#include "gs_runtime_frame_ui.h"
#include "gs_runtime_menu_ui.h"
#include "gs_camera_calibration_shared.h"
#include "gs_runtime_platform_services.h"
#include "gs_runtime_state.h"
#include "gs_top_overlay_shared.h"
#include "gs_video_layout_shared.h"
#include "core/osd_menu_controller.h"
#include "core/osd_menu_imgui_shared.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "jpeg_parser.h"
#include "main.h"
#include "util.h"
#include "utils/utils.h"
#include "../../components_gs/mcp/gs_mcp_server.h"

namespace
{

std::thread s_comms_thread;

//===================================================================================
//===================================================================================
// Queues one ImGui key press before the next frame consumes input state.
void queueLinuxPointerKey(ImGuiKey key)
{
    if (key == ImGuiKey_None)
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(key, true);
    io.AddKeyEvent(key, false);
}

//===================================================================================
//===================================================================================
// Returns true when a canonical-space tap falls inside the supplied rectangle.
bool linuxPointerInRect(float x, float y, float rect_x, float rect_y, float rect_w, float rect_h)
{
    return rect_w > 0.0f &&
           rect_h > 0.0f &&
           x >= rect_x &&
           x <= rect_x + rect_w &&
           y >= rect_y &&
           y <= rect_y + rect_h;
}

//===================================================================================
//===================================================================================
// Translates Linux pointer taps into the same semantic key actions used by Android.
void handleLinuxPointerTap(float tap_x, float tap_y)
{
    const ImVec2 display_size = ImGui::GetIO().DisplaySize;
    if (display_size.x <= 0.0f || display_size.y <= 0.0f)
    {
        return;
    }

    const bool controls_visible =
        gs::menu::g_osdMenuController.isVisible() ||
        (s_playbackManager != nullptr && s_playbackManager->status().active);
    if (!controls_visible)
    {
        queueLinuxPointerKey(ImGuiKey_Enter);
        return;
    }

    if (s_groundstation_config.screenFlipV)
    {
        tap_x = display_size.x - tap_x;
        tap_y = display_size.y - tap_y;
    }

    if (s_groundstation_config.vrMode)
    {
        const float half_w = display_size.x * 0.5f;
        const float offset = s_groundstation_config.screenVrSeparation * half_w;
        if (tap_x >= half_w)
        {
            tap_x -= half_w;
            tap_x += offset;
        }
        else
        {
            tap_x -= offset;
        }
    }

    const float layout_width = s_groundstation_config.vrMode ? (display_size.x * 0.5f) : display_size.x;
    const gs::render::NavPadLayout nav =
        gs::render::buildTouchNavPadLayout(static_cast<int>(layout_width), static_cast<int>(display_size.y));
    const auto in_nav = [&](float x, float y)
    {
        return linuxPointerInRect(tap_x, tap_y, x, y, nav.size, nav.size);
    };

    if (in_nav(nav.center_x, nav.up_y))
    {
        queueLinuxPointerKey(ImGuiKey_UpArrow);
    }
    else if (in_nav(nav.center_x, nav.down_y))
    {
        queueLinuxPointerKey(ImGuiKey_DownArrow);
    }
    else if (in_nav(nav.left_x, nav.mid_y))
    {
        queueLinuxPointerKey(ImGuiKey_LeftArrow);
    }
    else if (in_nav(nav.right_x, nav.mid_y))
    {
        queueLinuxPointerKey(ImGuiKey_RightArrow);
    }
    else if (in_nav(nav.center_x, nav.mid_y))
    {
        queueLinuxPointerKey(ImGuiKey_Enter);
    }
    else if (in_nav(nav.margin, nav.mid_y))
    {
        queueLinuxPointerKey(ImGuiKey_R);
    }
    else if (in_nav(nav.margin, nav.down_y))
    {
        queueLinuxPointerKey(ImGuiKey_G);
    }
}

//===================================================================================
//===================================================================================
// Background thread that handles all communication: sending control packets,
// receiving and dispatching session packets, processing telemetry and video frames.
void comms_thread_proc()
{
    Clock::time_point last_stats_tp = Clock::now();
    Clock::time_point last_stats_tp10 = Clock::now();
    Clock::time_point last_comms_sent_tp = Clock::now();

    //===================================================================================
    //===================================================================================
    // Holds a received packet buffer, its size, and the RSSI of the received signal.
    struct RX_Data
    {
        std::array<uint8_t, AIR2GROUND_MAX_MTU> data;
        size_t size;
        int16_t rssi = 0;
    };

    RX_Data rx_data;

    while (true)
    {
        if (isTransportReconnectPauseRequested())
        {
            setTransportReconnectPauseObserved(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        setTransportReconnectPauseObserved(false);

        if (Clock::now() - last_stats_tp >= std::chrono::milliseconds(1000))
        {
            const Clock::time_point now = Clock::now();
            const gs::core::LinkStatusSnapshot link_status = s_runtimeCore.session.consumeLinkStatus(now);
            const gs::core::PeriodicStatsSnapshot periodic_stats = s_runtimeCore.session.consumePeriodicStats();
            const gs::core::VideoFrameAssembler::Stats assembler_stats = s_runtimeCore.assembler.consumeStats();
            LOGI("Sent: {}, RX len: {}, TlmIn: {}, TlmOut: {}, RSSI: {}/{}, Latency: {}/{}/{}, vfps: {}, AIR:0x{:04X}, GS:0x{:04X}",
                periodic_stats.sent_count, periodic_stats.total_data, periodic_stats.in_tlm_size, periodic_stats.out_tlm_size,
                (int)s_last_gs_stats.rssiDbm[0], (int)s_last_gs_stats.rssiDbm[1],
                link_status.ping_min_ms,
                link_status.ping_max_ms,
                link_status.ping_avg_ms,
                video_fps,
                s_runtimeCore.session.connectedAirDeviceId(), s_groundstation_config.deviceId);

            s_total_data = periodic_stats.total_data;
            s_noPing = link_status.no_ping;
            {
                std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
                const int8_t rssi0 = s_gs_stats.rssiDbm[0];
                const int8_t rssi1 = s_gs_stats.rssiDbm[1];
                const int8_t noise_floor = s_gs_stats.noiseFloorDbm;
                s_gs_stats.pingMinMS = link_status.ping_min_ms;
                s_gs_stats.pingMaxMS = link_status.ping_max_ms;
                s_gs_stats.receivedCompletedFrames = periodic_stats.received_completed_frames;
                s_gs_stats.restoredCompletedFrames = periodic_stats.restored_completed_frames;
                s_gs_stats.discardedFramesAssemblerPoolOverflow += static_cast<int>(assembler_stats.discarded_frames);

                s_gs_stats.brokenFrames += s_last_gs_stats.brokenFrames;
                s_last_gs_stats = s_gs_stats;
                s_runtimeCore.last_ground_stats = s_last_gs_stats;
                s_gs_stats = GSStats();
                s_gs_stats.statsPacketIndex = s_last_gs_stats.lastPacketIndex;
                s_gs_stats.rssiDbm[0] = rssi0;
                s_gs_stats.rssiDbm[1] = rssi1;
                s_gs_stats.noiseFloorDbm = noise_floor;
            }

            last_stats_tp = now;
        }

        if (Clock::now() - last_stats_tp10 >= std::chrono::milliseconds(100))
        {
            s_dataSize_stats.add(s_runtimeCore.session.consumeDataRateSample());
            last_stats_tp10 = Clock::now();
        }

        if (Clock::now() - last_comms_sent_tp >= std::chrono::milliseconds(500))
        {
            std::vector<uint8_t> control_payload;
            if (tryBuildControlPacketPayload(s_groundstation_config.deviceId, control_payload))
            {
                std::unique_lock<std::mutex> transport_lock(s_transport_mutex, std::try_to_lock);
                if (transport_lock.owns_lock())
                {
                    s_transport->send(control_payload.data(), control_payload.size(), true);
                    s_runtimeCore.session.addSentPackets(1);
                }
            }

            last_comms_sent_tp = Clock::now();
            s_runtimeCore.session.onPingSent(last_comms_sent_tp);
        }

        g_CPUTemp.process();
        processPendingSelectedTransportReconnect();

        {
            std::unique_lock<std::mutex> transport_lock(s_transport_mutex, std::try_to_lock);
            if (transport_lock.owns_lock())
            {
                s_runtimeCore.session.processIncomingTelemetry(
                    s_groundstation_config.deviceId,
                    *s_transport,
                    s_gs_stats_mutex,
                    s_gs_stats);
            }
        }

        while (true)
        {
            bool restoredByFEC = false;
            {
                std::unique_lock<std::mutex> transport_lock(s_transport_mutex, std::try_to_lock);
                if (!transport_lock.owns_lock())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    break;
                }

                s_transport->process();
                rx_data.size = rx_data.data.size();
                if (!s_transport->receive(rx_data.data.data(), rx_data.size, restoredByFEC))
                {
                    std::this_thread::yield();
                    break;
                }
            }

            if (rx_data.size == 0)
            {
                break;
            }

            ProcessedSessionPacket processed_packet = {};
            processed_packet.processed_tp = Clock::now();
            processed_packet.event =
                s_runtimeCore.session.processReceivedPacket(rx_data.data.data(),
                                                           rx_data.size,
                                                           s_groundstation_config.deviceId,
                                                           processed_packet.processed_tp,
                                                           *s_transport);
            s_last_packet_tp = processed_packet.processed_tp;
            rx_data.rssi = (int16_t)s_transport->get_input_dBm();
            if (restoredByFEC)
            {
                s_runtimeCore.restored_transport_packets++;
                std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
                s_gs_stats.restoredTransportPackets++;
            }

            const SessionEventPipelineDispatch dispatch = {
                [&](const gs::core::SessionEvent&)
                {
                    printf("Connecting to Air Device Id 0x%04x\n", s_runtimeCore.session.connectedAirDeviceId());
                },
                [&](const gs::core::SessionEvent& invalid_event)
                {
                    logInvalidLinuxSessionEvent(invalid_event, s_runtimeCore.assembler.currentFrameIndex());
                },
                [](const gs::core::SessionEvent&)
                {
                },
                {
                    [&, restoredByFEC](const ProcessedVideoEvent&, const VideoDispatchDecision& video_decision)
                    {
                        handleLinuxVideoDispatch(video_decision);
                    }
                }
            };
            const RuntimeEventClass event_class =
                processAndDispatchSessionEvent(processed_packet,
                                              s_runtimeCore.assembler,
                                              s_runtimeCore.session,
                                              restoredByFEC,
                                              dispatch);
            if (event_class == RuntimeEventClass::ConnectAccepted ||
                event_class == RuntimeEventClass::Ignore ||
                event_class == RuntimeEventClass::Invalid)
            {
                if (s_runtimeCore.session.syncConfigPacket(s_runtimeCore.config_packet))
                {
                    pendingOsdFontReload();
                }
                break;
            }

            if (s_runtimeCore.session.syncConfigPacket(s_runtimeCore.config_packet))
            {
                pendingOsdFontReload();
            }
        }
    }
}

//===================================================================================
//===================================================================================
// Handles SIGTERM and SIGINT signals to perform graceful application shutdown.
void signalHandler(int signal)
{
    if (signal == SIGTERM || signal == SIGINT)
    {
        std::cout << "Received termination signal. Exiting gracefully..." << std::endl;
        cleanupLinuxSingleInstancePidFile();
        s_RuntimePlatformServices->exitApp();
    }
}

} // namespace


//===================================================================================
//===================================================================================
// Builds and registers the per-frame render callback with the HAL.
// The callback draws the OSD, top overlay, stats panel, menu, and processes hotkeys.
void registerLinuxRenderCallback(Ground2Air_Config_Packet& config, char* argv[])
{
    s_hal->set_pointer_tap_callback([](float tap_x, float tap_y)
    {
        handleLinuxPointerTap(tap_x, tap_y);
    });

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
            overlay_stats_snapshot.ground_stats = last_gs_stats;
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

        bool ignore_keys = gs::menu::g_osdMenuController.visible;

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("fullscreen", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        {
            const ImVec2 display_size = ImGui::GetIO().DisplaySize;
            const float overlay_width = frame_ui.vr_mode ? display_size.x * 0.5f : display_size.x;
            // Linux VR is authored once into the left-eye area; PI_HAL replays the
            // final ImGui draw data into both halves so menu/overlay/video match.
            s_flightOSD.draw(static_cast<int>(overlay_width),
                             static_cast<int>(display_size.y),
                             frame_ui.flight_osd_is_16x9 ? 16 : 4,
                             frame_ui.flight_osd_is_16x9 ? 9 : 3,
                             static_cast<int>(frame_ui.screen_mode));

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
            input.throughput_mbps = static_cast<float>(s_total_data) * 8.0f / (1024.0f * 1024.0f);
            input.video_fps = static_cast<int>(video_fps);
            input.video_fps_alert = had_loss;
            input.no_ping = s_noPing;
            input.interference = shouldShowInterferenceChip(s_last_gs_stats);
            input.sd_slow = s_SDSlow;
            input.air_record = s_air_record;
            input.gs_record = s_recordingsStorage->isRecording();
            input.hq_dvr = isHQDVRMode();
            input.gs_temp_celsius = s_RuntimePlatformServices->getCpuTemperatureCelsius();
            input.osd_font_error = s_flightOSD.isFontError();
            input.incompatible_firmware_time = s_incompatibleFirmwareTime;
            input.now = Clock::now();
            input.transport_message = s_transport->getTransportMessage();

            gs::imgui::drawTopOverlayStatus(input, overlay_width);
            //------------------------------------------------

            if (frame_ui.overlay_stats_visible)
            {
                gs::stats::drawFullscreenStatsPanel(overlay_stats_snapshot);
            }

            drawPlaybackProgressOverlay(overlay_width, display_size.y);
            gs::calibration::drawCalibrationOverlay(overlay_width, display_size.y);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);

        const bool menu_visible = gs::menu::g_osdMenuController.isVisible();
        const bool playback_active = s_playbackManager != nullptr && s_playbackManager->status().active;
        RuntimeMenuUiState menu_ui = {};
        menu_ui.visible = menu_visible;
        menu_ui.vr_mode = s_groundstation_config.vrMode;
        // touch_nav_enabled intentionally left false for Linux/WSL (keyboard-driven, no DPAD/REC buttons)
        menu_ui.surface_width = s_groundstation_config.vrMode
            ? ImGui::GetIO().DisplaySize.x * 0.5f
            : ImGui::GetIO().DisplaySize.x;
        menu_ui.surface_height = ImGui::GetIO().DisplaySize.y;
        drawRuntimeMenuOverlay(menu_ui);
        gs::menu::g_osdMenuController.draw(config);
        RuntimeMenuUiState touch_nav_ui = menu_ui;
        touch_nav_ui.visible = menu_visible || playback_active || gs::calibration::isActive();
        touch_nav_ui.gs_recording = s_recordingsStorage != nullptr && s_recordingsStorage->isRecording();
        touch_nav_ui.air_recording = s_air_record;
        drawRuntimeMenuTouchNav(touch_nav_ui);

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

//===================================================================================
//===================================================================================
// Processes one frame tick: unlocks/locks decoder output, drives the HAL,
// and updates FPS and timing state.
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

//===================================================================================
//===================================================================================
// Initializes subsystems and runs the main Linux runtime loop until the application exits.
int runLinuxRuntimeLoop(char* argv[])
{
    ImGuiIO& io = ImGui::GetIO();

    s_decoder.init(*s_hal);

    setupNonBlockingInput();

    s_comms_thread = std::thread(&comms_thread_proc);

    Ground2Air_Config_Packet& config = s_runtimeCore.config_packet;
    config = s_runtimeCore.session.copyConfigPacket();
    size_t video_frame_count = 0;

    s_flightOSD.loadFont(s_flightOSD.selectedFontName().c_str());

    Clock::time_point last_stats_tp = Clock::now();
    Clock::time_point last_tp = Clock::now();

    registerLinuxRenderCallback(config, argv);

    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT, signalHandler);

    while (true)
    {
        processLinuxFrameTick(video_frame_count, last_stats_tp, last_tp, io);
    }

    return 0;
}
