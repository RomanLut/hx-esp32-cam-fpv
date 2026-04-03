#include "gs_runtime_sync.h"

#include <algorithm>
#include <cmath>

#include "gs_runtime_state.h"
#include "gs_runtime_session.h"

RuntimeSyncState collectRuntimeSyncState(GsRuntimeCore& core, const RuntimeSyncParams& params)
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
        const SessionPulseStats session_stats = consumeSessionPulseStats(core.session, now);
        const gs::core::PeriodicStatsSnapshot& periodic_stats = session_stats.periodic_stats;
        const gs::core::LinkStatusSnapshot& link_status = session_stats.link_status;
        core.gs_stats.outPacketCounter = static_cast<uint16_t>(periodic_stats.sent_count);
        core.gs_stats.pingMinMS = link_status.ping_min_ms;
        core.gs_stats.pingMaxMS = link_status.ping_max_ms;

        const gs::core::VideoFrameAssembler::Stats assembler_stats = core.assembler.consumeStats();
        core.gs_stats.brokenFrames = static_cast<uint8_t>(std::min<uint32_t>(params.decode_stats.broken_frames, 255));

        core.last_ground_stats = gs::stats::buildGroundStatsSnapshot(core.gs_stats);
        core.last_ground_stats.decoded_jpeg_count = static_cast<int>(params.decode_stats.decoded_count);
        core.last_ground_stats.decoded_jpeg_time_total_ms = static_cast<int>(params.decode_stats.decoded_total_ms);
        core.last_ground_stats.decoded_jpeg_time_min_ms = static_cast<int>(params.decode_stats.decoded_min_ms);
        core.last_ground_stats.decoded_jpeg_time_max_ms = static_cast<int>(params.decode_stats.decoded_max_ms);
        core.last_ground_stats.texture_upload_count = static_cast<int>(params.renderer_stats.upload_count);
        core.last_ground_stats.texture_upload_time_total_ms = static_cast<int>(params.renderer_stats.upload_total_ms);
        core.last_ground_stats.texture_upload_time_min_ms = static_cast<int>(params.renderer_stats.upload_min_ms);
        core.last_ground_stats.texture_upload_time_max_ms = static_cast<int>(params.renderer_stats.upload_max_ms);
        core.last_ground_stats.discarded_frames_assembler_pool_overflow =
            static_cast<int>(assembler_stats.discarded_frames);
        core.last_ground_stats.discarded_frames_decoder_input =
            static_cast<int>(params.decode_stats.overwritten_pending_count);
        core.last_ground_stats.discarded_frames_decoded_output =
            static_cast<int>(params.renderer_stats.discarded_pending_count);
        core.last_ground_stats.restored_transport_packets = core.restored_transport_packets;
        core.last_ground_stats.restored_video_parts = core.restored_video_parts;
        core.last_ground_stats.received_completed_frames = periodic_stats.received_completed_frames;
        core.last_ground_stats.restored_completed_frames = periodic_stats.restored_completed_frames;
        GSStats next_stats = {};
        next_stats.statsPacketIndex = core.last_ground_stats.last_packet_index;
        core.gs_stats = next_stats;
        core.restored_transport_packets = 0;
        core.restored_video_parts = 0;
        core.last_periodic_stats_tp = now;
    }

    RuntimeOverlayBuildInput overlay_input = {};
    s_isOV5640 = core.session.airStatus().is_ov5640;
    s_isDual = params.is_dual;
    overlay_input.config = &core.config_packet;
    overlay_input.air_stats = &core.session.lastAirStats();
    overlay_input.is_ov5640 = core.session.airStatus().is_ov5640;
    overlay_input.is_dual = params.is_dual;
    overlay_input.wifi_queue_percent = core.session.airStatus().wifi_queue_max;
    overlay_input.wifi_queue_alert = core.session.airStatus().wifi_ovf;
    overlay_input.throughput_mbps = params.throughput_mbps;
    overlay_input.video_fps = static_cast<int>(std::round(params.udp_video_fps));
    overlay_input.air_record = core.session.airStatus().air_record;
    overlay_input.gs_record = core.groundstation_config.record;
    overlay_input.hq_dvr = core.session.lastAirStats().hq_dvr_mode != 0;
    overlay_input.osd_font_error = params.osd_font_error;
    overlay_input.incompatible_firmware_time = Clock::time_point{};
    overlay_input.now = now;
    state.frame_ui_state.overlay = buildRuntimeOverlayState(overlay_input);
    state.frame_ui_state.overlay.gs_link_text = "GS:UDP";

    if (core.groundstation_config.stats)
    {
        state.frame_ui_state.overlay.stats_visible = true;
        const auto& frame_stats = core.session.frameStats();
        gs::stats::FullscreenStatsBuildInput stats_input = {};
        stats_input.fec_codec_n = core.config_packet.dataChannel.fec_codec_n;
        stats_input.current_quality = core.session.airStatus().curr_quality;
        stats_input.wifi_queue_max = core.session.airStatus().wifi_queue_max;
        stats_input.cpu_temp_c = 0;
        stats_input.air_stats = core.session.lastAirStats();
        stats_input.ground_stats = core.last_ground_stats;
        stats_input.frame_stats = frame_stats.frame_stats;
        stats_input.frame_parts_stats = frame_stats.frame_parts_stats;
        stats_input.frame_time_stats = frame_stats.frame_time_stats;
        stats_input.frame_quality_stats = frame_stats.frame_quality_stats;
        stats_input.data_size_stats = core.data_size_stats;
        stats_input.queue_usage_stats = frame_stats.queue_usage_stats;
        state.frame_ui_state.overlay.stats_snapshot = gs::stats::buildFullscreenStatsSnapshot(stats_input);
    }
    state.frame_ui_state.menu_footer = params.build_info;
    state.frame_ui_state.screen_mode = static_cast<int>(core.groundstation_config.screenAspectRatio);
    state.frame_ui_state.vsync = core.groundstation_config.vsync;
    state.frame_ui_state.vr_mode = core.groundstation_config.vrMode;
    state.build_info = params.build_info;
    state.osd_font_name = params.osd_font_name;
    state.clear_flight_osd = core.pending_flight_osd_clear;
    state.has_flight_osd_update = core.pending_flight_osd_dirty;
    if (state.has_flight_osd_update)
    {
        state.flight_osd_data = core.pending_flight_osd;
    }
    core.pending_flight_osd_clear = false;
    core.pending_flight_osd_dirty = false;
    if (core.frame_packets_debug.isVisible())
    {
        state.has_frame_debug_osd = true;
        state.frame_debug_osd = core.frame_packets_debug.osdChars();
    }
    return state;
}
