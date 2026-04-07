#pragma once

#include <deque>
#include <vector>

#include "core/transport_base.h"

//===================================================================================
//===================================================================================
// Provides a shared test transport that streams a static JPEG at a paced 30 FPS.
class GSTestTransport final : public gs::core::TransportBase
{
public:
    bool init(const gs::core::RXDescriptor& rx_descriptor,
              const gs::core::TXDescriptor& tx_descriptor) override;
    void process() override;
    void reset_rx_state() override;
    void send(const void* data, size_t size, bool flush) override;
    bool receive(void* data, size_t& size, bool& restoredByFEC) override;
    size_t get_data_rate() const override;

private:
    void resetStreamState();
    bool loadStaticJpeg();
    void queueConnectConfigPacket();
    void scheduleDuePackets(Clock::time_point now);
    std::vector<std::vector<uint8_t>> buildFramePackets(uint32_t frame_index) const;
    uint16_t currentGsDeviceId() const;
    size_t maxVideoPayloadSize() const;

    std::vector<uint8_t> m_static_jpeg;
    std::deque<std::vector<uint8_t>> m_ready_packets;
    Clock::time_point m_next_frame_tp = Clock::time_point::min();
    Clock::time_point m_next_packet_tp = Clock::time_point::min();
    Clock::duration m_frame_period = std::chrono::microseconds(33333);
    Clock::duration m_packet_period = std::chrono::microseconds(33333);
    uint32_t m_next_frame_index = 1;
    size_t m_data_rate = 0;
    bool m_config_pending = true;
};
