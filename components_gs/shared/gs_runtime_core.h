#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "../../components/common/Clock.h"
#include "core/gs_session_core.h"
#include "core/stats_panel_shared.h"
#include "core/transport.h"
#include "core/video_frame_assembler.h"
#include "fec.h"
#include "fec_block_decoder.h"
#include "flight_osd.h"
#include "gs_runtime_osd_font_storage.h"
#include "gs_shared_state.h"
#include "gs_stats.h"
#include "packets.h"
#include "stats.h"

struct GsRuntimeCore
{
    GsRuntimeCore(uint16_t gs_device_id_value,
                  TGroundstationConfig& groundstation_config_ref,
                  Ground2Air_Config_Packet& config_packet_ref);
    ~GsRuntimeCore();

    void resetState(uint16_t gs_device_id_value, bool clear_apfpv_state = true);
    void resetPairing(gs::core::ITransport& transport, Clock::time_point now);
    void resetTransportRuntime(gs::core::ITransport& transport, Clock::time_point now);
    void resetTransportRuntimePreserveApfpvState(gs::core::ITransport& transport, Clock::time_point now);

    uint16_t gs_device_id = 1;
    gs::core::GsSessionCore session;
    gs::core::VideoFrameAssembler assembler;
    FecBlockDecoder rx_decoder;
    uint8_t rx_decoder_k = FEC_K;
    uint8_t rx_decoder_n = 8;
    uint16_t rx_decoder_mtu = AIR2GROUND_MAX_MTU;
    Ground2Air_Config_Packet& config_packet;
    gs::core::SessionEventKind last_event_kind = gs::core::SessionEventKind::Ignore;
    std::vector<uint8_t> last_completed_frame;
    uint32_t next_tx_block_index = 1;
    fec_t* tx_fec = nullptr;
    bool tx_block_has_first_packet = false;
    std::array<uint8_t, GROUND2AIR_MAX_MTU> tx_first_packet_payload = {};
    uint32_t transport_packets_seen = 0;
    uint32_t transport_packets_passed_filter = 0;
    uint32_t transport_packets_filtered = 0;
    uint32_t decoded_packets_seen = 0;
    uint32_t last_decoded_type = 0;
    uint32_t last_decoded_size = 0;
    uint32_t last_decoded_air = 0;
    uint32_t last_decoded_gs = 0;
    uint32_t last_transport_block = 0;
    uint32_t last_transport_packet_index = 0;
    uint32_t last_transport_payload_size = 0;
    uint32_t last_transport_from = 0;
    uint32_t last_transport_to = 0;
    uint32_t last_video_frame_index = 0;
    uint32_t last_video_part_index = 0;
    uint32_t last_video_last_part = 0;
    uint32_t last_video_payload_size = 0;
    Clock::time_point last_packet_tp = Clock::now();
    TGroundstationConfig& groundstation_config;
    gs::stats::GroundStatsSnapshot last_ground_stats = {};
    GSStats gs_stats = {};
    GSStats last_gs_stats = {};
    int restored_transport_packets = 0;
    int restored_video_parts = 0;
    Stats data_size_stats = {};
    Clock::time_point last_periodic_stats_tp = Clock::now();
    Clock::time_point last_data_rate_sample_tp = Clock::now();
    uint64_t last_udp_packets_sample = 0;
    bool exit_requested = false;
    bool osd_font_reload_pending = false;
};

void pendingOsdFontReload();
void processPendingOsdFontReload(const Ground2Air_Config_Packet& config);

extern GsRuntimeCore s_runtimeCore;
