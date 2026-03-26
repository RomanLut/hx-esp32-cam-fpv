#pragma once

#include "imgui.h"
#include "packets.h"
#include "stats.h"

namespace gs::stats
{

struct GroundStatsSnapshot
{
    uint16_t out_packet_counter = 0;
    uint16_t in_packet_counter[2] = {0, 0};
    uint32_t last_packet_index = 0;
    uint32_t stats_packet_index = 0;
    uint16_t in_duplicated_packet_counter = 0;
    uint16_t in_unique_packet_counter = 0;
    uint32_t fec_succ_packet_index_counter = 0;
    uint32_t fec_blocks_counter = 0;
    int8_t rssi_dbm[2] = {0, 0};
    int8_t noise_floor_dbm = 0;
    uint8_t broken_frames = 0;
    int ping_min_ms = 0;
    int ping_max_ms = 0;
    int rc_period_max = -1;
    int decoded_jpeg_count = 0;
    int decoded_jpeg_time_total_ms = 0;
    int decoded_jpeg_time_min_ms = 99;
    int decoded_jpeg_time_max_ms = 0;
    int restored_transport_packets = 0;
    int restored_video_parts = 0;
    int restored_completed_frames = 0;
    int abandoned_new_frame_waiting_next_part = 0;
};

struct FullscreenStatsSnapshot
{
    int fec_codec_n = 0;
    int current_quality = 0;
    int wifi_queue_max = 0;
    int cpu_temp_c = 0;
    AirStats air_stats = {};
    GroundStatsSnapshot ground_stats = {};
    Stats frame_stats;
    Stats frame_parts_stats;
    Stats frame_time_stats;
    Stats frame_quality_stats;
    Stats data_size_stats;
    Stats queue_usage_stats;
};

void drawFullscreenStatsPanel(const FullscreenStatsSnapshot& snapshot);

} // namespace gs::stats
