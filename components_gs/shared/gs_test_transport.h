#pragma once

#include <array>
#include <deque>
#include <vector>

#include "core/transport_base.h"
#include "fec_block_decoder.h"

//===================================================================================
//===================================================================================
// Provides a shared test transport that streams a static JPEG at a paced 30 FPS.
class GSTestTransport : public gs::core::TransportBase
{
public:
    ~GSTestTransport() override;
    bool init(const gs::core::RXDescriptor& rx_descriptor,
              const gs::core::TXDescriptor& tx_descriptor) override;
    void process() override;
    void reset_rx_state() override;
    void send(const void* data, size_t size, bool flush) override;
    bool receive(void* data, size_t& size, bool& restoredByFEC) override;
    size_t get_data_rate() const override;

protected:
    virtual bool loadStaticJpeg();
    virtual void queueStatsOsdPacket(Clock::time_point now);

    std::vector<uint8_t> m_static_jpeg;
    std::deque<std::vector<uint8_t>> m_pending_packets;
    Clock::time_point  m_next_stats_tp       = Clock::time_point::min();
    Clock::duration    m_target_frame_period  = std::chrono::microseconds(33333);
    uint8_t            m_effective_coding_k   = FEC_K;
    uint8_t            m_effective_coding_n   = FEC_N;

private:
    void resetStreamState();
    bool initLoopbackCodec();
    void encodePendingPackets(Clock::time_point now);
    void queueConnectConfigPacket();
    void scheduleDuePackets(Clock::time_point now);
    std::vector<std::vector<uint8_t>> buildFramePackets(uint32_t frame_index) const;
    uint16_t currentGsDeviceId() const;
    size_t maxVideoPayloadSize() const;

    Clock::time_point m_next_frame_tp = Clock::time_point::min();
    Clock::time_point m_next_packet_tp = Clock::time_point::min();
    Clock::duration m_frame_period = std::chrono::microseconds(33333);
    Clock::duration m_packet_period = std::chrono::microseconds(33333);
    uint32_t m_next_frame_index = 1;
    size_t m_pre_sent_for_fill = 0;
    size_t m_data_rate = 0;
    bool m_config_pending = true;
    FecBlockDecoder m_rx_decoder;
    fec_t* m_tx_fec = nullptr;
    uint32_t m_next_transport_block_index = 1;
    uint16_t m_transport_payload_size = AIR2GROUND_MAX_MTU;
};
