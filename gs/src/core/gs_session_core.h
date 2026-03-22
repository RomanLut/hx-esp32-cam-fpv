#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "Clock.h"
#include "core/gs_protocol.h"
#include "core/transport.h"
#include "packets.h"
#include "stats.h"

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

enum class ControlPacketType
{
    None,
    Connect,
    Config
};

struct ControlPacketView
{
    ControlPacketType type = ControlPacketType::None;
    Ground2Air_Config_Packet config_packet = {};
    Ground2Air_Connect_Packet connect_packet = {};
};

struct TelemetryTxDecision
{
    bool should_flush = false;
    bool should_send = false;
    Ground2Air_Data_Packet packet = {};
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

struct LinkStatusSnapshot
{
    int ping_min_ms = 0;
    int ping_max_ms = 0;
    int ping_avg_ms = 0;
    bool no_ping = true;
};

struct PeriodicStatsSnapshot
{
    size_t sent_count = 0;
    size_t in_tlm_size = 0;
    size_t out_tlm_size = 0;
    size_t total_data = 0;
};

struct FrameStatsState
{
    int lost_frame_count = 0;
    Stats frame_stats;
    Stats frame_parts_stats;
    Stats frame_time_stats;
    Stats frame_quality_stats;
    Stats queue_usage_stats;
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
    ControlPacketView buildControlPacket(uint16_t gs_device_id) const;
    size_t telemetryBufferedSize() const;
    size_t telemetryFreeBytes() const;
    uint8_t* telemetryPayloadWritePtr();
    void appendTelemetryBytes(size_t bytes);
    TelemetryTxDecision buildTelemetryTxDecision(bool got_rc_packet,
                                                 Clock::time_point now,
                                                 uint16_t gs_device_id);

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
    LinkStatusSnapshot consumeLinkStatus(Clock::time_point now);
    void addSentPackets(size_t count);
    void addInboundTelemetryBytes(size_t bytes);
    void addOutboundTelemetryBytes(size_t bytes);
    void addReceivedBytes(size_t bytes);
    PeriodicStatsSnapshot consumePeriodicStats();
    uint8_t consumeDataRateSample();
    void onLostPartialFrame(uint8_t lost_partial_parts, uint8_t queue_usage);
    void onLostWholeFrames(int lost_whole_frames);
    void onCompletedFrame(bool restored_by_fec,
                          uint8_t completed_part_index,
                          uint8_t quality,
                          uint8_t queue_usage,
                          Clock::time_point now);
    FrameStatsState& frameStats();
    const FrameStatsState& frameStats() const;

    uint16_t connectedAirDeviceId() const;
    bool gotConfigPacket() const;
    bool acceptConfigPacket() const;

private:
    mutable std::mutex m_state_mutex;
    mutable std::mutex m_config_packet_mutex;
    Ground2Air_Config_Packet m_config_packet = {};

    mutable std::mutex m_data_packet_mutex;
    Ground2Air_Data_Packet m_data_packet = {};

    AirStats m_last_air_stats = {};
    AirStatusState m_air_status = {};
    PingSnapshot m_ping_snapshot = {};
    PeriodicStatsSnapshot m_periodic_stats = {};
    FrameStatsState m_frame_stats = {};
    size_t m_total_data10 = 0;
    size_t m_telemetry_buffered_size = 0;
    uint8_t m_last_sent_ping = 0;
    Clock::time_point m_last_ping_sent_tp = Clock::now();
    Clock::time_point m_last_frame_completed_tp = Clock::now();
    Clock::time_point m_last_data_sent_tp = Clock::now();
    uint16_t m_connected_air_device_id = 0;
    bool m_got_config_packet = false;
    bool m_accept_config_packet = false;
};

}
