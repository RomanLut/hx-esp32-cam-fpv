#pragma once

#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <functional>
#include "core/transport.h"
#include "fec_block_decoder.h"

#define MIN_TX_POWER 5
#define DEFAULT_TX_POWER 45
#define MAX_TX_POWER 63


struct fec_t;

//===================================================================================
//===================================================================================
class LinuxRawBroadcastTransport : public gs::core::ITransport
{
public:
    LinuxRawBroadcastTransport();
    ~LinuxRawBroadcastTransport();

    using TX_Descriptor = gs::core::TXDescriptor;
    using RX_Descriptor = gs::core::RXDescriptor;

    bool init(const RX_Descriptor& rx_descriptor, const TX_Descriptor& tx_descriptor) override;
    bool usesChannelSearch() const override;

    void process() override;
    void reset_rx_state() override;

    void send(const void* data, size_t size, bool flush) override;
    bool receive(void* data, size_t& size, bool& restoredByFEC) override;

    void setChannel(int ch) override;
    void setTxPower(int txPower) override; //MIN_TX_POWER...MAX_TX_POWER
    void setMonitorMode(const std::vector<std::string> interfaces) override;

    void setTxInterface(const std::string& interface) override;

    const RX_Descriptor& getRXDescriptor() const override;

    size_t get_data_rate() const override;
    int get_input_dBm() const override;

    PacketFilter& getPacketFilter() override;

    struct PCap;
    struct RX;
    struct TX;

private:
    bool prepare_pcap(std::string const& interface, PCap& pcap, RX_Descriptor const& rx_descriptor);

    bool prepare_filter(PCap& pcap);
    void prepare_radiotap_header(size_t rate_hz);
    void prepare_tx_packet_header(uint8_t* buffer);
    bool process_rx_packet(PCap& pcap);
    void process_rx_packets();
    void sync_rx_decoder_stats();

    void tx_thread_proc();
    void rx_thread_proc(size_t index);

    TX_Descriptor m_tx_descriptor;
    RX_Descriptor m_rx_descriptor;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_exit = false;

    size_t m_packet_header_offset = 0;   //=RADIOTAP_HEADER.size() + sizeof(WLAN_IEEE_HEADER_GROUND2AIR);
    size_t m_payload_offset = 0;         //=RADIOTAP_HEADER.size() + sizeof(WLAN_IEEE_HEADER_GROUND2AIR) + Packet_Header

    std::atomic_int m_best_input_dBm = {0};
    std::atomic_int m_latched_input_dBm = {0};

    size_t m_data_stats_rate = 0;
    size_t m_data_stats_data_accumulated = 0;
    Clock::time_point m_data_stats_last_tp = Clock::now();
    uint64_t m_last_rx_decoded_bytes_total = 0;
    PacketFilter m_packet_filter;
};
