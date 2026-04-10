#include "gs_runtime_sync.h"

#include <algorithm>
#include <cmath>

#include "frame_packets_debug.h"
#include "gs_recordings_storage.h"
#include "gs_runtime_state.h"

RuntimeSyncState collectRuntimeSyncState(GsRuntimeCore& core,
                                         const RuntimeSyncParams& params,
                                         gs::imgui::TopOverlayData& overlay_input)
{
    RuntimeSyncState state;
    const Clock::time_point now = Clock::now();
    if (now - core.last_data_rate_sample_tp >= std::chrono::milliseconds(100))
    {
        core.data_size_stats.add(core.session.consumeDataRateSample());
        core.last_data_rate_sample_tp = now;
    }

    if (now - core.last_periodic_stats_tp >= std::chrono::milliseconds(1000))
    {
        const gs::core::LinkStatusSnapshot link_status = core.session.consumeLinkStatus(now);
        const gs::core::PeriodicStatsSnapshot periodic_stats = core.session.consumePeriodicStats();
        core.gs_stats.outPacketCounter = static_cast<uint16_t>(periodic_stats.sent_count);
        core.gs_stats.pingMinMS = link_status.ping_min_ms;
        core.gs_stats.pingMaxMS = link_status.ping_max_ms;
        const int8_t gs_rssi0 = core.gs_stats.rssiDbm[0];
        const int8_t gs_rssi1 = core.gs_stats.rssiDbm[1];
        const int8_t gs_noise_floor = core.gs_stats.noiseFloorDbm;

        const gs::core::VideoFrameAssembler::Stats assembler_stats = core.assembler.consumeStats();
        core.gs_stats.brokenFrames = static_cast<uint8_t>(std::min<uint32_t>(params.decode_stats.broken_frames, 255));

        core.last_ground_stats = core.gs_stats;
        core.last_ground_stats.decodedJpegCount = static_cast<int>(params.decode_stats.decoded_count);
        core.last_ground_stats.decodedJpegTimeTotalMS = static_cast<int>(params.decode_stats.decoded_total_ms);
        core.last_ground_stats.decodedJpegTimeMinMS = static_cast<int>(params.decode_stats.decoded_min_ms);
        core.last_ground_stats.decodedJpegTimeMaxMS = static_cast<int>(params.decode_stats.decoded_max_ms);
        core.last_ground_stats.textureUploadCount = static_cast<int>(params.renderer_stats.upload_count);
        core.last_ground_stats.textureUploadTimeTotalMS = static_cast<int>(params.renderer_stats.upload_total_ms);
        core.last_ground_stats.textureUploadTimeMinMS = static_cast<int>(params.renderer_stats.upload_min_ms);
        core.last_ground_stats.textureUploadTimeMaxMS = static_cast<int>(params.renderer_stats.upload_max_ms);
        core.last_ground_stats.discardedFramesAssemblerPoolOverflow =
            static_cast<int>(assembler_stats.discarded_frames);
        core.last_ground_stats.discardedFramesDecoderInput =
            static_cast<int>(params.decode_stats.overwritten_pending_count);
        core.last_ground_stats.discardedFramesDecodedOutput =
            static_cast<int>(params.renderer_stats.discarded_pending_count);
        core.last_ground_stats.restoredTransportPackets = core.restored_transport_packets;
        core.last_ground_stats.restoredVideoParts = core.restored_video_parts;
        core.last_ground_stats.receivedCompletedFrames = periodic_stats.received_completed_frames;
        core.last_ground_stats.restoredCompletedFrames = periodic_stats.restored_completed_frames;
        GSStats next_stats = {};
        next_stats.statsPacketIndex = core.last_ground_stats.lastPacketIndex;
        next_stats.rssiDbm[0] = gs_rssi0;
        next_stats.rssiDbm[1] = gs_rssi1;
        next_stats.noiseFloorDbm = gs_noise_floor;
        core.gs_stats = next_stats;
        core.restored_transport_packets = 0;
        core.restored_video_parts = 0;
        core.last_periodic_stats_tp = now;
    }

    s_isOV5640 = core.session.lastAirStats().isOV5640 != 0;
    s_isDual = params.is_dual;
    overlay_input.config = core.config_packet;
    overlay_input.air_rssi_dbm = core.session.lastAirStats().rssiDbm;
    overlay_input.air_temperature = core.session.lastAirStats().temperature;
    overlay_input.air_overheat = core.session.lastAirStats().overheatTrottling != 0;
    overlay_input.air_suspended = core.session.lastAirStats().suspended == 1;
    overlay_input.has_gs_stats = true;
    overlay_input.gs_rssi_dbm0 = core.last_ground_stats.rssiDbm[0];
    overlay_input.gs_rssi_dbm1 = core.last_ground_stats.rssiDbm[1];
    overlay_input.is_ov5640 = core.session.lastAirStats().isOV5640 != 0;
    overlay_input.is_dual = params.is_dual;
    overlay_input.wifi_queue_percent = core.session.lastAirStats().wifi_queue_max;
    overlay_input.wifi_queue_alert = core.session.lastAirStats().wifi_ovf != 0;
    overlay_input.throughput_mbps = params.throughput_mbps;
    overlay_input.video_fps = static_cast<int>(std::round(params.udp_video_fps));
    overlay_input.air_record = core.session.lastAirStats().air_record_state != 0;
    overlay_input.gs_record = s_recordingsStorage->isRecording();
    overlay_input.hq_dvr = core.session.lastAirStats().hq_dvr_mode != 0;
    overlay_input.interference = shouldShowInterferenceChip(core.last_ground_stats);
    overlay_input.osd_font_error = params.osd_font_error;
    overlay_input.incompatible_firmware_time = Clock::time_point{};
    overlay_input.now = now;
    overlay_input.link_state = getLinkState();

    if (core.groundstation_config.stats)
    {
        state.frame_ui_state.overlay_stats_visible = true;
        const auto& frame_stats = core.session.frameStats();
        gs::stats::FullscreenStatsSnapshot stats_snapshot = {};
        stats_snapshot.fec_codec_n = core.config_packet.dataChannel.fec_codec_n;
        stats_snapshot.current_quality = core.session.lastAirStats().curr_quality;
        stats_snapshot.wifi_queue_max = core.session.lastAirStats().wifi_queue_max;
        stats_snapshot.cpu_temp_c = 0;
        stats_snapshot.air_stats = core.session.lastAirStats();
        stats_snapshot.ground_stats = core.last_ground_stats;
        stats_snapshot.frame_stats = frame_stats.frame_stats;
        stats_snapshot.frame_parts_stats = frame_stats.frame_parts_stats;
        stats_snapshot.frame_time_stats = frame_stats.frame_time_stats;
        stats_snapshot.frame_quality_stats = frame_stats.frame_quality_stats;
        stats_snapshot.data_size_stats = core.data_size_stats;
        stats_snapshot.queue_usage_stats = frame_stats.queue_usage_stats;
        state.overlay_stats_snapshot = stats_snapshot;
    }
    state.frame_ui_state.menu_footer = params.build_info;
    state.frame_ui_state.screen_mode = core.groundstation_config.screenAspectRatio;
    state.frame_ui_state.vsync = core.groundstation_config.vsync;
    state.frame_ui_state.vr_mode = core.groundstation_config.vrMode;
    state.build_info = params.build_info;
    state.osd_font_name = params.osd_font_name;
    return state;
}
