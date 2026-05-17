#include "gs_runtime_core.h"

#include <optional>
#include <string>
#include <vector>

#include "Log.h"
#include "frame_packets_debug.h"
#include "gs_runtime_state.h"

GsRuntimeCore::GsRuntimeCore(uint16_t gs_device_id_value,
                             TGroundstationConfig& groundstation_config_ref,
                             Ground2Air_Config_Packet& config_packet_ref)
    : gs_device_id(gs_device_id_value),
      config_packet(config_packet_ref),
      groundstation_config(groundstation_config_ref)
{
    init_crc8_table();
    init_fec();
    tx_fec = fec_new(2, 3);

    FecBlockDecoder::Descriptor decoder_descriptor = {};
    decoder_descriptor.coding_k = FEC_K;
    decoder_descriptor.coding_n = 8;
    decoder_descriptor.mtu = AIR2GROUND_MAX_MTU;
    decoder_descriptor.reset_duration = std::chrono::milliseconds(0);
    decoder_descriptor.restart_backjump_blocks = 64;
    decoder_descriptor.max_block_queue_size = 3;
    decoder_descriptor.duplicate_window = 100;
    decoder_descriptor.interface_count = 1;
    rx_decoder_k = decoder_descriptor.coding_k;
    rx_decoder_n = decoder_descriptor.coding_n;
    rx_decoder_mtu = decoder_descriptor.mtu;
    rx_decoder.init(decoder_descriptor);

    FecBlockDecoder::Callbacks callbacks = {};
    callbacks.on_packet_received = [this](uint32_t block_index, uint32_t packet_index, const uint8_t* packet_data, bool old)
    {
        g_framePacketsDebug.onPacketReceived(block_index, packet_index, packet_data, old);
    };
    callbacks.on_packet_restored = [this](uint32_t block_index, uint32_t packet_index, const uint8_t* packet_data)
    {
        g_framePacketsDebug.onPacketRestored(block_index, packet_index, packet_data);
    };
    rx_decoder.setCallbacks(std::move(callbacks));
    resetState(gs_device_id_value);
}

GsRuntimeCore::~GsRuntimeCore()
{
    if (tx_fec)
    {
        fec_free(tx_fec);
        tx_fec = nullptr;
    }
}

void GsRuntimeCore::resetState(uint16_t gs_device_id_value, bool clear_apfpv_state)
{
    gs_device_id = gs_device_id_value;
    assembler = gs::core::VideoFrameAssembler();
    rx_decoder.reset(Clock::now());
    rx_decoder_k = FEC_K;
    rx_decoder_n = 8;
    rx_decoder_mtu = AIR2GROUND_MAX_MTU;
    last_event_kind = gs::core::SessionEventKind::Ignore;
    last_completed_frame.clear();
    next_tx_block_index = 1;
    tx_block_has_first_packet = false;
    tx_first_packet_payload.fill(0);
    transport_packets_seen = 0;
    transport_packets_passed_filter = 0;
    transport_packets_filtered = 0;
    decoded_packets_seen = 0;
    last_decoded_type = 0;
    last_decoded_size = 0;
    last_decoded_air = 0;
    last_decoded_gs = 0;
    last_transport_block = 0;
    last_transport_packet_index = 0;
    last_transport_payload_size = 0;
    last_transport_from = 0;
    last_transport_to = 0;
    last_video_frame_index = 0;
    last_video_part_index = 0;
    last_video_last_part = 0;
    last_video_payload_size = 0;
    last_packet_tp = Clock::now();
    g_framePacketsDebug.off();
    last_ground_stats = {};
    gs_stats = {};
    last_gs_stats = {};
    restored_transport_packets = 0;
    restored_video_parts = 0;
    data_size_stats = {};
    last_periodic_stats_tp = Clock::now();
    last_data_rate_sample_tp = Clock::now();
    last_udp_packets_sample = 0;
    exit_requested = false;
    osd_font_reload_pending = false;
    if (clear_apfpv_state)
    {
        clearApfpvCameraRuntimeState();
    }
    setLinkState(LinkState::None);
}

void GsRuntimeCore::resetPairing(gs::core::ITransport& transport, Clock::time_point now)
{
    session.resetPairing(gs_device_id, transport, now);
    transport.getPacketFilter().set_packet_filtering(0, 0);
}

//===================================================================================
//===================================================================================
// Resets runtime transport/session state after a transport switch or explicit session reset.
void GsRuntimeCore::resetTransportRuntime(gs::core::ITransport& transport, Clock::time_point now)
{
    resetState(gs_device_id);
    resetPairing(transport, now);
}

//===================================================================================
//===================================================================================
// Resets transport/session runtime while preserving the current APFPV camera runtime snapshot.
void GsRuntimeCore::resetTransportRuntimePreserveApfpvState(gs::core::ITransport& transport, Clock::time_point now)
{
    resetState(gs_device_id, false);
    resetPairing(transport, now);
}

//===================================================================================
//===================================================================================
// Marks the current OSD font selection for lazy reload on the render thread.
void pendingOsdFontReload()
{
    s_runtimeCore.osd_font_reload_pending = true;
}

//===================================================================================
//===================================================================================
// Resolves the selected OSD font name without forcing GL texture creation on this thread.
void processPendingOsdFontReload(const Ground2Air_Config_Packet& config)
{
    if (!s_runtimeCore.osd_font_reload_pending)
    {
        return;
    }
    s_runtimeCore.osd_font_reload_pending = false;
    const std::vector<std::string>& font_names = s_OSDFontStorage->osdFontsList();
    const std::optional<std::string> font_name =
        s_flightOSD.findFontNameByCrc(font_names, config.misc.osdFontCRC32);
    if (!font_name.has_value())
    {
        s_flightOSD.setFontName(s_flightOSD.defaultFontName());
        return;
    }
    s_flightOSD.setFontName(*font_name);
}
