#pragma once

#include "Comms.h"
#include "core/transport_manager_base.h"
#include "gs_stub_transport.h"
#include "gs_test_transport.h"

//===================================================================================
//===================================================================================
// Owns Linux transport implementations and switches the active runtime transport at runtime.
class LinuxTransportManager final : public gs::core::TransportManagerBase
{
public:
private:
    gs::core::ITransport& resolveTransport(gs::core::TransportKind kind) override;
    const gs::core::ITransport& resolveTransport(gs::core::TransportKind kind) const override;
    bool isTransportInitialized(gs::core::TransportKind kind) const override;
    void setTransportInitialized(gs::core::TransportKind kind, bool initialized) override;

    Comms m_raw_broadcast;
    GSStubTransport m_apfpv_stub = GSStubTransport("Linux APFPV");
    GSTestTransport m_test_transport;
    bool m_raw_broadcast_initialized = false;
    bool m_apfpv_stub_initialized = false;
    bool m_test_transport_initialized = false;
};

LinuxTransportManager& getLinuxTransportManager();
