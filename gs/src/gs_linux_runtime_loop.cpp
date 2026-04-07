#include "gs_linux_runtime_loop.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>
#include <unistd.h>

#include "Comms.h"
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
#include "gs_runtime_frame_ui.h"
#include "gs_runtime_menu_ui.h"
#include "gs_runtime_platform_services.h"
#include "gs_runtime_state.h"
#include "gs_top_overlay_shared.h"
#include "core/osd_menu_controller.h"
#include "hx_mavlink_parser.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "jpeg_parser.h"
#include "main.h"
#include "util.h"
#include "utils/utils.h"

namespace
{

std::thread s_comms_thread;

HXMavlinkParser mavlinkParserIn(true);

//===================================================================================
//===================================================================================
// Background thread that handles all communication: sending control packets,
// receiving and dispatching session packets, processing telemetry and video frames.
void comms_thread_proc()
{
    Clock::time_point last_stats_tp = Clock::now();
    Clock::time_point last_stats_tp10 = Clock::now();
    Clock::time_point last_comms_sent_tp = Clock::now();

    gs::core::VideoFrameAssembler videoFrameAssembler;

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
        if (Clock::now() - last_stats_tp >= std::chrono::milliseconds(1000))
        {
            const Clock::time_point now = Clock::now();
            const gs::core::LinkStatusSnapshot link_status = s_runtimeCore.session.consumeLinkStatus(now);
            const gs::core::PeriodicStatsSnapshot periodic_stats = s_runtimeCore.session.consumePeriodicStats();
            const gs::core::VideoFrameAssembler::Stats assembler_stats = videoFrameAssembler.consumeStats();
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
                s_runtimeCore.last_ground_stats = gs::stats::buildGroundStatsSnapshot(s_last_gs_stats);
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
                std::lock_guard<std::mutex> transport_lock(s_transport_mutex);
                s_transport->send(control_payload.data(), control_payload.size(), true);
                s_runtimeCore.session.addSentPackets(1);
            }

            last_comms_sent_tp = Clock::now();
            s_runtimeCore.session.onPingSent(last_comms_sent_tp);
        }

        g_CPUTemp.process();

        {
            std::lock_guard<std::mutex> transport_lock(s_transport_mutex);
            s_runtimeCore.session.processIncomingTelemetry(
                mavlinkParserIn,
                s_groundstation_config.deviceId,
                *s_transport,
                s_gs_stats_mutex,
                s_gs_stats);
        }

        do
        {
            std::lock_guard<std::mutex> transport_lock(s_transport_mutex);
            s_transport->process();
            rx_data.size = rx_data.data.size();
            bool restoredByFEC;
            if (!s_transport->receive(rx_data.data.data(), rx_data.size, restoredByFEC))
            {
                std::this_thread::yield();
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
                    logInvalidLinuxSessionEvent(invalid_event, videoFrameAssembler.currentFrameIndex());
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
                                              videoFrameAssembler,
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
        while (false);
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
// Initializes the OSD by loading the currently selected font.
void initializeLinuxOsd()
{
    const std::string font_name = s_flightOSD.selectedFontName();
    s_flightOSD.loadFont(font_name.c_str());
}

//===================================================================================
//===================================================================================
// Builds and registers the per-frame render callback with the HAL.
// The callback draws the OSD, top overlay, stats panel, menu, and processes hotkeys.
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

        bool ignore_keys = gs::menu::g_osdMenuController.visible;

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
        menu_ui.visible = gs::menu::g_osdMenuController.isVisible();
        menu_ui.vr_mode = s_groundstation_config.vrMode;
        menu_ui.touch_nav_enabled = false;
        menu_ui.surface_width = ImGui::GetIO().DisplaySize.x;
        menu_ui.surface_height = ImGui::GetIO().DisplaySize.y;
        drawRuntimeMenuOverlay(menu_ui);
        gs::menu::g_osdMenuController.draw(config);
        drawRuntimeMenuTouchNav(menu_ui);

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

    initializeLinuxOsd();

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
