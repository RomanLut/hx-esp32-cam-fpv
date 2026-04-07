#include "fec_block_decoder.h"

#include <algorithm>
#include <cstring>
#include <limits>

//===================================================================================
//===================================================================================
// Default constructor.
FecBlockDecoder::FecBlockDecoder() = default;

//===================================================================================
//===================================================================================
// Destructor. Frees the FEC codec if it was allocated.
FecBlockDecoder::~FecBlockDecoder()
{
    if (m_fec)
    {
        fec_free(m_fec);
        m_fec = nullptr;
    }
}

//===================================================================================
//===================================================================================
// Initializes the decoder with the given descriptor, allocating the FEC codec
// and resetting all internal state. Returns false if parameters are invalid.
bool FecBlockDecoder::init(const Descriptor& descriptor)
{
    if (descriptor.coding_k == 0 ||
        descriptor.coding_n < descriptor.coding_k ||
        descriptor.coding_n > 32 ||
        descriptor.mtu == 0 ||
        descriptor.mtu > AIR2GROUND_MAX_MTU)
    {
        return false;
    }

    std::lock_guard<std::mutex> lg(m_state_mutex);

    if (m_fec)
    {
        fec_free(m_fec);
        m_fec = nullptr;
    }

    m_fec = fec_new(descriptor.coding_k, descriptor.coding_n);
    if (!m_fec)
    {
        return false;
    }

    m_descriptor = descriptor;
    m_last_packet_id_by_interface.assign(std::max<size_t>(descriptor.interface_count, 1), 0u);
    m_last_block_index_by_interface.assign(std::max<size_t>(descriptor.interface_count, 1), 0u);
    m_fec_src_packet_ptrs.assign(descriptor.coding_k, nullptr);
    m_fec_dst_packet_ptrs.assign(descriptor.coding_k, nullptr);
    m_block_queue.clear();
    m_received_packet_ids.clear();
    m_stats = {};
    m_next_block_index = 0;
    m_last_block_tp = TimePoint();
    m_last_packet_tp = TimePoint();

    {
        std::lock_guard<std::mutex> ready_lg(m_ready_mutex);
        m_ready_packets.clear();
    }

    return true;
}

//===================================================================================
//===================================================================================
// Clears all queued blocks and received packet history, resetting the decoder
// to a clean state as of the given timestamp.
void FecBlockDecoder::reset(TimePoint now)
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    m_block_queue.clear();
    m_received_packet_ids.clear();
    std::fill(m_last_packet_id_by_interface.begin(), m_last_packet_id_by_interface.end(), 0u);
    std::fill(m_last_block_index_by_interface.begin(), m_last_block_index_by_interface.end(), 0u);
    m_next_block_index = 0;
    m_last_block_tp = now;
    m_last_packet_tp = now;
    m_stats = {};

    {
        std::lock_guard<std::mutex> ready_lg(m_ready_mutex);
        m_ready_packets.clear();
    }
}

//===================================================================================
//===================================================================================
// Replaces the current callback set used to notify callers of decoded and restored packets.
void FecBlockDecoder::setCallbacks(Callbacks callbacks)
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    m_callbacks = std::move(callbacks);
}

//===================================================================================
//===================================================================================
// Accepts an incoming raw packet from the given interface, deduplicates it,
// inserts it into the appropriate block queue slot, and detects stream restarts.
// Returns false if the packet is malformed or out of range.
bool FecBlockDecoder::pushPacket(const uint8_t* data,
                                 size_t size,
                                 size_t interface_index,
                                 TimePoint now)
{
    if (!data || size < sizeof(Packet_Header))
    {
        return false;
    }

    const Packet_Header& header = *reinterpret_cast<const Packet_Header*>(data);
    const uint32_t block_index = header.block_index;
    const uint32_t packet_index = header.packet_index;
    const uint32_t packet_order_index = block_index * m_descriptor.coding_n + packet_index;

    std::function<void(uint32_t, uint32_t, const uint8_t*, bool)> on_packet_received;
    std::function<void(uint32_t, uint32_t, uint32_t)> on_stream_restart_detected;
    bool packet_received_old = false;
    bool signal_restart = false;
    uint32_t restart_from = 0;
    uint32_t restart_to = 0;
    uint32_t restart_delta = 0;

    {
        std::lock_guard<std::mutex> lg(m_state_mutex);

        if (interface_index >= m_last_packet_id_by_interface.size())
        {
            const size_t new_size = interface_index + 1;
            m_last_packet_id_by_interface.resize(new_size, 0u);
            m_last_block_index_by_interface.resize(new_size, 0u);
        }

        m_stats.last_packet_index = packet_order_index;
        m_last_packet_id_by_interface[interface_index] = packet_order_index;

        auto packet_id_it = std::lower_bound(m_received_packet_ids.begin(), m_received_packet_ids.end(), packet_order_index);
        if (packet_id_it == m_received_packet_ids.end() || *packet_id_it != packet_order_index)
        {
            m_received_packet_ids.insert(packet_id_it, packet_order_index);
            m_stats.unique_packet_count++;
        }
        else
        {
            m_stats.duplicate_packet_count++;
        }
        pruneReceivedPacketIdsLocked(packet_order_index);

        if (packet_index >= m_descriptor.coding_n)
        {
            return false;
        }

        m_last_block_index_by_interface[interface_index] = block_index;
        packet_received_old = block_index < m_next_block_index;
        on_packet_received = m_callbacks.on_packet_received;

        bool clear_received_packet_ids = false;
        if (block_index < m_next_block_index)
        {
            const uint32_t backjump = m_next_block_index - block_index;
            if (backjump >= m_descriptor.restart_backjump_blocks)
            {
                signal_restart = true;
                restart_from = m_next_block_index;
                restart_to = block_index;
                restart_delta = backjump;

                m_block_queue.clear();
                std::fill(m_last_block_index_by_interface.begin(), m_last_block_index_by_interface.end(), block_index);
                m_next_block_index = block_index;
                m_last_block_tp = now;
                m_last_packet_tp = now;

                {
                    std::lock_guard<std::mutex> ready_lg(m_ready_mutex);
                    m_ready_packets.clear();
                }

                clear_received_packet_ids = true;
            }
            else
            {
                return true;
            }
        }

        auto block_it = std::lower_bound(
            m_block_queue.begin(),
            m_block_queue.end(),
            block_index,
            [](const BlockPtr& block, uint32_t index) { return block->index < index; });
        BlockPtr block;
        if (block_it != m_block_queue.end() && (*block_it)->index == block_index)
        {
            block = *block_it;
        }
        else
        {
            block = makeBlock();
            block->index = block_index;
            m_block_queue.insert(block_it, block);
        }

        PacketPtr packet = makePacket();
        packet->index = packet_index;
        packet->size = header.size;
        packet->data.resize(header.size);
        std::memcpy(packet->data.data(), data + sizeof(Packet_Header), header.size);

        if (packet_index >= m_descriptor.coding_k)
        {
            auto iter = std::lower_bound(
                block->fec_packets.begin(),
                block->fec_packets.end(),
                packet_index,
                [](const PacketPtr& value, uint32_t index) { return value->index < index; });
            if (iter != block->fec_packets.end() && (*iter)->index == packet_index)
            {
                return true;
            }
            block->fec_packets.insert(iter, packet);
        }
        else
        {
            auto iter = std::lower_bound(
                block->packets.begin(),
                block->packets.end(),
                packet_index,
                [](const PacketPtr& value, uint32_t index) { return value->index < index; });
            if (iter != block->packets.end() && (*iter)->index == packet_index)
            {
                return true;
            }
            block->packets.insert(iter, packet);
        }

        if (clear_received_packet_ids)
        {
            m_received_packet_ids.clear();
            std::fill(m_last_packet_id_by_interface.begin(), m_last_packet_id_by_interface.end(), 0u);
            m_last_packet_id_by_interface[interface_index] = packet_order_index;
        }

        on_stream_restart_detected = m_callbacks.on_stream_restart_detected;
    }

    if (on_packet_received)
    {
        on_packet_received(block_index, packet_index, data + sizeof(Packet_Header), packet_received_old);
    }
    if (signal_restart && on_stream_restart_detected)
    {
        on_stream_restart_detected(restart_from, restart_to, restart_delta);
    }

    return true;
}

//===================================================================================
//===================================================================================
// Processes the block queue: emits complete blocks directly, attempts FEC recovery
// on incomplete blocks that have enough redundancy packets, and skips blocks
// that all interfaces have moved past.
void FecBlockDecoder::process(TimePoint now)
{
    std::unique_lock<std::mutex> lg(m_state_mutex);

    if (m_descriptor.reset_duration.count() > 0 &&
        now - m_last_packet_tp > m_descriptor.reset_duration)
    {
        m_next_block_index = 0;
    }

    while (!m_block_queue.empty())
    {
        BlockPtr block = m_block_queue.front();

        for (size_t i = 0; i < block->packets.size(); i++)
        {
            const PacketPtr& packet = block->packets[i];
            if (packet->index != i)
            {
                break;
            }
            if (!packet->is_processed)
            {
                packet->restored_by_fec = false;
                emitDecodedPacket(packet, block->index);
                m_last_packet_tp = now;
                packet->is_processed = true;
            }
        }

        if (block->packets.size() >= m_descriptor.coding_k)
        {
            m_stats.fec_blocks_count++;
            m_last_block_tp = now;
            m_next_block_index = block->index + 1;
            m_block_queue.pop_front();
            continue;
        }

        if (block->packets.size() + block->fec_packets.size() >= m_descriptor.coding_k)
        {
            m_stats.fec_blocks_count++;

            if (!block->fec_packets.empty())
            {
                auto max_fec_packet = std::max_element(
                    block->fec_packets.begin(),
                    block->fec_packets.end(),
                    [](const PacketPtr& a, const PacketPtr& b) { return a->index < b->index; });
                m_stats.fec_success_packet_index_sum += (*max_fec_packet)->index;
            }

            std::array<unsigned, 32> indices = {};
            size_t primary_index = 0;
            size_t used_fec_index = 0;
            const size_t block_packet_size = block->fec_packets.empty() ? 0 : block->fec_packets[0]->size;

            for (size_t i = 0; i < m_descriptor.coding_k; i++)
            {
                if (primary_index < block->packets.size() && i == block->packets[primary_index]->index)
                {
                    m_fec_src_packet_ptrs[i] = block->packets[primary_index]->data.data();
                    indices[i] = block->packets[primary_index]->index;
                    primary_index++;
                }
                else
                {
                    m_fec_src_packet_ptrs[i] = block->fec_packets[used_fec_index]->data.data();
                    indices[i] = block->fec_packets[used_fec_index]->index;
                    used_fec_index++;
                }
            }

            size_t fec_index = 0;
            for (size_t i = 0; i < m_descriptor.coding_k; i++)
            {
                if (i >= block->packets.size() || i != block->packets[i]->index)
                {
                    PacketPtr packet = makePacket();
                    packet->data.resize(block_packet_size);
                    packet->size = static_cast<uint16_t>(block_packet_size);
                    packet->index = static_cast<uint32_t>(i);
                    block->packets.insert(block->packets.begin() + i, packet);
                    m_fec_dst_packet_ptrs[fec_index++] = packet->data.data();
                }
            }

            lg.unlock();
            fec_decode(m_fec,
                       m_fec_src_packet_ptrs.data(),
                       m_fec_dst_packet_ptrs.data(),
                       indices.data(),
                       block_packet_size);
            lg.lock();

            const auto on_packet_restored = m_callbacks.on_packet_restored;
            for (size_t i = 0; i < block->packets.size(); i++)
            {
                const PacketPtr& packet = block->packets[i];
                if (!packet->is_processed)
                {
                    if (on_packet_restored)
                    {
                        on_packet_restored(block->index, static_cast<uint32_t>(i), packet->data.data());
                    }
                    packet->restored_by_fec = true;
                    emitDecodedPacket(packet, block->index);
                    m_last_packet_tp = now;
                    packet->is_processed = true;
                }
            }

            m_last_block_tp = now;
            m_next_block_index = block->index + 1;
            m_block_queue.pop_front();
            continue;
        }

        uint32_t earliest_block_index = std::numeric_limits<uint32_t>::max();
        for (uint32_t index : m_last_block_index_by_interface)
        {
            earliest_block_index = std::min(earliest_block_index, index);
        }

        bool skipped_blocks = false;
        while ((!m_block_queue.empty() && m_block_queue.front()->index < earliest_block_index) ||
               m_block_queue.size() > m_descriptor.max_block_queue_size)
        {
            BlockPtr skipped_block = m_block_queue.front();
            m_next_block_index = skipped_block->index + 1;
            m_block_queue.pop_front();
            skipped_blocks = true;
        }

        if (!skipped_blocks)
        {
            break;
        }
    }
}

//===================================================================================
//===================================================================================
// Dequeues the next decoded packet into the caller's buffer.
// Returns false if no packet is available.
bool FecBlockDecoder::receive(void* data, size_t& size, bool& restored_by_fec)
{
    std::lock_guard<std::mutex> lg(m_ready_mutex);
    if (m_ready_packets.empty())
    {
        return false;
    }

    DecodedPacket packet = std::move(m_ready_packets.front());
    m_ready_packets.pop_front();

    size = packet.data.size();
    if (size > 0)
    {
        std::memcpy(data, packet.data.data(), size);
    }
    restored_by_fec = packet.restored_by_fec;
    return true;
}

//===================================================================================
//===================================================================================
// Returns a snapshot of the current decoder statistics.
FecBlockDecoder::Stats FecBlockDecoder::getStats() const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return m_stats;
}

//===================================================================================
//===================================================================================
// Returns the timestamp of the last successfully completed FEC block.
FecBlockDecoder::TimePoint FecBlockDecoder::lastBlockTime() const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return m_last_block_tp;
}

//===================================================================================
//===================================================================================
// Returns the timestamp of the last packet that produced a decoded output.
FecBlockDecoder::TimePoint FecBlockDecoder::lastPacketTime() const
{
    std::lock_guard<std::mutex> lg(m_state_mutex);
    return m_last_packet_tp;
}

//===================================================================================
//===================================================================================
// Allocates a new Packet with data storage pre-reserved to the configured MTU.
FecBlockDecoder::PacketPtr FecBlockDecoder::makePacket() const
{
    PacketPtr packet = std::make_shared<Packet>();
    packet->data.reserve(m_descriptor.mtu);
    return packet;
}

//===================================================================================
//===================================================================================
// Allocates a new Block with packet vectors pre-reserved to the configured k and n counts.
FecBlockDecoder::BlockPtr FecBlockDecoder::makeBlock() const
{
    BlockPtr block = std::make_shared<Block>();
    block->packets.reserve(m_descriptor.coding_k);
    block->fec_packets.reserve(m_descriptor.coding_n - m_descriptor.coding_k);
    return block;
}

//===================================================================================
//===================================================================================
// Copies a decoded packet into the ready queue and fires the on_decoded_packet callback.
void FecBlockDecoder::emitDecodedPacket(const PacketPtr& packet, uint32_t block_index)
{
    DecodedPacket decoded = {};
    decoded.data = packet->data;
    decoded.restored_by_fec = packet->restored_by_fec;
    decoded.block_index = block_index;
    decoded.packet_index = static_cast<uint8_t>(packet->index);

    m_stats.decoded_bytes_total += decoded.data.size();

    {
        std::lock_guard<std::mutex> lg(m_ready_mutex);
        m_ready_packets.push_back(decoded);
    }

    if (m_callbacks.on_decoded_packet)
    {
        m_callbacks.on_decoded_packet(decoded);
    }
}

//===================================================================================
//===================================================================================
// Removes packet IDs from the duplicate-detection window that are older than
// the configured duplicate_window size. Must be called with m_state_mutex held.
void FecBlockDecoder::pruneReceivedPacketIdsLocked(uint32_t newest_packet_index)
{
    const uint32_t last_seen = *std::max_element(m_last_packet_id_by_interface.begin(), m_last_packet_id_by_interface.end());
    const uint32_t too_old_ids = last_seen > m_descriptor.duplicate_window
        ? last_seen - static_cast<uint32_t>(m_descriptor.duplicate_window)
        : 0u;
    auto it = std::lower_bound(m_received_packet_ids.begin(), m_received_packet_ids.end(), too_old_ids);
    m_received_packet_ids.erase(m_received_packet_ids.begin(), it);
    (void)newest_packet_index;
}
