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

struct AirStatusState
{
    WIFI_Rate curr_wifi_rate = WIFI_Rate::RATE_B_2M_CCK;
    int wifi_queue_min = 0;
    int wifi_queue_max = 0;
    uint8_t curr_quality = 0;
    uint16_t sd_total_space_gb16 = 0;
    uint16_t sd_free_space_gb16 = 0;
    bool air_record = false;
    bool wifi_ovf = false;
    bool sd_detected = false;
    bool sd_slow = false;
    bool sd_error = false;
    bool is_ov5640 = false;
};

struct PingSnapshot
{
    Clock::duration min = std::chrono::seconds(999);
    Clock::duration max = std::chrono::seconds(0);
    Clock::duration total = std::chrono::seconds(0);
    size_t count = 0;
    Clock::time_point last_received_tp = Clock::now();
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
    AirStatusState& airStatus();
    const AirStatusState& airStatus() const;
    uint8_t currentPingToken() const;
    void onPingSent(Clock::time_point now);
    void onVideoPong(uint8_t pong, Clock::time_point now);
    PingSnapshot consumePingSnapshot();

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
    AirStatusState m_air_status = {};
    PingSnapshot m_ping_snapshot = {};
    uint8_t m_last_sent_ping = 0;
    Clock::time_point m_last_ping_sent_tp = Clock::now();
    uint16_t m_connected_air_device_id = 0;
    bool m_got_config_packet = false;
    bool m_accept_config_packet = false;
};

}
