#pragma once

#include "imgui.h"
#include "gs_stats.h"
#include "packets.h"
#include "stats.h"

namespace gs::stats
{

struct FullscreenStatsSnapshot
{
    int fec_codec_n = 0;
    int current_quality = 0;
    int wifi_queue_max = 0;
    int cpu_temp_c = 0;
    AirStats air_stats = {};
    GSStats ground_stats = {};
    Stats frame_stats;
    Stats frame_parts_stats;
    Stats frame_time_stats;
    Stats frame_quality_stats;
    Stats data_size_stats;
    Stats queue_usage_stats;
};

void drawFullscreenStatsPanel(const FullscreenStatsSnapshot& snapshot);

} // namespace gs::stats
