#include "gs_runtime_sync.h"

#include <algorithm>
#include <cmath>

#include "frame_packets_debug.h"
#include "gs_recordings_storage.h"
#include "gs_runtime_platform_services.h"
#include "gs_runtime_state.h"
#include "gs_video_stabilization_shared.h"

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

    // Accumulate decode and upload stats every call so the 1-second snapshot
    // reflects the full window instead of only the last frame's slice.
    if (params.decode_stats.decoded_count > 0)
    {
        core.acc_decode_count += params.decode_stats.decoded_count;
        core.acc_decode_total_ms += params.decode_stats.decoded_total_ms;
        core.acc_decode_min_ms = std::min(core.acc_decode_min_ms, params.decode_stats.decoded_min_ms);
        core.acc_decode_max_ms = std::max(core.acc_decode_max_ms, params.decode_stats.decoded_max_ms);
    }
    if (params.renderer_stats.upload_count > 0)
    {
        core.acc_upload_count += params.renderer_stats.upload_count;
        core.acc_upload_total_ms += params.renderer_stats.upload_total_ms;
        core.acc_upload_min_ms = std::min(core.acc_upload_min_ms, params.renderer_stats.upload_min_ms);
        core.acc_upload_max_ms = std::max(core.acc_upload_max_ms, params.renderer_stats.upload_max_ms);
    }

    if (now - core.last_periodic_stats_tp >= std::chrono::milliseconds(1000))
    {
        const gs::core::LinkStatusSnapshot link_status = core.session.consumeLinkStatus(now);
        const gs::core::PeriodicStatsSnapshot periodic_stats = core.session.consumePeriodicStats();
        core.last_throughput_mbps = static_cast<float>(periodic_stats.total_data) * 8.0f / (1024.0f * 1024.0f);
        core.gs_stats.outPacketCounter = static_cast<uint16_t>(periodic_stats.sent_count);
        core.gs_stats.pingMinMS = link_status.ping_min_ms;
        core.gs_stats.pingMaxMS = link_status.ping_max_ms;
        const int8_t gs_rssi0 = core.gs_stats.rssiDbm[0];
        const int8_t gs_rssi1 = core.gs_stats.rssiDbm[1];
        const int8_t gs_noise_floor = core.gs_stats.noiseFloorDbm;

        const gs::core::VideoFrameAssembler::Stats assembler_stats = core.assembler.consumeStats();
        const gs::stabilization::StabilizationStats stabilization_stats =
            gs::stabilization::consumeStats();
        core.gs_stats.brokenFrames = static_cast<uint8_t>(std::min<uint32_t>(params.decode_stats.broken_frames, 255));
        core.gs_stats.receivedCompletedFrames = periodic_stats.received_completed_frames;
        core.gs_stats.restoredCompletedFrames = periodic_stats.restored_completed_frames;
        core.gs_stats.decodedJpegCount = static_cast<int>(core.acc_decode_count);
        core.gs_stats.decodedJpegTimeTotalMS = static_cast<int>(core.acc_decode_total_ms);
        core.gs_stats.decodedJpegTimeMinMS =
            static_cast<int>(core.acc_decode_count > 0 ? core.acc_decode_min_ms : 0);
        core.gs_stats.decodedJpegTimeMaxMS = static_cast<int>(core.acc_decode_max_ms);
        core.gs_stats.textureUploadCount = static_cast<int>(core.acc_upload_count);
        core.gs_stats.textureUploadTimeTotalMS = static_cast<int>(core.acc_upload_total_ms);
        core.gs_stats.textureUploadTimeMinMS =
            static_cast<int>(core.acc_upload_count > 0 ? core.acc_upload_min_ms : 0);
        core.gs_stats.textureUploadTimeMaxMS = static_cast<int>(core.acc_upload_max_ms);
        core.gs_stats.stabilizationCount = static_cast<int>(stabilization_stats.count);
        core.gs_stats.stabilizationTimeMinMS = static_cast<int>(stabilization_stats.min_ms);
        core.gs_stats.stabilizationTimeMaxMS = static_cast<int>(stabilization_stats.max_ms);
        core.gs_stats.discardedFramesAssemblerPoolOverflow =
            static_cast<int>(assembler_stats.discarded_frames);
        core.gs_stats.discardedFramesDecoderInput =
            static_cast<int>(params.decode_stats.overwritten_pending_count);
        core.gs_stats.discardedFramesDecodedOutput =
            static_cast<int>(params.renderer_stats.discarded_pending_count);
        core.gs_stats.restoredTransportPackets = core.restored_transport_packets;
        core.gs_stats.restoredVideoParts = core.restored_video_parts;

        core.last_ground_stats = core.gs_stats;

        core.acc_decode_count = 0;
        core.acc_decode_total_ms = 0;
        core.acc_decode_min_ms = 9999;
        core.acc_decode_max_ms = 0;
        core.acc_upload_count = 0;
        core.acc_upload_total_ms = 0;
        core.acc_upload_min_ms = 9999;
        core.acc_upload_max_ms = 0;
        core.last_had_frame_loss = core.session.consumeLostFrameCount() != 0;
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
    overlay_input.throughput_mbps = core.last_throughput_mbps;
    overlay_input.video_fps = core.last_ground_stats.receivedCompletedFrames +
        core.last_ground_stats.restoredCompletedFrames;
    overlay_input.video_fps_alert = core.last_had_frame_loss;
    overlay_input.air_record = core.session.lastAirStats().air_record_state != 0;
    overlay_input.gs_record = s_recordingsStorage->isRecording();
    overlay_input.hq_dvr = core.session.lastAirStats().hq_dvr_mode != 0;
    overlay_input.gs_temp_celsius = s_RuntimePlatformServices != nullptr
        ? s_RuntimePlatformServices->getCpuTemperatureCelsius()
        : 0.0f;
    overlay_input.interference = shouldShowInterferenceChip(core.last_ground_stats);
    overlay_input.osd_font_error = params.osd_font_error;
    overlay_input.incompatible_firmware_time = Clock::time_point{};
    overlay_input.now = now;

    if (core.groundstation_config.stats)
    {
        state.frame_ui_state.overlay_stats_visible = true;
        const auto& frame_stats = core.session.frameStats();
        gs::stats::FullscreenStatsSnapshot stats_snapshot = {};
        stats_snapshot.fec_codec_n = core.config_packet.dataChannel.fec_codec_n;
        stats_snapshot.current_quality = core.session.lastAirStats().curr_quality;
        stats_snapshot.wifi_queue_max = core.session.lastAirStats().wifi_queue_max;
        stats_snapshot.cpu_temp_c = static_cast<int>(overlay_input.gs_temp_celsius + 0.5f);
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
    state.frame_ui_state.screen_flip_v = core.groundstation_config.screenFlipV;
    state.frame_ui_state.screen_zoom = core.groundstation_config.screenZoom;
    state.frame_ui_state.vr_separation = core.groundstation_config.screenVrSeparation;
    state.build_info = params.build_info;
    state.osd_font_name = params.osd_font_name;
    return state;
}
