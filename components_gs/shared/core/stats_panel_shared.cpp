#include "core/stats_panel_shared.h"
#include "core/osd_menu_imgui_shared.h"

#include <algorithm>
#include <cstdio>

namespace gs::stats
{

namespace
{

constexpr float kTopOverlayChipGap = 6.0f;

//===================================================================================
//===================================================================================
// Calculates packet loss ratio as a percentage from sent and received packet counts.
float calcLossRatio(int out_count, int in_count)
{
    if (out_count == 0)
    {
        return 0.0f;
    }
    const int loss = out_count - in_count;
    if (loss <= 0)
    {
        return 0.0f;
    }
    return (loss * 100.0f) / out_count;
}

//===================================================================================
//===================================================================================
// Prints an unknown stats value when the air-side stats snapshot is stale.
void textAirValue(bool valid, const char* format, int value)
{
    if (!valid)
    {
        ImGui::TextUnformatted("?");
        return;
    }
    ImGui::Text(format, value);
}

//===================================================================================
//===================================================================================
// Prints an unknown stats value when the air-side stats snapshot is stale.
void textAirValue(bool valid, const char* format, double value)
{
    if (!valid)
    {
        ImGui::TextUnformatted("?");
        return;
    }
    ImGui::Text(format, value);
}

} // namespace

//===================================================================================
//===================================================================================
// Draws the fullscreen stats graphs and tables over the video frame.
void drawFullscreenStatsPanel(const FullscreenStatsSnapshot& snapshot)
{
    const int fec_codec_n = snapshot.fec_codec_n;
    const int current_quality = snapshot.current_quality;
    const int wifi_queue_max = snapshot.wifi_queue_max;
    const int cpu_temp_c = snapshot.cpu_temp_c;
    const bool air_stats_valid = snapshot.air_stats_valid;
    const AirStats& air_stats = snapshot.air_stats;
    const GSStats& ground_stats = snapshot.ground_stats;
    const Stats& frame_stats = snapshot.frame_stats;
    const Stats& frame_parts_stats = snapshot.frame_parts_stats;
    const Stats& frame_time_stats = snapshot.frame_time_stats;
    const Stats& frame_quality_stats = snapshot.frame_quality_stats;
    const Stats& data_size_stats = snapshot.data_size_stats;
    const Stats& queue_usage_stats = snapshot.queue_usage_stats;

    char overlay[64];
    const int restored_parts = ground_stats.restoredVideoParts;
    const int received_parts = std::max(
        0,
        static_cast<int>(ground_stats.inUniquePacketCounter) -
            ground_stats.restoredTransportPackets);
    const int received_frames = ground_stats.receivedCompletedFrames;
    const int restored_frames = ground_stats.restoredCompletedFrames;

    const float osd_scale = gs::menu::imgui::calcOsdScale(ImGui::GetIO().DisplaySize.y);
    const float top_overlay_one_line_height = std::max(20.0f, ImGui::GetIO().DisplaySize.y * 0.04f);
    const float left_stats_start_y = top_overlay_one_line_height + (kTopOverlayChipGap * osd_scale);

    // Keep the left stats below one top-overlay row. The top overlay may wrap, but
    // stats intentionally reserve only one fixed row so the layout stays predictable.
    ImGui::SetCursorPosY(left_stats_start_y);
    ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.25f);
    std::snprintf(overlay, sizeof(overlay), "Frames %d+%d", received_frames, restored_frames);
    ImGui::PlotHistogram(overlay, Stats::getter, const_cast<Stats*>(&frame_stats), frame_stats.count(), 0, nullptr, 0, 4.0f, ImVec2(0, 24.0f * osd_scale));

    std::snprintf(
        overlay,
        sizeof(overlay),
        "Max: %d, %d+%d",
        static_cast<int>(frame_parts_stats.max()),
        received_parts,
        restored_parts);
    ImGui::PlotHistogram("Parts", Stats::getter, const_cast<Stats*>(&frame_parts_stats), frame_parts_stats.count(), 0, overlay, 0, frame_parts_stats.average() * 2 + 1.0f, ImVec2(0, 60.0f * osd_scale));
    ImGui::PlotHistogram("Period", Stats::getter, const_cast<Stats*>(&frame_time_stats), frame_time_stats.count(), 0, nullptr, 0, 100.0f, ImVec2(0, 60.0f * osd_scale));

    if (air_stats_valid)
    {
        std::snprintf(overlay, sizeof(overlay), "cur: %d", current_quality);
    }
    else
    {
        std::snprintf(overlay, sizeof(overlay), "cur: ?");
    }
    ImGui::PlotHistogram("Quality", Stats::getter, const_cast<Stats*>(&frame_quality_stats), frame_quality_stats.count(), 0, overlay, 0, 64.0f, ImVec2(0, 60.0f * osd_scale));

    std::snprintf(overlay, sizeof(overlay), "avg: %d KB/sec", static_cast<int>(data_size_stats.average() + 0.5f) * 10);
    ImGui::PlotHistogram("DataSize", Stats::getter, const_cast<Stats*>(&data_size_stats), data_size_stats.count(), 0, overlay, 0, 100.0f, ImVec2(0, 60.0f * osd_scale));

    if (air_stats_valid)
    {
        std::snprintf(overlay, sizeof(overlay), "%d%%", wifi_queue_max);
    }
    else
    {
        std::snprintf(overlay, sizeof(overlay), "?");
    }
    ImGui::PlotHistogram("Wifi Load", Stats::getter, const_cast<Stats*>(&queue_usage_stats), queue_usage_stats.count(), 0, overlay, 0, 100.0f, ImVec2(0, 60.0f * osd_scale));

    const float graphs_bottom_y = ImGui::GetCursorPosY();
    ImGui::PopItemWidth();

    const float table_width = 420.0f * osd_scale;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - table_width);
    ImGui::SetCursorPosY(10.0f * osd_scale);

    if (ImGui::BeginTable("table1", 2, 0, ImVec2(table_width, 24.0f * osd_scale)))
    {
        ImGuiStyle& style = ImGui::GetStyle();
        const ImU32 c = ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_FrameBg]);

        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 270.0f * osd_scale);

        auto row = [&](const char* name, auto draw_value)
        {
            ImGui::TableNextRow();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name);
            ImGui::TableSetColumnIndex(1);
            draw_value();
        };

        row("AirOutPacketRate", [&] { textAirValue(air_stats_valid, "%d pkt/s", air_stats.outPacketRate); });
        row("AirInPacketRate", [&] { textAirValue(air_stats_valid, "%d pkt/s", air_stats.inPacketRate); });
        row("AirOthersPacketRate", [&] { textAirValue(air_stats_valid, "%d pkt/s", air_stats.inRejectedPacketRate); });
        row("AirPacketLossRatio", [&] { textAirValue(air_stats_valid, "%.1f%%", static_cast<double>(calcLossRatio(ground_stats.outPacketCounter, air_stats.inPacketRate))); });
        row("GSOutPacketRate", [&] { ImGui::Text("%d pkt/s", ground_stats.outPacketCounter); });
        row("GSInPacketRateAll", [&] { ImGui::Text("%d+%d", ground_stats.inPacketCounterAll[0], ground_stats.inPacketCounterAll[1]); });
        row("GSInPacketRate", [&] { ImGui::Text("%d+%d", ground_stats.inPacketCounter[0], ground_stats.inPacketCounter[1]); });
        row("GSPacketLossRatio1", [&]
        {
            const int n = static_cast<int>((ground_stats.lastPacketIndex - ground_stats.statsPacketIndex) / 12 * fec_codec_n);
            ImGui::Text("%.1f,%.1f%%",
                        calcLossRatio(n, ground_stats.inPacketCounter[0]),
                        calcLossRatio(n, ground_stats.inPacketCounter[1]));
        });
        row("GSPacketLossRatio2", [&]
        {
            const int n = static_cast<int>((ground_stats.lastPacketIndex - ground_stats.statsPacketIndex) / 12 * fec_codec_n);
            ImGui::Text("%.1f%%", calcLossRatio(n, ground_stats.inUniquePacketCounter));
        });
        row("Air RSSI", [&] { textAirValue(air_stats_valid, "%d dbm", -air_stats.rssiDbm); });
        row("Air Noise Floor", [&] { textAirValue(air_stats_valid, "%d dbm", -air_stats.noiseFloorDbm); });
        row("Air SNR", [&] { textAirValue(air_stats_valid, "%d db", static_cast<int>(air_stats.noiseFloorDbm) - static_cast<int>(air_stats.rssiDbm)); });
        row("GS RSSI 1", [&] { textAirValue(air_stats_valid, "%d dbm", ground_stats.rssiDbm[0]); });
        row("GS RSSI 2", [&] { textAirValue(air_stats_valid, "%d dbm", ground_stats.rssiDbm[1]); });
        row("GS Noise Floor", [&] { textAirValue(air_stats_valid, "%d dbm", ground_stats.noiseFloorDbm); });
        row("Ping min", [&] { ImGui::Text("%d ms", ground_stats.pingMinMS); });
        row("Ping max", [&] { ImGui::Text("%d ms", ground_stats.pingMaxMS); });
        row("Capture FPS", [&] { textAirValue(air_stats_valid, "%d FPS", air_stats.captureFPS); });
        row("Frame size min", [&] { textAirValue(air_stats_valid, "%d b", air_stats.cam_frame_size_min); });
        row("Frame size max", [&] { textAirValue(air_stats_valid, "%d b", air_stats.cam_frame_size_max); });
        row("Camera Overflow", [&] { textAirValue(air_stats_valid, "%d", air_stats.cam_ovf_count); });
        row("Broken frames", [&] { ImGui::Text("%d", ground_stats.brokenFrames); });
        row("Temperature GS/Air", [&]
        {
            if (!air_stats_valid)
            {
                ImGui::Text("%dC/?", cpu_temp_c);
                return;
            }
            ImGui::Text("%dC/%dC", cpu_temp_c, static_cast<int>(air_stats.temperature));
        });

        ImGui::EndTable();
    }

    ImGui::SetCursorPosX(10.0f * osd_scale);
    ImGui::SetCursorPosY(graphs_bottom_y + 3.0f);

    if (ImGui::BeginTable("table2", 2, 0, ImVec2(table_width, 220.0f * osd_scale)))
    {
        ImGuiStyle& style = ImGui::GetStyle();
        const ImU32 c = ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_FrameBg]);

        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 270.0f * osd_scale);

        auto row = [&](const char* name, auto draw_value)
        {
            ImGui::TableNextRow();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name);
            ImGui::TableSetColumnIndex(1);
            draw_value();
        };

        row("Mavlink Up", [&] { textAirValue(air_stats_valid, "%d b/s", air_stats.inMavlinkRate); });
        row("Mavlink Down", [&] { textAirValue(air_stats_valid, "%d b/s", air_stats.outMavlinkRate); });
        row("GS RC Period Max", [&] { ImGui::Text("%d ms", ground_stats.RCPeriodMax); });
        row("AIR RC Period Max", [&]
        {
            int v = -1;
            if (!air_stats_valid)
            {
                ImGui::TextUnformatted("?");
                return;
            }
            if (air_stats.RCPeriodMax > 0 && air_stats.RCPeriodMax <= 100)
            {
                v = air_stats.RCPeriodMax;
            }
            else if (air_stats.RCPeriodMax > 100)
            {
                v = (static_cast<int>(air_stats.RCPeriodMax) - 101) * 10;
            }
            ImGui::Text("%d ms", v);
        });
        row("Jpeg Decode", [&]
        {
            const int avg = ground_stats.decodedJpegTimeTotalMS /
                            (ground_stats.decodedJpegCount ? ground_stats.decodedJpegCount : 1);
            ImGui::Text("%d/%d/%d ms",
                        ground_stats.decodedJpegTimeMinMS,
                        avg,
                        ground_stats.decodedJpegTimeMaxMS);
        });
        row("Texture Upload", [&]
        {
            const int avg = ground_stats.textureUploadTimeTotalMS /
                            (ground_stats.textureUploadCount ? ground_stats.textureUploadCount : 1);
            ImGui::Text("%d/%d/%d ms",
                        ground_stats.textureUploadTimeMinMS,
                        avg,
                        ground_stats.textureUploadTimeMaxMS);
        });
        row("Waiting for GPU", [&]
        {
            ImGui::Text("%d/%d ms",
                        ground_stats.gpuWaitLastFrameMS,
                        ground_stats.gpuWaitMaxMS);
        });
        row("Stabilization", [&]
        {
            ImGui::Text("%d/%d+%d/%d ms",
                        ground_stats.stabilizationFeaturesLastMS,
                        ground_stats.stabilizationFeaturesMaxMS,
                        ground_stats.stabilizationMotionLastMS,
                        ground_stats.stabilizationMotionMaxMS);
        });
        row("Discarded frames", [&]
        {
            ImGui::Text("%d/%d/%d",
                        ground_stats.discardedFramesAssemblerPoolOverflow,
                        ground_stats.discardedFramesDecoderInput,
                        ground_stats.discardedFramesDecodedOutput);
        });
        ImGui::EndTable();
    }
}


} // namespace gs::stats
