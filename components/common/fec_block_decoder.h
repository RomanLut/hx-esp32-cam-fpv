#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "fec.h"
#include "packets.h"

class FecBlockDecoder
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Descriptor
    {
        uint8_t coding_k = FEC_K;
        uint8_t coding_n = FEC_N;
        uint16_t mtu = AIR2GROUND_MAX_MTU;
        std::chrono::milliseconds reset_duration = std::chrono::milliseconds(0);
        uint32_t restart_backjump_blocks = 64;
        size_t max_block_queue_size = 3;
        size_t duplicate_window = 100;
        size_t interface_count = 2;
    };

    struct Stats
    {
        uint32_t last_packet_index = 0;
        uint32_t unique_packet_count = 0;
        uint32_t duplicate_packet_count = 0;
        uint32_t fec_blocks_count = 0;
        uint32_t fec_success_packet_index_sum = 0;
        uint64_t decoded_bytes_total = 0;
    };

    struct DecodedPacket
    {
        std::vector<uint8_t> data;
        bool restored_by_fec = false;
        uint32_t block_index = 0;
        uint8_t packet_index = 0;
    };

    struct Callbacks
    {
        std::function<void(uint32_t, uint32_t, const uint8_t*, bool)> on_packet_received;
        std::function<void(uint32_t, uint32_t, const uint8_t*)> on_packet_restored;
        std::function<void(uint32_t, uint32_t, uint32_t)> on_stream_restart_detected;
        std::function<void(const DecodedPacket&)> on_decoded_packet;
    };

    FecBlockDecoder();
    ~FecBlockDecoder();

    bool init(const Descriptor& descriptor);
    void reset(TimePoint now);

    void setCallbacks(Callbacks callbacks);

    bool pushPacket(const uint8_t* data,
                    size_t size,
                    size_t interface_index,
                    TimePoint now);

    void process(TimePoint now);
    bool receive(void* data, size_t& size, bool& restored_by_fec);

    Stats getStats() const;
    TimePoint lastBlockTime() const;
    TimePoint lastPacketTime() const;

private:
    struct Packet
    {
        bool is_processed = false;
        bool restored_by_fec = false;
        uint32_t index = 0;
        uint16_t size = 0;
        std::vector<uint8_t> data;
    };

    using PacketPtr = std::shared_ptr<Packet>;

    struct Block
    {
        uint32_t index = 0;
        std::vector<PacketPtr> packets;
        std::vector<PacketPtr> fec_packets;
    };

    using BlockPtr = std::shared_ptr<Block>;

    PacketPtr makePacket() const;
    BlockPtr makeBlock() const;

    void emitDecodedPacket(const PacketPtr& packet, uint32_t block_index);
    void pruneReceivedPacketIdsLocked(uint32_t newest_packet_index);

    Descriptor m_descriptor = {};
    fec_t* m_fec = nullptr;
    Callbacks m_callbacks = {};

    mutable std::mutex m_state_mutex;
    std::deque<BlockPtr> m_block_queue;
    std::deque<uint32_t> m_received_packet_ids;
    std::vector<uint32_t> m_last_packet_id_by_interface;
    std::vector<uint32_t> m_last_block_index_by_interface;
    std::vector<const gf*> m_fec_src_packet_ptrs;
    std::vector<gf*> m_fec_dst_packet_ptrs;
    uint32_t m_next_block_index = 0;
    TimePoint m_last_block_tp = {};
    TimePoint m_last_packet_tp = {};
    Stats m_stats = {};

    mutable std::mutex m_ready_mutex;
    std::deque<DecodedPacket> m_ready_packets;
};
