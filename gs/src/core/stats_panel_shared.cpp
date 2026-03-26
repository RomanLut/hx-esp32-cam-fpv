#include "core/stats_panel_shared.h"

#include <cstdio>

namespace gs::stats
{

namespace
{

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

} // namespace

void drawFullscreenStatsPanel(const FullscreenStatsSnapshot& snapshot)
{
    char overlay[64];

    ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.25f);
    ImGui::PlotHistogram("Frames", Stats::getter, const_cast<Stats*>(&snapshot.frame_stats), snapshot.frame_stats.count(), 0, nullptr, 0, 4.0f, ImVec2(0, 24));

    std::snprintf(overlay, sizeof(overlay), "max: %d", static_cast<int>(snapshot.frame_parts_stats.max()));
    ImGui::PlotHistogram("Parts", Stats::getter, const_cast<Stats*>(&snapshot.frame_parts_stats), snapshot.frame_parts_stats.count(), 0, overlay, 0, snapshot.frame_parts_stats.average() * 2 + 1.0f, ImVec2(0, 60));
    ImGui::PlotHistogram("Period", Stats::getter, const_cast<Stats*>(&snapshot.frame_time_stats), snapshot.frame_time_stats.count(), 0, nullptr, 0, 100.0f, ImVec2(0, 60));

    std::snprintf(overlay, sizeof(overlay), "cur: %d", snapshot.current_quality);
    ImGui::PlotHistogram("Quality", Stats::getter, const_cast<Stats*>(&snapshot.frame_quality_stats), snapshot.frame_quality_stats.count(), 0, overlay, 0, 64.0f, ImVec2(0, 60));

    std::snprintf(overlay, sizeof(overlay), "avg: %d KB/sec", static_cast<int>(snapshot.data_size_stats.average() + 0.5f) * 10);
    ImGui::PlotHistogram("DataSize", Stats::getter, const_cast<Stats*>(&snapshot.data_size_stats), snapshot.data_size_stats.count(), 0, overlay, 0, 100.0f, ImVec2(0, 60));

    std::snprintf(overlay, sizeof(overlay), "%d%%", snapshot.wifi_queue_max);
    ImGui::PlotHistogram("Wifi Load", Stats::getter, const_cast<Stats*>(&snapshot.queue_usage_stats), snapshot.queue_usage_stats.count(), 0, overlay, 0, 100.0f, ImVec2(0, 60));

    ImGui::PopItemWidth();

    const float table_width = 420.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - table_width);
    ImGui::SetCursorPosY(10);

    if (ImGui::BeginTable("table1", 2, 0, ImVec2(table_width, 24.0f)))
    {
        ImGuiStyle& style = ImGui::GetStyle();
        const ImU32 c = ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_FrameBg]);

        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 270.0f);

        auto row = [&](const char* name, auto draw_value)
        {
            ImGui::TableNextRow();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name);
            ImGui::TableSetColumnIndex(1);
            draw_value();
        };

        row("AirOutPacketRate", [&] { ImGui::Text("%d pkt/s", snapshot.air_stats.outPacketRate); });
        row("AirInPacketRate", [&] { ImGui::Text("%d pkt/s", snapshot.air_stats.inPacketRate); });
        row("AirOthersPacketRate", [&] { ImGui::Text("%d pkt/s", snapshot.air_stats.inRejectedPacketRate); });
        row("AirPacketLossRatio", [&] { ImGui::Text("%.1f%%", calcLossRatio(snapshot.ground_stats.out_packet_counter, snapshot.air_stats.inPacketRate)); });
        row("GSOutPacketRate", [&] { ImGui::Text("%d pkt/s", snapshot.ground_stats.out_packet_counter); });
        row("GSInPacketRate", [&] { ImGui::Text("%d+%d", snapshot.ground_stats.in_packet_counter[0], snapshot.ground_stats.in_packet_counter[1]); });
        row("GSPacketLossRatio1", [&]
        {
            const int n = static_cast<int>((snapshot.ground_stats.last_packet_index - snapshot.ground_stats.stats_packet_index) / 12 * snapshot.fec_codec_n);
            ImGui::Text("%.1f,%.1f%%",
                        calcLossRatio(n, snapshot.ground_stats.in_packet_counter[0]),
                        calcLossRatio(n, snapshot.ground_stats.in_packet_counter[1]));
        });
        row("GSPacketLossRatio2", [&]
        {
            const int n = static_cast<int>((snapshot.ground_stats.last_packet_index - snapshot.ground_stats.stats_packet_index) / 12 * snapshot.fec_codec_n);
            ImGui::Text("%.1f%%", calcLossRatio(n, snapshot.ground_stats.in_unique_packet_counter));
        });
        row("Air RSSI", [&] { ImGui::Text("%d dbm", -snapshot.air_stats.rssiDbm); });
        row("Air Noise Floor", [&] { ImGui::Text("%d dbm", -snapshot.air_stats.noiseFloorDbm); });
        row("Air SNR", [&] { ImGui::Text("%d db", static_cast<int>(snapshot.air_stats.noiseFloorDbm) - static_cast<int>(snapshot.air_stats.rssiDbm)); });
        row("GS RSSI 1", [&] { ImGui::Text("%d dbm", snapshot.ground_stats.rssi_dbm[0]); });
        row("GS RSSI 2", [&] { ImGui::Text("%d dbm", snapshot.ground_stats.rssi_dbm[1]); });
        row("GS Noise Floor", [&] { ImGui::Text("%d dbm", snapshot.ground_stats.noise_floor_dbm); });
        row("Ping min", [&] { ImGui::Text("%d ms", snapshot.ground_stats.ping_min_ms); });
        row("Ping max", [&] { ImGui::Text("%d ms", snapshot.ground_stats.ping_max_ms); });
        row("Capture FPS", [&] { ImGui::Text("%d FPS", snapshot.air_stats.captureFPS); });
        row("Frame size min", [&] { ImGui::Text("%d b", snapshot.air_stats.cam_frame_size_min); });
        row("Frame size max", [&] { ImGui::Text("%d b", snapshot.air_stats.cam_frame_size_max); });
        row("Camera Overflow", [&] { ImGui::Text("%d", snapshot.air_stats.cam_ovf_count); });
        row("Broken frames", [&] { ImGui::Text("%d", snapshot.ground_stats.broken_frames); });
        row("Temperature GS/Air", [&] { ImGui::Text("%dC/%dC", snapshot.cpu_temp_c, static_cast<int>(snapshot.air_stats.temperature)); });

        ImGui::EndTable();
    }

    ImGui::SetCursorPosX(10);
    ImGui::SetCursorPosY(400);

    if (ImGui::BeginTable("table2", 2, 0, ImVec2(table_width, 24.0f)))
    {
        ImGuiStyle& style = ImGui::GetStyle();
        const ImU32 c = ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_FrameBg]);

        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 270.0f);

        auto row = [&](const char* name, auto draw_value)
        {
            ImGui::TableNextRow();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name);
            ImGui::TableSetColumnIndex(1);
            draw_value();
        };

        row("Mavlink Up", [&] { ImGui::Text("%d b/s", snapshot.air_stats.inMavlinkRate); });
        row("Mavlink Down", [&] { ImGui::Text("%d b/s", snapshot.air_stats.outMavlinkRate); });
        row("GS RC Period Max", [&] { ImGui::Text("%d ms", snapshot.ground_stats.rc_period_max); });
        row("AIR RC Period Max", [&]
        {
            int v = -1;
            if (snapshot.air_stats.RCPeriodMax > 0 && snapshot.air_stats.RCPeriodMax <= 100)
            {
                v = snapshot.air_stats.RCPeriodMax;
            }
            else if (snapshot.air_stats.RCPeriodMax > 100)
            {
                v = (static_cast<int>(snapshot.air_stats.RCPeriodMax) - 101) * 10;
            }
            ImGui::Text("%d ms", v);
        });
        row("Jpeg Decode", [&]
        {
            const int avg = snapshot.ground_stats.decoded_jpeg_time_total_ms /
                            (snapshot.ground_stats.decoded_jpeg_count ? snapshot.ground_stats.decoded_jpeg_count : 1);
            ImGui::Text("%d/%d/%d ms",
                        snapshot.ground_stats.decoded_jpeg_time_min_ms,
                        avg,
                        snapshot.ground_stats.decoded_jpeg_time_max_ms);
        });

        ImGui::EndTable();
    }
}

} // namespace gs::stats
