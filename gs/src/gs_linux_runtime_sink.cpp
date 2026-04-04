#include "gs_linux_runtime_sink.h"

#include "avi.h"
#include "flight_osd.h"
#include "frame_packets_debug.h"
#include "gs_recordings_storage.h"
#include "gs_linux_runtime.h"
#include "gs_runtime_core.h"
#include "gs_stats.h"
#include "jpeg_parser.h"
#include "Log.h"
#include "gs_linux_socket.h"
#include "gs_udp_broadcast.h"

void logInvalidLinuxSessionEvent(const gs::core::SessionEvent& event, uint32_t current_frame_index)
{
    if (event.kind == gs::core::SessionEventKind::InvalidVideoPacket)
    {
        LOGE("Video frame {}: invalid packet {}", current_frame_index, event.packet_info.packetSize);
    }
    else if (event.kind == gs::core::SessionEventKind::InvalidTelemetryPacket)
    {
        LOGE("Telemetry frame: invalid packet {}", event.packet_info.packetSize);
    }
    else if (event.kind == gs::core::SessionEventKind::InvalidOsdPacket)
    {
        LOGE("OSD frame: invalid packet");
    }
    else if (event.kind == gs::core::SessionEventKind::UnsupportedPacket)
    {
        LOGE("Unknown air packet: {}", event.packet_info.header->type);
    }
}

void handleLinuxVideoDispatch(const VideoDispatchDecision& video_decision)
{
    if (video_decision.restored_video_part)
    {
        s_runtimeCore.restored_video_parts++;
        std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
        s_gs_stats.restoredVideoParts++;
    }

    const CompletedVideoFrameView& completed_frame = video_decision.completed_frame;
    if (!completed_frame.has_frame)
    {
        return;
    }

    auto video_frame = completed_frame.frame_data;
    uint8_t* video_frame_data = completed_frame.data;
    s_decoder.decode_data(video_frame, completed_frame.frame_index);
    if (s_recordingsStorage->isRecording())
    {
        s_recordingsStorage->writeVideoFrame(video_frame_data, video_frame->data.size());
    }

    if (g_gsUDPBroadcast && g_gsUDPBroadcast->isOpen())
    {
        g_gsUDPBroadcast->sendVideoFrame(video_frame_data, completed_frame.size);
    }
}

void handleLinuxOsdDispatch(const OsdDispatchDecision& osd_decision)
{
    syncAirStatusGlobals();

    if (osd_decision.should_apply && !g_framePacketsDebug.isOn())
    {
        s_flightOSD.update(osd_decision.payload, osd_decision.payload_size);
    }

    s_last_stats_packet_tp = Clock::now();
}

void handleLinuxTelemetryDispatch(const TelemetryDispatchDecision& telemetry_decision)
{
    if (g_serialTelemetry->isOpen() && telemetry_decision.has_payload)
    {
        g_serialTelemetry->write(telemetry_decision.payload, telemetry_decision.payload_size);
        s_runtimeCore.session.addOutboundTelemetryBytes(telemetry_decision.payload_size);
    }
}
