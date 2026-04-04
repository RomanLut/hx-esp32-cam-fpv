#include "gs_linux_runtime_loop.h"

#include <cerrno>
#include <csignal>
#include <thread>
#include <unistd.h>

#include "Comms.h"
#include "PI_HAL.h"
#include "cpu_temp.h"
#include "flight_osd.h"
#include "shared/frame_packets_debug.h"
#include "gpio_buttons.h"
#include "gs_linux_render_helpers.h"
#include "gs_linux_run_loop.h"
#include "gs_linux_runtime.h"
#include "gs_linux_runtime_sink.h"
#include "gs_linux_startup.h"
#include "gs_runtime_core.h"
#include "gs_runtime_event_pipeline.h"
#include "gs_runtime_platform_services.h"
#include "gs_runtime_state.h"
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
Clock::time_point s_last_rc_command = Clock::now();

HXMavlinkParser mavlinkParserIn(true);

void comms_thread_proc()
{
    Clock::time_point last_stats_tp = Clock::now();
    Clock::time_point last_stats_tp10 = Clock::now();
    Clock::time_point last_comms_sent_tp = Clock::now();

    gs::core::VideoFrameAssembler videoFrameAssembler;

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
            const SessionPulseStats session_stats = consumeSessionPulseStats(s_runtimeCore.session, now);
            const gs::core::LinkStatusSnapshot& link_status = session_stats.link_status;
            const gs::core::PeriodicStatsSnapshot& periodic_stats = session_stats.periodic_stats;
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
                s_transport.send(control_payload.data(), control_payload.size(), true);
                s_runtimeCore.session.addSentPackets(1);
            }

            last_comms_sent_tp = Clock::now();
            s_runtimeCore.session.onPingSent(last_comms_sent_tp);
        }

        g_CPUTemp.process();

        if (g_serialTelemetry->isOpen())
        {
            const int frb = static_cast<int>(s_runtimeCore.session.telemetryFreeBytes());
            uint8_t* payload_write_ptr = s_runtimeCore.session.telemetryPayloadWritePtr();
            int n = g_serialTelemetry->read(payload_write_ptr, frb);

            bool gotRCPacket = false;

            if (n > 0)
            {
                uint8_t* dPtr = payload_write_ptr;
                for (int i = 0; i < n; i++)
                {
                    mavlinkParserIn.processByte(*dPtr++);
                    if (mavlinkParserIn.gotPacket() &&
                        mavlinkParserIn.getMessageId() == HX_MAXLINK_RC_CHANNELS_OVERRIDE)
                    {
                        Clock::time_point t = Clock::now();
                        int dt = std::chrono::duration_cast<std::chrono::milliseconds>(t - s_last_rc_command).count();
                        s_last_rc_command = t;
                        std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
                        s_gs_stats.RCPeriodMax = std::max(s_gs_stats.RCPeriodMax, dt);
                        gotRCPacket = true;
                    }
                }

                s_runtimeCore.session.appendTelemetryBytes(n);
                s_runtimeCore.session.addInboundTelemetryBytes(n);
            }

            const gs::core::TelemetryTxDecision telemetry_tx =
                s_runtimeCore.session.buildTelemetryTxDecision(gotRCPacket, Clock::now(), s_groundstation_config.deviceId);
            if (telemetry_tx.should_flush && telemetry_tx.should_send)
            {
                s_transport.send(&telemetry_tx.packet, telemetry_tx.packet.size, true);
                s_runtimeCore.session.addSentPackets(1);
            }
        }

        do
        {
            s_transport.process();
            bool restoredByFEC;
            if (!s_transport.receive(rx_data.data.data(), rx_data.size, restoredByFEC))
            {
                std::this_thread::yield();
                break;
            }

            const ProcessedSessionPacket processed_packet =
                processIncomingSessionPacket(s_runtimeCore.session,
                                             rx_data.data.data(),
                                             rx_data.size,
                                             s_groundstation_config.deviceId,
                                             s_transport);
            s_last_packet_tp = processed_packet.processed_tp;
            rx_data.rssi = (int16_t)s_transport.get_input_dBm();
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
                    },
                    [&](const ProcessedTelemetryEvent&, const TelemetryDispatchDecision& telemetry_decision)
                    {
                        handleLinuxTelemetryDispatch(telemetry_decision);
                    },
                    [&](const ProcessedOsdEvent&, const OsdDispatchDecision& osd_decision)
                    {
                        handleLinuxOsdDispatch(osd_decision);
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
                break;
            }
        }
        while (false);
    }
}

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

int runLinuxRuntimeLoop(char* argv[])
{
    ImGuiIO& io = ImGui::GetIO();

    s_decoder.init(*s_hal);

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    s_comms_thread = std::thread(&comms_thread_proc);

    Ground2Air_Config_Packet config = s_runtimeCore.session.copyConfigPacket();
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

