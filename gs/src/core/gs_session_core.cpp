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
