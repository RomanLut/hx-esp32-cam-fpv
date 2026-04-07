#pragma once

#include "Clock.h"
#include "packets.h"
#include "transport.h"
#include "transport_kind.h"

namespace gs::core
{

//===================================================================================
//===================================================================================
// Manages the currently active transport and performs runtime transport switching.
class ITransportManager
{
public:
    virtual ~ITransportManager() = default;

    virtual bool init(TransportKind initial_kind,
                      const RXDescriptor& rx_descriptor,
                      const TXDescriptor& tx_descriptor) = 0;
    virtual bool switchTransport(TransportKind kind) = 0;

    virtual TransportKind activeKind() const = 0;
    virtual void beginSearchOrConnect(Ground2Air_Config_Packet& config,
                                      Clock::time_point& search_tp,
                                      bool& search_done) = 0;
    virtual void advanceSearchOrConnect(Ground2Air_Config_Packet& config,
                                        Clock::time_point& search_tp,
                                        bool& search_done) = 0;
    virtual void cancelSearchOrConnect() = 0;
    virtual bool isConnected() const = 0;
    virtual ITransport& activeTransport() = 0;
    virtual const ITransport& activeTransport() const = 0;
};

}
