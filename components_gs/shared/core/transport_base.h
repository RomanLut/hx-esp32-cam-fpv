#pragma once

#include "Log.h"
#include "transport.h"

namespace gs::core
{

//===================================================================================
//===================================================================================
// Provides shared descriptor storage and noop implementations for optional transport controls.
class TransportBase : public ITransport
{
public:
    void storeDescriptors(const RXDescriptor& rx_descriptor, const TXDescriptor& tx_descriptor)
    {
        m_rx_descriptor = rx_descriptor;
        m_tx_descriptor = tx_descriptor;
    }

    bool usesChannelSearch() const override
    {
        return false;
    }

    void reset_rx_state() override {}
    void setChannel(int /* ch */) override {}
    void setTxPower(int /* txPower */) override {}
    void setMonitorMode(const std::vector<std::string> /* interfaces */) override {}
    void setTxInterface(const std::string& /* interface */) override {}
    const RXDescriptor& getRXDescriptor() const override
    {
        return m_rx_descriptor;
    }

    size_t get_data_rate() const override
    {
        return 0;
    }

    int get_input_dBm() const override
    {
        return 0;
    }

    PacketFilter& getPacketFilter() override
    {
        return m_packet_filter;
    }

protected:
    RXDescriptor m_rx_descriptor = {};
    TXDescriptor m_tx_descriptor = {};
    PacketFilter m_packet_filter = {};
};

}
