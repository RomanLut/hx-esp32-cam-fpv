#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <vector>

#include "core/transport_base.h"
#include "fec_block_decoder.h"

//===================================================================================
//===================================================================================
// Implements the Linux APFPV UDP transport with managed-mode Wi-Fi setup and FEC decode.
class LinuxApfpvTransport final : public gs::core::TransportBase
{
public:
    LinuxApfpvTransport();
    ~LinuxApfpvTransport() override;

    bool init(const gs::core::RXDescriptor& rx_descriptor,
              const gs::core::TXDescriptor& tx_descriptor) override;
    void activate() override;
    void process() override;
    void reset_rx_state() override;
    void send(const void* data, size_t size, bool flush) override;
    bool receive(void* data, size_t& size, bool& restoredByFEC) override;
    size_t get_data_rate() const override;
    int get_input_dBm() const override;

private:
    void ensureRxDecoderConfig();
    void syncRxDecoderStats();
    bool startBackend();
    void stopBackend();
    void rxThreadProc();
    bool openSocket();
    void closeSocket();
    void sendTransportPacket(const uint8_t* payload, size_t payload_size, uint8_t packet_index);

    int m_socket_fd = -1;
    sockaddr_in m_peer_addr = {};
    std::mutex m_socket_mutex;

    std::atomic<bool> m_exit_requested = false;
    std::atomic<bool> m_backend_running = false;
    std::thread m_rx_thread;

    FecBlockDecoder m_rx_decoder;
    fec_t* m_tx_fec = nullptr;
    std::array<uint8_t, GROUND2AIR_MAX_MTU> m_first_tx_payload = {};
    bool m_has_first_tx_payload = false;
    uint32_t m_next_tx_block_index = 1;

    size_t m_data_stats_rate = 0;
    size_t m_data_stats_data_accumulated = 0;
    uint64_t m_last_rx_decoded_bytes_total = 0;
    Clock::time_point m_data_stats_last_tp = Clock::now();

    int m_input_dbm = 0;
};
