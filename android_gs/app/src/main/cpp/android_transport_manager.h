#pragma once

#include "android_apfpv_transport.h"
#include "core/transport_manager_base.h"
#include "gs_stub_transport.h"
#include "gs_test_transport.h"

//===================================================================================
//===================================================================================
// Owns Android transport implementations and switches the active runtime transport at runtime.
class AndroidTransportManager final : public gs::core::TransportManagerBase
{
public:
    AndroidAPFPVTransport& apfpvTransport();
    const AndroidAPFPVTransport& apfpvTransport() const;

private:
    gs::core::ITransport& resolveTransport(gs::core::TransportKind kind) override;
    const gs::core::ITransport& resolveTransport(gs::core::TransportKind kind) const override;
    bool isTransportInitialized(gs::core::TransportKind kind) const override;
    void setTransportInitialized(gs::core::TransportKind kind, bool initialized) override;

    AndroidAPFPVTransport m_apfpv_transport;
    GSStubTransport m_raw_broadcast_stub = GSStubTransport("Android raw_broadcast");
    GSTestTransport m_test_transport;
    bool m_apfpv_initialized = false;
    bool m_raw_broadcast_stub_initialized = false;
    bool m_test_transport_initialized = false;
};
