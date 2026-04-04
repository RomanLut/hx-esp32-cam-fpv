#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "Clock.h"
#include "core/gs_protocol.h"
#include "core/transport.h"
#include "packets.h"
#include "stats.h"

class ISerialTelemetry;
class HXMavlinkParser;
struct GSStats;

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

enum class SessionEventKind
{
    Ignore,
    ConnectAccepted,
    ConfigReceived,
    VideoPacket,
    TelemetryPayload,
    OsdUpdate,
    InvalidVideoPacket,
    InvalidTelemetryPacket,
    InvalidOsdPacket,
    UnsupportedPacket
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
    int received_completed_frames = 0;
    int restored_completed_frames = 0;
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

struct SessionEvent
{
    SessionEventKind kind = SessionEventKind::Ignore;
    protocol::AirPacketInfo packet_info = {};
    VideoPacketView video = {};
    TelemetryPacketView telemetry = {};
    OsdPacketView osd = {};
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
    bool syncConfigPacket(Ground2Air_Config_Packet& config);
    void setConfigPacket(const Ground2Air_Config_Packet& config);
    ControlPacketView buildControlPacket(uint16_t gs_device_id) const;
    size_t telemetryBufferedSize() const;
    size_t telemetryFreeBytes() const;
    uint8_t* telemetryPayloadWritePtr();
    void appendTelemetryBytes(size_t bytes);
    void processIncomingTelemetry(ISerialTelemetry& serial,
                                  HXMavlinkParser& mavlink_parser,
                                  uint16_t gs_device_id,
                                  ITransport& transport,
                                  std::mutex& gs_stats_mutex,
                                  GSStats& gs_stats);
    void flushTelemetryIfNeeded(bool got_rc_packet,
                               Clock::time_point now,
                               uint16_t gs_device_id,
                               ITransport& transport);
    SessionEvent processReceivedPacket(const uint8_t* packet_data,
                                       size_t transport_packet_size,
                                       uint16_t gs_device_id,
                                       Clock::time_point now,
                                       ITransport& transport);

    Ground2Air_Config_Packet copyConfigPacket() const;

    std::mutex& dataPacketMutex();
    Ground2Air_Data_Packet& dataPacket();

    AirStats& lastAirStats();
    const AirStats& lastAirStats() const;
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
    FrameStatsState copyFrameStats() const;
    int consumeLostFrameCount();
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
