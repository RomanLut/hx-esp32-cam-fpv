#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "Clock.h"
#include "core/gs_protocol.h"
#include "core/transport.h"
#include "packets.h"

namespace gs::core
{

enum class SessionPacketType
{
    Ignore,
    Config,
    Video,
    Telemetry,
    OSD
};

struct SessionPacketDecision
{
    SessionPacketType type = SessionPacketType::Ignore;
    protocol::AirPacketInfo packet_info = {};
    bool accepted_connect_config = false;
};

struct TelemetryPacketView
{
    const Air2Ground_Data_Packet* packet = nullptr;
    size_t payload_size = 0;
};

struct VideoPacketView
{
    const Air2Ground_Video_Packet* packet = nullptr;
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;
};

struct OsdPacketView
{
    const Air2Ground_OSD_Packet* packet = nullptr;
    uint16_t osd_data_size = 0;
};

class GsSessionCore
{
public:
    void resetPairing(uint16_t gs_device_id, ITransport& transport, Clock::time_point now);

    bool tryAcceptConnectConfig(const protocol::AirPacketInfo& packet_info,
                                const uint8_t* packet_data,
                                uint16_t gs_device_id,
                                ITransport& transport);

    bool isPacketForSession(const protocol::AirPacketInfo& packet_info, uint16_t gs_device_id) const;
    SessionPacketDecision classifyPacket(const uint8_t* packet_data,
                                         size_t packet_size,
                                         uint16_t gs_device_id,
                                         ITransport& transport);
    bool tryParseVideoPacket(const uint8_t* packet_data,
                             size_t transport_packet_size,
                             size_t packet_size,
                             VideoPacketView& out_view) const;
    bool tryParseTelemetryPacket(const uint8_t* packet_data,
                                 size_t transport_packet_size,
                                 size_t packet_size,
                                 TelemetryPacketView& out_view) const;
    bool tryParseOsdPacket(const uint8_t* packet_data,
                           size_t transport_packet_size,
                           size_t packet_size,
                           OsdPacketView& out_view);

    bool promoteAcceptedConfig(Ground2Air_Config_Packet& config_out);

    std::mutex& configPacketMutex();
    Ground2Air_Config_Packet& configPacket();

    std::mutex& dataPacketMutex();
    Ground2Air_Data_Packet& dataPacket();

    AirStats& lastAirStats();
    const AirStats& lastAirStats() const;

    uint16_t connectedAirDeviceId() const;
    bool gotConfigPacket() const;
    bool acceptConfigPacket() const;

private:
    mutable std::mutex m_state_mutex;
    std::mutex m_config_packet_mutex;
    Ground2Air_Config_Packet m_config_packet = {};

    std::mutex m_data_packet_mutex;
    Ground2Air_Data_Packet m_data_packet = {};

    AirStats m_last_air_stats = {};
    uint16_t m_connected_air_device_id = 0;
    bool m_got_config_packet = false;
    bool m_accept_config_packet = false;
};

}
