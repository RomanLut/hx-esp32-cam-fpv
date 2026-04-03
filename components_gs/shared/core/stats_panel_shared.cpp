#include "core/stats_panel_shared.h"

#include <algorithm>
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

GroundStatsSnapshot buildGroundStatsSnapshot(const GSStats& gs_stats)
{
    GroundStatsSnapshot ground_stats = {};
    ground_stats.out_packet_counter = gs_stats.outPacketCounter;
    ground_stats.in_packet_counter[0] = gs_stats.inPacketCounter[0];
    ground_stats.in_packet_counter[1] = gs_stats.inPacketCounter[1];
    ground_stats.last_packet_index = gs_stats.lastPacketIndex;
    ground_stats.stats_packet_index = gs_stats.statsPacketIndex;
    ground_stats.in_duplicated_packet_counter = gs_stats.inDublicatedPacketCounter;
    ground_stats.in_unique_packet_counter = gs_stats.inUniquePacketCounter;
    ground_stats.fec_succ_packet_index_counter = gs_stats.FECSuccPacketIndexCounter;
    ground_stats.fec_blocks_counter = gs_stats.FECBlocksCounter;
    ground_stats.rssi_dbm[0] = gs_stats.rssiDbm[0];
    ground_stats.rssi_dbm[1] = gs_stats.rssiDbm[1];
    ground_stats.noise_floor_dbm = gs_stats.noiseFloorDbm;
    ground_stats.broken_frames = gs_stats.brokenFrames;
    ground_stats.ping_min_ms = gs_stats.pingMinMS;
    ground_stats.ping_max_ms = gs_stats.pingMaxMS;
    ground_stats.rc_period_max = gs_stats.RCPeriodMax;
    ground_stats.decoded_jpeg_count = gs_stats.decodedJpegCount;
    ground_stats.decoded_jpeg_time_total_ms = gs_stats.decodedJpegTimeTotalMS;
    ground_stats.decoded_jpeg_time_min_ms = gs_stats.decodedJpegTimeMinMS;
    ground_stats.decoded_jpeg_time_max_ms = gs_stats.decodedJpegTimeMaxMS;
    ground_stats.texture_upload_count = gs_stats.textureUploadCount;
    ground_stats.texture_upload_time_total_ms = gs_stats.textureUploadTimeTotalMS;
    ground_stats.texture_upload_time_min_ms = gs_stats.textureUploadTimeMinMS;
    ground_stats.texture_upload_time_max_ms = gs_stats.textureUploadTimeMaxMS;
    ground_stats.discarded_frames_assembler_pool_overflow = gs_stats.discardedFramesAssemblerPoolOverflow;
    ground_stats.discarded_frames_decoder_input = gs_stats.discardedFramesDecoderInput;
    ground_stats.discarded_frames_decoded_output = gs_stats.discardedFramesDecodedOutput;
    ground_stats.restored_transport_packets = gs_stats.restoredTransportPackets;
    ground_stats.restored_video_parts = gs_stats.restoredVideoParts;
    ground_stats.received_completed_frames = gs_stats.receivedCompletedFrames;
    ground_stats.restored_completed_frames = gs_stats.restoredCompletedFrames;
    return ground_stats;
}


void drawFullscreenStatsPanel(const FullscreenStatsSnapshot& snapshot)
{
    const int fec_codec_n = snapshot.fec_codec_n;
    const int current_quality = snapshot.current_quality;
    const int wifi_queue_max = snapshot.wifi_queue_max;
    const int cpu_temp_c = snapshot.cpu_temp_c;
    const AirStats& air_stats = snapshot.air_stats;
    const GroundStatsSnapshot& ground_stats = snapshot.ground_stats;
    const Stats& frame_stats = snapshot.frame_stats;
    const Stats& frame_parts_stats = snapshot.frame_parts_stats;
    const Stats& frame_time_stats = snapshot.frame_time_stats;
    const Stats& frame_quality_stats = snapshot.frame_quality_stats;
    const Stats& data_size_stats = snapshot.data_size_stats;
    const Stats& queue_usage_stats = snapshot.queue_usage_stats;

    char overlay[64];
    const int restored_parts = ground_stats.restored_video_parts;
    const int received_parts = std::max(
        0,
        static_cast<int>(ground_stats.in_unique_packet_counter) -
            ground_stats.restored_transport_packets);
    const int received_frames = ground_stats.received_completed_frames;
    const int restored_frames = ground_stats.restored_completed_frames;

    ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.25f);
    std::snprintf(overlay, sizeof(overlay), "Frames %d+%d", received_frames, restored_frames);
    ImGui::PlotHistogram(overlay, Stats::getter, const_cast<Stats*>(&frame_stats), frame_stats.count(), 0, nullptr, 0, 4.0f, ImVec2(0, 24));

    std::snprintf(
        overlay,
        sizeof(overlay),
        "Max: %d, %d+%d",
        static_cast<int>(frame_parts_stats.max()),
        received_parts,
        restored_parts);
    ImGui::PlotHistogram("Parts", Stats::getter, const_cast<Stats*>(&frame_parts_stats), frame_parts_stats.count(), 0, overlay, 0, frame_parts_stats.average() * 2 + 1.0f, ImVec2(0, 60));
    ImGui::PlotHistogram("Period", Stats::getter, const_cast<Stats*>(&frame_time_stats), frame_time_stats.count(), 0, nullptr, 0, 100.0f, ImVec2(0, 60));

    std::snprintf(overlay, sizeof(overlay), "cur: %d", current_quality);
    ImGui::PlotHistogram("Quality", Stats::getter, const_cast<Stats*>(&frame_quality_stats), frame_quality_stats.count(), 0, overlay, 0, 64.0f, ImVec2(0, 60));

    std::snprintf(overlay, sizeof(overlay), "avg: %d KB/sec", static_cast<int>(data_size_stats.average() + 0.5f) * 10);
    ImGui::PlotHistogram("DataSize", Stats::getter, const_cast<Stats*>(&data_size_stats), data_size_stats.count(), 0, overlay, 0, 100.0f, ImVec2(0, 60));

    std::snprintf(overlay, sizeof(overlay), "%d%%", wifi_queue_max);
    ImGui::PlotHistogram("Wifi Load", Stats::getter, const_cast<Stats*>(&queue_usage_stats), queue_usage_stats.count(), 0, overlay, 0, 100.0f, ImVec2(0, 60));

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

        row("AirOutPacketRate", [&] { ImGui::Text("%d pkt/s", air_stats.outPacketRate); });
        row("AirInPacketRate", [&] { ImGui::Text("%d pkt/s", air_stats.inPacketRate); });
        row("AirOthersPacketRate", [&] { ImGui::Text("%d pkt/s", air_stats.inRejectedPacketRate); });
        row("AirPacketLossRatio", [&] { ImGui::Text("%.1f%%", calcLossRatio(ground_stats.out_packet_counter, air_stats.inPacketRate)); });
        row("GSOutPacketRate", [&] { ImGui::Text("%d pkt/s", ground_stats.out_packet_counter); });
        row("GSInPacketRate", [&] { ImGui::Text("%d+%d", ground_stats.in_packet_counter[0], ground_stats.in_packet_counter[1]); });
        row("GSPacketLossRatio1", [&]
        {
            const int n = static_cast<int>((ground_stats.last_packet_index - ground_stats.stats_packet_index) / 12 * fec_codec_n);
            ImGui::Text("%.1f,%.1f%%",
                        calcLossRatio(n, ground_stats.in_packet_counter[0]),
                        calcLossRatio(n, ground_stats.in_packet_counter[1]));
        });
        row("GSPacketLossRatio2", [&]
        {
            const int n = static_cast<int>((ground_stats.last_packet_index - ground_stats.stats_packet_index) / 12 * fec_codec_n);
            ImGui::Text("%.1f%%", calcLossRatio(n, ground_stats.in_unique_packet_counter));
        });
        row("Air RSSI", [&] { ImGui::Text("%d dbm", -air_stats.rssiDbm); });
        row("Air Noise Floor", [&] { ImGui::Text("%d dbm", -air_stats.noiseFloorDbm); });
        row("Air SNR", [&] { ImGui::Text("%d db", static_cast<int>(air_stats.noiseFloorDbm) - static_cast<int>(air_stats.rssiDbm)); });
        row("GS RSSI 1", [&] { ImGui::Text("%d dbm", ground_stats.rssi_dbm[0]); });
        row("GS RSSI 2", [&] { ImGui::Text("%d dbm", ground_stats.rssi_dbm[1]); });
        row("GS Noise Floor", [&] { ImGui::Text("%d dbm", ground_stats.noise_floor_dbm); });
        row("Ping min", [&] { ImGui::Text("%d ms", ground_stats.ping_min_ms); });
        row("Ping max", [&] { ImGui::Text("%d ms", ground_stats.ping_max_ms); });
        row("Capture FPS", [&] { ImGui::Text("%d FPS", air_stats.captureFPS); });
        row("Frame size min", [&] { ImGui::Text("%d b", air_stats.cam_frame_size_min); });
        row("Frame size max", [&] { ImGui::Text("%d b", air_stats.cam_frame_size_max); });
        row("Camera Overflow", [&] { ImGui::Text("%d", air_stats.cam_ovf_count); });
        row("Broken frames", [&] { ImGui::Text("%d", ground_stats.broken_frames); });
        row("Temperature GS/Air", [&] { ImGui::Text("%dC/%dC", cpu_temp_c, static_cast<int>(air_stats.temperature)); });

        ImGui::EndTable();
    }

    ImGui::SetCursorPosX(10);
    ImGui::SetCursorPosY(340);

    if (ImGui::BeginTable("table2", 2, 0, ImVec2(table_width, 220.0f)))
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

        row("Mavlink Up", [&] { ImGui::Text("%d b/s", air_stats.inMavlinkRate); });
        row("Mavlink Down", [&] { ImGui::Text("%d b/s", air_stats.outMavlinkRate); });
        row("GS RC Period Max", [&] { ImGui::Text("%d ms", ground_stats.rc_period_max); });
        row("AIR RC Period Max", [&]
        {
            int v = -1;
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
            const int avg = ground_stats.decoded_jpeg_time_total_ms /
                            (ground_stats.decoded_jpeg_count ? ground_stats.decoded_jpeg_count : 1);
            ImGui::Text("%d/%d/%d ms",
                        ground_stats.decoded_jpeg_time_min_ms,
                        avg,
                        ground_stats.decoded_jpeg_time_max_ms);
        });
        row("Texture Upload", [&]
        {
            const int avg = ground_stats.texture_upload_time_total_ms /
                            (ground_stats.texture_upload_count ? ground_stats.texture_upload_count : 1);
            ImGui::Text("%d/%d/%d ms",
                        ground_stats.texture_upload_time_min_ms,
                        avg,
                        ground_stats.texture_upload_time_max_ms);
        });
        row("Discarded frames", [&]
        {
            ImGui::Text("%d/%d/%d",
                        ground_stats.discarded_frames_assembler_pool_overflow,
                        ground_stats.discarded_frames_decoder_input,
                        ground_stats.discarded_frames_decoded_output);
        });
        ImGui::EndTable();
    }
}


} // namespace gs::stats
