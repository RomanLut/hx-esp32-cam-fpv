#pragma once

#include "transport_manager.h"

namespace gs::core
{

//===================================================================================
//===================================================================================
// Provides shared transport-manager state and lifecycle logic across platforms.
class TransportManagerBase : public ITransportManager
{
public:
    static const char* transportModeLabel(TransportKind kind);
    static bool transportKindUsesChannelSearch(TransportKind kind);

    bool init(TransportKind initial_kind,
              const RXDescriptor& rx_descriptor,
              const TXDescriptor& tx_descriptor) override;
    bool switchTransport(TransportKind kind) override;

    TransportKind activeKind() const override;
    void beginSearchOrConnect(Ground2Air_Config_Packet& config,
                              Clock::time_point& search_tp,
                              bool& search_done) override;
    void advanceSearchOrConnect(Ground2Air_Config_Packet& config,
                                Clock::time_point& search_tp,
                                bool& search_done) override;
    void cancelSearchOrConnect() override;
    bool isConnected() const override;
    ITransport& activeTransport() override;
    const ITransport& activeTransport() const override;

protected:
    virtual ITransport& resolveTransport(TransportKind kind) = 0;
    virtual const ITransport& resolveTransport(TransportKind kind) const = 0;
    virtual bool isTransportInitialized(TransportKind kind) const = 0;
    virtual void setTransportInitialized(TransportKind kind, bool initialized) = 0;

    ITransport* m_active_transport = nullptr;
    TransportKind m_active_kind = TransportKind::RawBroadcast;
    RXDescriptor m_rx_descriptor = {};
    TXDescriptor m_tx_descriptor = {};
    bool m_descriptors_ready = false;
};

}
