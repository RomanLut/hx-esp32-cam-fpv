#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "Clock.h"
#include "fec.h"
#include "packet_filter.h"
#include "structures.h"

namespace gs::core
{

class ITransportManager;

struct TXDescriptor
{
    std::string interface;
    uint32_t coding_k = 12;
    uint32_t coding_n = 20;
    size_t mtu = WLAN_MAX_PAYLOAD_SIZE - sizeof(Packet_Header);
};

struct RXDescriptor
{
    std::vector<std::string> interfaces;
    // Resets RX FEC/block assembly state after this much inactivity.
    Clock::duration reset_duration = std::chrono::milliseconds(1000);
    uint32_t coding_k = 12;
    uint32_t coding_n = 20;
    size_t mtu = WLAN_MAX_PAYLOAD_SIZE - sizeof(Packet_Header);
    // When true, do not ask libpcap to enable monitor mode on RX interfaces.
    bool skip_mon_mode_cfg = true;
};

class ITransport
{
public:
    virtual ~ITransport() = default;

    virtual bool init(const RXDescriptor& rx_descriptor, const TXDescriptor& tx_descriptor) = 0;
    virtual bool usesChannelSearch() const = 0;

    virtual void process() = 0;
    virtual void reset_rx_state() = 0;

    virtual void send(const void* data, size_t size, bool flush) = 0;
    virtual bool receive(void* data, size_t& size, bool& restoredByFEC) = 0;

    virtual void setChannel(int ch) = 0;
    virtual void setTxPower(int txPower) = 0;
    virtual void setMonitorMode(const std::vector<std::string> interfaces) = 0;
    virtual void setTxInterface(const std::string& interface) = 0;

    virtual const RXDescriptor& getRXDescriptor() const = 0;

    virtual size_t get_data_rate() const = 0;
    virtual int get_input_dBm() const = 0;

    virtual PacketFilter& getPacketFilter() = 0;
};

}

extern gs::core::ITransportManager* s_transportManager;
extern gs::core::ITransport* s_transport;
extern std::mutex s_transport_mutex;
