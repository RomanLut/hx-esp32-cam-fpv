#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/stats_panel_shared.h"
#include "gs_runtime_core.h"
#include "gs_runtime_frame_ui.h"
#include "gs_top_overlay_shared.h"

struct RuntimeDecodeStats
{
    uint32_t input_submitted_count = 0;
    uint32_t decoded_count = 0;
    uint32_t overwritten_pending_count = 0;
    uint32_t broken_frames = 0;
    uint32_t decoded_total_ms = 0;
    uint32_t decoded_min_ms = 0;
    uint32_t decoded_max_ms = 0;
};

struct RuntimeRendererStats
{
    uint32_t upload_count = 0;
    uint32_t upload_total_ms = 0;
    uint32_t upload_min_ms = 0;
    uint32_t upload_max_ms = 0;
    uint32_t swap_count = 0;
    uint32_t swap_total_ms = 0;
    uint32_t swap_min_ms = 0;
    uint32_t swap_max_ms = 0;
    uint32_t discarded_pending_count = 0;
};

struct RuntimeSyncParams
{
    RuntimeDecodeStats decode_stats = {};
    RuntimeRendererStats renderer_stats = {};
    std::string build_info;
    std::string osd_font_name;
    float throughput_mbps = 0.0f;
    float udp_video_fps = 0.0f;
    bool is_dual = false;
    bool osd_font_error = false;
};

struct RuntimeSyncState
{
    RuntimeFrameUiState frame_ui_state;
    gs::stats::FullscreenStatsSnapshot overlay_stats_snapshot;
    std::string build_info;
    std::string osd_font_name;
};

RuntimeSyncState collectRuntimeSyncState(GsRuntimeCore& core,
                                         const RuntimeSyncParams& params,
                                         gs::imgui::TopOverlayData& overlay_input);
