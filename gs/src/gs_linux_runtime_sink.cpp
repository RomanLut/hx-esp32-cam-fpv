#include "gs_linux_runtime_sink.h"

#include "avi.h"
#include "linux_osd.h"
#include "frame_packets_debug.h"
#include "gs_linux_runtime.h"
#include "gs_runtime_core.h"
#include "gs_stats.h"
#include "jpeg_parser.h"
#include "Log.h"
#include "gs_linux_socket.h"

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
    if (s_groundstation_config.record)
    {
#ifdef WRITE_RAW_MJPEG_STREAM
        std::lock_guard<std::mutex> lg(s_groundstation_config.record_mutex);
        fwrite(video_frame_data, video_frame->data.size(), 1, s_groundstation_config.record_file);
#else
        int width, height;
        if (getJPEGDimensions(video_frame_data, width, height, 2048))
        {
            if ((width != s_avi_frameWidth) ||
                (height != s_avi_frameHeight) ||
                (s_avi_ov2640HighFPS != s_runtimeCore.session.copyConfigPacket().camera.ov2640HighFPS) ||
                (s_avi_ov5640HighFPS != s_runtimeCore.session.copyConfigPacket().camera.ov5640HighFPS))
            {
                toggleGSRecording(0, 0, "auto_restart_resolution_change_stop");
                toggleGSRecording(width, height, "auto_restart_resolution_change_start");
            }

            {
                std::lock_guard<std::mutex> lg(s_groundstation_config.record_mutex);
                uint16_t jpegSize = video_frame->data.size();
                uint16_t filler = (4 - (jpegSize & 0x3)) & 0x3;
                size_t jpegSize1 = jpegSize + filler;
                uint8_t buf[8];
                memcpy(buf, dcBuf, 4);
                memcpy(&buf[4], &jpegSize1, 4);

                fwrite(buf, 8, 1, s_groundstation_config.record_file);
                fwrite(video_frame_data, video_frame->data.size(), 1, s_groundstation_config.record_file);

                memset(buf, 0, 4);
                fwrite(buf, filler, 1, s_groundstation_config.record_file);

                buildAviIdx(jpegSize1);
                s_avi_frameCnt++;
            }

            if ((s_avi_frameCnt == (DVR_MAX_FRAMES - 1)) || (moviSize > 50 * 1024 * 1024))
            {
                toggleGSRecording(0, 0, "auto_split_stop");
                toggleGSRecording(width, height, "auto_split_start");
            }
        }
        else
        {
            LOGI("Received frame - unknown size!");
        }
#endif
    }

    if (s_groundstation_config.socket_fd > 0)
    {
        send_data_to_udp(s_groundstation_config.socket_fd, video_frame_data, video_frame->data.size());
    }
}

void handleLinuxOsdDispatch(const OsdDispatchDecision& osd_decision)
{
    syncAirStatusGlobals();

    if (osd_decision.should_apply && !g_framePacketsDebug.isOn())
    {
        g_osd.update(osd_decision.payload, osd_decision.payload_size);
    }

    s_last_stats_packet_tp = Clock::now();
}

void handleLinuxTelemetryDispatch(const TelemetryDispatchDecision& telemetry_decision)
{
#ifdef USE_MAVLINK
    if (fdUART != -1 && telemetry_decision.has_payload)
    {
        write(fdUART, telemetry_decision.payload, telemetry_decision.payload_size);
        s_runtimeCore.session.addOutboundTelemetryBytes(telemetry_decision.payload_size);
    }
#else
    (void)telemetry_decision;
#endif
}
