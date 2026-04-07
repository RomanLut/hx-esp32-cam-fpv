#include "core/gs_session_core.h"
#include "ISerialTelemetry.h"
#include "flight_osd.h"
#include "frame_packets_debug.h"
#include "gs_runtime_core.h"
#include "gs_runtime_state.h"
#include "gs_stats.h"

namespace gs::core
{

//===================================================================================
//===================================================================================
// Resets session state and reconfigures the transport packet filter for a fresh pairing.
void GsSessionCore::resetPairing(uint16_t gs_device_id, ITransport& transport, Clock::time_point now)
{
    {
        std::lock_guard<std::mutex> lg(m_state_mutex);
        m_connected_air_device_id = 0;
        m_got_config_packet = false;
        m_accept_config_packet = false;
        m_telemetry_buffered_size = 0;
        m_last_sent_ping = 0;
        m_last_ping_sent_tp = now;
        m_last_data_sent_tp = now;
        m_ping_snapshot = {};
        m_ping_snapshot.last_received_tp = now;
    }

    transport.getPacketFilter().set_packet_header_data(gs_device_id, 0);
    transport.getPacketFilter().set_packet_filtering(0, gs_device_id);
    transport.reset_rx_state();

    (void)now;
}

//===================================================================================
//===================================================================================
// Accepts an initial connect-config packet from the air unit if not already connected,
// stores the air config, and updates the transport packet filter.
bool GsSessionCore::tryAcceptConnectConfig(const protocol::AirPacketInfo& packet_info,
                                          const uint8_t* packet_data,
                                          uint16_t gs_device_id,
                                          ITransport& transport)
{
    {
        std::lock_guard<std::mutex> lg(m_state_mutex);
        if (m_got_config_packet || m_accept_config_packet)
        {
            return false;
        }
    }

    if (!protocol::isConnectConfigPacket(packet_info, Air2Ground_Header::Type::Config, gs_device_id))
    {
        return false;
    }

    const auto* air_config = reinterpret_cast<const Air2Ground_Config_Packet*>(packet_data);
    {
        std::lock_guard<std::mutex> config_lock(m_config_packet_mutex);
        m_config_packet.camera = air_config->camera;
        m_config_packet.dataChannel = air_config->dataChannel;
        m_config_packet.misc = air_config->misc;
    }

    const uint16_t air_device_id = packet_info.header->airDeviceId;
    {
        std::lock_guard<std::mutex> lg(m_state_mutex);
        m_connected_air_device_id = air_device_id;
        m_accept_config_packet = true;
    }

    transport.getPacketFilter().set_packet_header_data(gs_device_id, air_device_id);
    transport.getPacketFilter().set_packet_filtering(air_device_id, gs_device_id);
    return true;
}

//===================================================================================
//===================================================================================
// Returns true if the packet belongs to the current active session.
bool GsSessionCore::isPacketForSession(const protocol::AirPacketInfo& packet_info, uint16_t gs_device_id) const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return m_got_config_packet &&
           protocol::isPacketForSession(packet_info, gs_device_id, m_connected_air_device_id);
}

//===================================================================================
//===================================================================================
// Parses a raw transport packet and classifies it by session packet type.
// Attempts to accept a connect-config if not yet connected.
SessionPacketDecision GsSessionCore::classifyPacket(const uint8_t* packet_data,
                                                    size_t packet_size,
                                                    uint16_t gs_device_id,
                                                    ITransport& transport)
{
    SessionPacketDecision decision;
    if (!protocol::tryParseAirPacket(packet_data, packet_size, decision.packet_info))
    {
        return decision;
    }

    if (!gotConfigPacket())
    {
        decision.accepted_connect_config =
            tryAcceptConnectConfig(decision.packet_info, packet_data, gs_device_id, transport);
        return decision;
    }

    if (!isPacketForSession(decision.packet_info, gs_device_id))
    {
        return decision;
    }

    switch (decision.packet_info.header->type)
    {
    case Air2Ground_Header::Type::Config:
        decision.type = SessionPacketType::Config;
        break;
    case Air2Ground_Header::Type::Video:
        decision.type = SessionPacketType::Video;
        break;
    case Air2Ground_Header::Type::Telemetry:
        decision.type = SessionPacketType::Telemetry;
        break;
    case Air2Ground_Header::Type::OSD:
        decision.type = SessionPacketType::OSD;
        break;
    default:
        decision.type = SessionPacketType::Ignore;
        break;
    }

    return decision;
}

//===================================================================================
//===================================================================================
// Validates and unpacks a video packet from raw data into a VideoPacketView.
bool GsSessionCore::tryParseVideoPacket(const uint8_t* packet_data,
                                        size_t transport_packet_size,
                                        size_t packet_size,
                                        VideoPacketView& out_view) const
{
    out_view = {};

    if (packet_size > transport_packet_size)
    {
        return false;
    }
    if (packet_size < sizeof(Air2Ground_Video_Packet))
    {
        return false;
    }

    const auto* video_packet = reinterpret_cast<const Air2Ground_Video_Packet*>(packet_data);
    if (!protocol::validateFixedHeaderCrc(*video_packet))
    {
        return false;
    }

    out_view.packet = video_packet;
    out_view.payload = packet_data + sizeof(Air2Ground_Video_Packet);
    out_view.payload_size = packet_size - sizeof(Air2Ground_Video_Packet);
    return true;
}

//===================================================================================
//===================================================================================
// Validates and unpacks a telemetry packet from raw data into a TelemetryPacketView.
bool GsSessionCore::tryParseTelemetryPacket(const uint8_t* packet_data,
                                            size_t transport_packet_size,
                                            size_t packet_size,
                                            TelemetryPacketView& out_view) const
{
    out_view = {};

    if (packet_size > transport_packet_size)
    {
        return false;
    }
    if (packet_size < (sizeof(Air2Ground_Data_Packet) + 1))
    {
        return false;
    }

    const auto* telemetry_packet = reinterpret_cast<const Air2Ground_Data_Packet*>(packet_data);
    if (!protocol::validateFixedHeaderCrc(*telemetry_packet))
    {
        return false;
    }

    out_view.packet = telemetry_packet;
    out_view.payload_size = packet_size - sizeof(Air2Ground_Data_Packet);
    return true;
}

//===================================================================================
//===================================================================================
// Validates and unpacks an OSD packet from raw data into an OsdPacketView,
// and updates the cached air stats from the packet.
bool GsSessionCore::tryParseOsdPacket(const uint8_t* packet_data,
                                      size_t transport_packet_size,
                                      size_t packet_size,
                                      OsdPacketView& out_view)
{
    out_view = {};

    if (packet_size > transport_packet_size)
    {
        return false;
    }
    if (packet_size < sizeof(Air2Ground_OSD_Packet))
    {
        return false;
    }

    const auto* osd_packet = reinterpret_cast<const Air2Ground_OSD_Packet*>(packet_data);
    if (!protocol::validateFixedHeaderCrc(*osd_packet))
    {
        return false;
    }

    m_last_air_stats = osd_packet->stats;

    out_view.packet = osd_packet;
    out_view.osd_data_size = osd_packet->size - (sizeof(Air2Ground_OSD_Packet) - 1);
    return true;
}

//===================================================================================
//===================================================================================
// Atomically promotes a pending accepted config to the active session config.
// Returns true and writes the config out if promotion occurred.
bool GsSessionCore::promoteAcceptedConfig(Ground2Air_Config_Packet& config_out)
{
    std::lock_guard<std::mutex> state_lock(m_state_mutex);
    if (!m_accept_config_packet)
    {
        return false;
    }

    m_accept_config_packet = false;
    m_got_config_packet = true;

    std::lock_guard<std::mutex> config_lock(m_config_packet_mutex);
    config_out = m_config_packet;
    return true;
}

//===================================================================================
//===================================================================================
// Promotes a pending accepted config if available, otherwise writes the given config
// back into the session. Returns true if a new config was promoted.
bool GsSessionCore::syncConfigPacket(Ground2Air_Config_Packet& config)
{
    if (promoteAcceptedConfig(config))
    {
        return true;
    }

    std::lock_guard<std::mutex> config_lock(m_config_packet_mutex);
    m_config_packet = config;
    return false;
}

//===================================================================================
//===================================================================================
// Overwrites the stored config packet with the given value.
void GsSessionCore::setConfigPacket(const Ground2Air_Config_Packet& config)
{
    std::lock_guard<std::mutex> config_lock(m_config_packet_mutex);
    m_config_packet = config;
}

//===================================================================================
//===================================================================================
// Builds a control packet to send to the air unit: a connect packet before session
// establishment, or a config packet with the current ping token once connected.
ControlPacketView GsSessionCore::buildControlPacket(uint16_t gs_device_id) const
{
    ControlPacketView view;

    std::lock_guard<std::mutex> state_lock(m_state_mutex);
    if (!m_got_config_packet)
    {
        view.type = ControlPacketType::Connect;
        view.connect_packet = protocol::makeConnectPacket(gs_device_id);
        return view;
    }

    view.type = ControlPacketType::Config;
    {
        std::lock_guard<std::mutex> config_lock(m_config_packet_mutex);
        view.config_packet = m_config_packet;
    }
    protocol::prepareConfigPacket(view.config_packet,
                                  m_last_sent_ping,
                                  m_connected_air_device_id,
                                  gs_device_id);
    return view;
}

//===================================================================================
//===================================================================================
// Returns the number of telemetry bytes currently buffered in the outbound payload.
size_t GsSessionCore::telemetryBufferedSize() const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return m_telemetry_buffered_size;
}

//===================================================================================
//===================================================================================
// Returns the number of free bytes remaining in the outbound telemetry payload buffer.
size_t GsSessionCore::telemetryFreeBytes() const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return GROUND2AIR_DATA_MAX_PAYLOAD_SIZE - m_telemetry_buffered_size;
}

//===================================================================================
//===================================================================================
// Returns a pointer to the next write position in the outbound telemetry payload buffer.
uint8_t* GsSessionCore::telemetryPayloadWritePtr()
{
    std::lock_guard<std::mutex> data_lock(m_data_packet_mutex);
    std::lock_guard<std::mutex> state_lock(m_state_mutex);
    return &m_data_packet.payload[m_telemetry_buffered_size];
}

//===================================================================================
//===================================================================================
// Advances the telemetry buffer write position by the given number of bytes.
void GsSessionCore::appendTelemetryBytes(size_t bytes)
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    m_telemetry_buffered_size += bytes;
}

//===================================================================================
//===================================================================================
// Writes outbound telemetry payload to serial and records the byte count.
void GsSessionCore::dispatchOutboundTelemetry(const uint8_t* payload, size_t payload_size)
{
    if (payload == nullptr || payload_size == 0 || !g_serialTelemetry->isOpen())
    {
        return;
    }
    g_serialTelemetry->write(payload, payload_size);
}

//===================================================================================
//===================================================================================
// Reads incoming serial telemetry bytes, parses MAVLink RC packets to detect RC input,
// updates RC period stats, and flushes the outbound telemetry packet if needed.
void GsSessionCore::processIncomingTelemetry(uint16_t gs_device_id,
                                             ITransport& transport,
                                             std::mutex& gs_stats_mutex,
                                             GSStats& gs_stats)
{
    if (!g_serialTelemetry->isOpen())
    {
        return;
    }

    const int frb = static_cast<int>(telemetryFreeBytes());
    uint8_t* payload_write_ptr = telemetryPayloadWritePtr();
    int n = g_serialTelemetry->read(payload_write_ptr, frb);

    bool gotRCPacket = false;

    if (n > 0)
    {
        uint8_t* dPtr = payload_write_ptr;
        for (int i = 0; i < n; i++)
        {
            m_mavlink_parser_in.processByte(*dPtr++);
            if (m_mavlink_parser_in.gotPacket() &&
                m_mavlink_parser_in.getMessageId() == HX_MAXLINK_RC_CHANNELS_OVERRIDE)
            {
                gotRCPacket = true;

                static Clock::time_point s_last_rc_command = Clock::now();
                Clock::time_point t = Clock::now();
                int dt = std::chrono::duration_cast<std::chrono::milliseconds>(t - s_last_rc_command).count();
                s_last_rc_command = t;
                std::lock_guard<std::mutex> lg(gs_stats_mutex);
                gs_stats.RCPeriodMax = std::max(gs_stats.RCPeriodMax, dt);
            }
        }

        appendTelemetryBytes(n);
    }

    flushTelemetryIfNeeded(gotRCPacket, Clock::now(), gs_device_id, transport);
}

//===================================================================================
//===================================================================================
// Sends the buffered outbound telemetry packet if a flush condition is met:
// buffer full, RC packet received, or the 100 ms drain timeout elapsed.
void GsSessionCore::flushTelemetryIfNeeded(bool got_rc_packet,
                                           Clock::time_point now,
                                           uint16_t gs_device_id,
                                           ITransport& transport)
{
    size_t buffered_size = 0;
    uint16_t connected_air_device_id = 0;
    bool got_config_packet = false;
    {
        std::lock_guard<std::mutex> state_lock(m_state_mutex);
        buffered_size = m_telemetry_buffered_size;
        got_config_packet = m_got_config_packet;
        connected_air_device_id = m_connected_air_device_id;

        bool should_flush =
            (buffered_size == GROUND2AIR_DATA_MAX_PAYLOAD_SIZE) ||
            got_rc_packet ||
            ((buffered_size > 0) && ((now - m_last_data_sent_tp) >= std::chrono::milliseconds(100)));

        if (!should_flush)
        {
            return;
        }

        m_last_data_sent_tp = now;
        m_telemetry_buffered_size = 0;
    }

    if (!got_config_packet)
    {
        return;
    }

    Ground2Air_Data_Packet packet;
    {
        std::lock_guard<std::mutex> data_lock(m_data_packet_mutex);
        packet = m_data_packet;
    }
    protocol::prepareTelemetryPacket(packet, buffered_size, connected_air_device_id, gs_device_id);
    transport.send(&packet, packet.size, true);
    addOutboundTelemetryBytes(packet.size);
    addSentPackets(1);
}

//===================================================================================
//===================================================================================
// Classifies and fully processes one received transport packet, returning a SessionEvent
// describing the result (connect, video, telemetry, OSD, or invalid).
SessionEvent GsSessionCore::processReceivedPacket(const uint8_t* packet_data,
                                                  size_t transport_packet_size,
                                                  uint16_t gs_device_id,
                                                  Clock::time_point now,
                                                  ITransport& transport)
{
    SessionEvent result;

    const SessionPacketDecision decision =
        classifyPacket(packet_data, transport_packet_size, gs_device_id, transport);
    result.packet_info = decision.packet_info;

    if (decision.accepted_connect_config)
    {
        result.kind = SessionEventKind::ConnectAccepted;
        return result;
    }

    if (decision.type == SessionPacketType::Ignore)
    {
        return result;
    }

    switch (decision.type)
    {
    case SessionPacketType::Config:
        result.kind = SessionEventKind::ConfigReceived;
        return result;

    case SessionPacketType::Video:
        if (!tryParseVideoPacket(packet_data,
                                 transport_packet_size,
                                 decision.packet_info.packetSize,
                                 result.video))
        {
            result.kind = SessionEventKind::InvalidVideoPacket;
            return result;
        }
        onVideoPong(result.video.packet->pong, now);
        addReceivedBytes(transport_packet_size);
        result.kind = SessionEventKind::VideoPacket;
        return result;

    case SessionPacketType::Telemetry:
        if (!tryParseTelemetryPacket(packet_data,
                                     transport_packet_size,
                                     decision.packet_info.packetSize,
                                     result.telemetry))
        {
            result.kind = SessionEventKind::InvalidTelemetryPacket;
            return result;
        }
        addReceivedBytes(transport_packet_size);
        {
            const uint8_t* payload = reinterpret_cast<const uint8_t*>(result.telemetry.packet) + sizeof(Air2Ground_Data_Packet);
            dispatchOutboundTelemetry(payload, result.telemetry.payload_size);
            addInboundTelemetryBytes(result.telemetry.payload_size);
        }
        return result;

    case SessionPacketType::OSD:
        if (!tryParseOsdPacket(packet_data,
                               transport_packet_size,
                               decision.packet_info.packetSize,
                               result.osd))
        {
            result.kind = SessionEventKind::InvalidOsdPacket;
            return result;
        }
        if ((result.osd.osd_data_size < 2) || (result.osd.osd_data_size > MAX_OSD_PAYLOAD_SIZE))
        {
            result.kind = SessionEventKind::InvalidOsdPacket;
            return result;
        }
        addReceivedBytes(transport_packet_size);
        syncAirStatusGlobals();
        if (!g_framePacketsDebug.isOn())
        {
            s_flightOSD.update(&result.osd.packet->osd_enc_start, result.osd.osd_data_size);
        }
        s_last_stats_packet_tp = Clock::now();
        return result;

    case SessionPacketType::Ignore:
    default:
        result.kind = SessionEventKind::UnsupportedPacket;
        return result;
    }
}

//===================================================================================
//===================================================================================
// Returns a thread-safe copy of the current config packet.
Ground2Air_Config_Packet GsSessionCore::copyConfigPacket() const
{
    std::lock_guard<std::mutex> config_lock(m_config_packet_mutex);
    return m_config_packet;
}


//===================================================================================
//===================================================================================
// Returns a mutable reference to the last received air stats.
AirStats& GsSessionCore::lastAirStats()
{
    return m_last_air_stats;
}

//===================================================================================
//===================================================================================
// Returns a const reference to the last received air stats.
const AirStats& GsSessionCore::lastAirStats() const
{
    return m_last_air_stats;
}

//===================================================================================
//===================================================================================
// Returns the current ping token that will be echoed back in the next pong.
uint8_t GsSessionCore::currentPingToken() const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return m_last_sent_ping;
}

//===================================================================================
//===================================================================================
// Records the timestamp at which the last ping was sent.
void GsSessionCore::onPingSent(Clock::time_point now)
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    m_last_ping_sent_tp = now;
}

//===================================================================================
//===================================================================================
// Processes a pong token received from the air unit, updates the ping snapshot
// with the round-trip time, and advances the ping token.
void GsSessionCore::onVideoPong(uint8_t pong, Clock::time_point now)
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    if (pong != m_last_sent_ping)
    {
        return;
    }

    m_last_sent_ping++;
    m_ping_snapshot.last_received_tp = now;
    const auto ping = (now - m_last_ping_sent_tp) / 2;
    m_ping_snapshot.min = std::min(m_ping_snapshot.min, ping);
    m_ping_snapshot.max = std::max(m_ping_snapshot.max, ping);
    m_ping_snapshot.total += ping;
    m_ping_snapshot.count++;
}

//===================================================================================
//===================================================================================
// Atomically returns and resets the accumulated ping snapshot.
PingSnapshot GsSessionCore::consumePingSnapshot()
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    PingSnapshot snapshot = m_ping_snapshot;
    m_ping_snapshot = {};
    m_ping_snapshot.last_received_tp = snapshot.last_received_tp;
    return snapshot;
}

//===================================================================================
//===================================================================================
// Computes and returns a link status snapshot with min/max/avg ping and a no-ping flag.
LinkStatusSnapshot GsSessionCore::consumeLinkStatus(Clock::time_point now)
{
    const PingSnapshot ping_snapshot = consumePingSnapshot();

    LinkStatusSnapshot snapshot;
    snapshot.ping_min_ms = std::chrono::duration_cast<std::chrono::milliseconds>(ping_snapshot.min).count();
    snapshot.ping_max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(ping_snapshot.max).count();
    snapshot.ping_avg_ms = ping_snapshot.count > 0
                               ? std::chrono::duration_cast<std::chrono::milliseconds>(ping_snapshot.total).count() / ping_snapshot.count
                               : 0;
    snapshot.no_ping = (now - ping_snapshot.last_received_tp) >= std::chrono::milliseconds(2000);
    return snapshot;
}

//===================================================================================
//===================================================================================
// Adds to the count of packets sent during the current stats period.
void GsSessionCore::addSentPackets(size_t count)
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    m_periodic_stats.sent_count += count;
}

//===================================================================================
//===================================================================================
// Adds to the count of inbound (air->gs) telemetry bytes for the current stats period.
void GsSessionCore::addInboundTelemetryBytes(size_t bytes)
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    m_periodic_stats.in_tlm_size += bytes;
}

//===================================================================================
//===================================================================================
// Adds to the count of outbound (gs->air) telemetry bytes for the current stats period.
void GsSessionCore::addOutboundTelemetryBytes(size_t bytes)
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    m_periodic_stats.out_tlm_size += bytes;
}

//===================================================================================
//===================================================================================
// Adds to the total received bytes counters for the current stats period.
void GsSessionCore::addReceivedBytes(size_t bytes)
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    m_periodic_stats.total_data += bytes;
    m_total_data10 += bytes;
}

//===================================================================================
//===================================================================================
// Atomically returns and resets the periodic stats snapshot.
PeriodicStatsSnapshot GsSessionCore::consumePeriodicStats()
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    PeriodicStatsSnapshot snapshot = m_periodic_stats;
    m_periodic_stats = {};
    return snapshot;
}

//===================================================================================
//===================================================================================
// Returns the data rate in KB accumulated since the last call, clamped to 255, and resets the counter.
uint8_t GsSessionCore::consumeDataRateSample()
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    size_t sample = m_total_data10 / 1024;
    if (sample > 255)
    {
        sample = 255;
    }
    m_total_data10 = 0;
    return static_cast<uint8_t>(sample);
}

//===================================================================================
//===================================================================================
// Records a partially lost frame (some parts missing) into frame stats.
void GsSessionCore::onLostPartialFrame(uint8_t lost_partial_parts, uint8_t queue_usage)
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    m_frame_stats.lost_frame_count++;
    m_frame_stats.frame_stats.add(0);
    m_frame_stats.frame_time_stats.add(0);
    m_frame_stats.frame_quality_stats.add(0);
    m_frame_stats.frame_parts_stats.add(lost_partial_parts);
    m_frame_stats.queue_usage_stats.add(queue_usage);
}

//===================================================================================
//===================================================================================
// Records one or more wholly lost frames into frame stats.
void GsSessionCore::onLostWholeFrames(int lost_whole_frames)
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    m_frame_stats.lost_frame_count += lost_whole_frames;
    m_frame_stats.frame_stats.addMultiple(0, lost_whole_frames);
    m_frame_stats.frame_time_stats.addMultiple(0, lost_whole_frames);
    m_frame_stats.frame_quality_stats.addMultiple(0, lost_whole_frames);
    m_frame_stats.frame_parts_stats.addMultiple(0, lost_whole_frames);
    m_frame_stats.queue_usage_stats.addMultiple(0, lost_whole_frames);
}

//===================================================================================
//===================================================================================
// Records a successfully completed frame into frame stats, including FEC recovery,
// part index, quality, queue usage, and inter-frame timing.
void GsSessionCore::onCompletedFrame(bool restored_by_fec,
                                     uint8_t completed_part_index,
                                     uint8_t quality,
                                     uint8_t queue_usage,
                                     Clock::time_point now)
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    if (restored_by_fec)
    {
        m_periodic_stats.restored_completed_frames++;
    }
    else
    {
        m_periodic_stats.received_completed_frames++;
    }

    m_frame_stats.frame_stats.add(restored_by_fec ? 2 : 4);
    m_frame_stats.frame_parts_stats.add(completed_part_index);
    m_frame_stats.frame_quality_stats.add(quality);
    m_frame_stats.queue_usage_stats.add(queue_usage);

    auto frame_period = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_frame_completed_tp).count();
    if (frame_period > 100)
    {
        frame_period = 100;
    }
    m_frame_stats.frame_time_stats.add(static_cast<uint8_t>(frame_period));
    m_last_frame_completed_tp = now;
}

//===================================================================================
//===================================================================================
// Returns a thread-safe copy of the current frame stats state.
FrameStatsState GsSessionCore::copyFrameStats() const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return m_frame_stats;
}

//===================================================================================
//===================================================================================
// Returns and resets the count of lost frames accumulated since the last call.
int GsSessionCore::consumeLostFrameCount()
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    const int lost = m_frame_stats.lost_frame_count;
    m_frame_stats.lost_frame_count = 0;
    return lost;
}

//===================================================================================
//===================================================================================
// Returns a mutable reference to the frame stats state.
FrameStatsState& GsSessionCore::frameStats()
{
    return m_frame_stats;
}

//===================================================================================
//===================================================================================
// Returns a const reference to the frame stats state.
const FrameStatsState& GsSessionCore::frameStats() const
{
    return m_frame_stats;
}

//===================================================================================
//===================================================================================
// Returns the device ID of the currently connected air unit.
uint16_t GsSessionCore::connectedAirDeviceId() const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return m_connected_air_device_id;
}

//===================================================================================
//===================================================================================
// Returns true if a config packet has been received and the session is established.
bool GsSessionCore::gotConfigPacket() const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return m_got_config_packet;
}

//===================================================================================
//===================================================================================
// Returns true if a config packet has been accepted but not yet promoted to active.
bool GsSessionCore::acceptConfigPacket() const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return m_accept_config_packet;
}

}
