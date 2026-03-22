#include "core/gs_session_core.h"

namespace gs::core
{

void GsSessionCore::resetPairing(uint16_t gs_device_id, ITransport& transport, Clock::time_point now)
{
    {
        std::lock_guard<std::mutex> lg(m_state_mutex);
        m_connected_air_device_id = 0;
        m_got_config_packet = false;
        m_accept_config_packet = false;
    }

    transport.getPacketFilter().set_packet_header_data(gs_device_id, 0);
    transport.getPacketFilter().set_packet_filtering(0, gs_device_id);
    transport.reset_rx_state();

    (void)now;
}

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

bool GsSessionCore::isPacketForSession(const protocol::AirPacketInfo& packet_info, uint16_t gs_device_id) const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return m_got_config_packet &&
           protocol::isPacketForSession(packet_info, gs_device_id, m_connected_air_device_id);
}

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
    m_air_status.curr_wifi_rate = static_cast<WIFI_Rate>(osd_packet->stats.curr_wifi_rate);
    m_air_status.curr_quality = osd_packet->stats.curr_quality;
    m_air_status.wifi_queue_min = osd_packet->stats.wifi_queue_min;
    m_air_status.wifi_queue_max = osd_packet->stats.wifi_queue_max;
    m_air_status.sd_total_space_gb16 = osd_packet->stats.SDTotalSpaceGB16;
    m_air_status.sd_free_space_gb16 = osd_packet->stats.SDFreeSpaceGB16;
    m_air_status.air_record = osd_packet->stats.air_record_state != 0;
    m_air_status.wifi_ovf = osd_packet->stats.wifi_ovf != 0;
    m_air_status.sd_detected = osd_packet->stats.SDDetected != 0;
    m_air_status.sd_error = osd_packet->stats.SDError != 0;
    m_air_status.sd_slow = osd_packet->stats.SDSlow != 0;
    m_air_status.is_ov5640 = osd_packet->stats.isOV5640 != 0;

    out_view.packet = osd_packet;
    out_view.osd_data_size = osd_packet->size - (sizeof(Air2Ground_OSD_Packet) - 1);
    return true;
}

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

std::mutex& GsSessionCore::configPacketMutex()
{
    return m_config_packet_mutex;
}

Ground2Air_Config_Packet& GsSessionCore::configPacket()
{
    return m_config_packet;
}

std::mutex& GsSessionCore::dataPacketMutex()
{
    return m_data_packet_mutex;
}

Ground2Air_Data_Packet& GsSessionCore::dataPacket()
{
    return m_data_packet;
}

AirStats& GsSessionCore::lastAirStats()
{
    return m_last_air_stats;
}

const AirStats& GsSessionCore::lastAirStats() const
{
    return m_last_air_stats;
}

AirStatusState& GsSessionCore::airStatus()
{
    return m_air_status;
}

const AirStatusState& GsSessionCore::airStatus() const
{
    return m_air_status;
}

uint16_t GsSessionCore::connectedAirDeviceId() const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return m_connected_air_device_id;
}

bool GsSessionCore::gotConfigPacket() const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return m_got_config_packet;
}

bool GsSessionCore::acceptConfigPacket() const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return m_accept_config_packet;
}

}
