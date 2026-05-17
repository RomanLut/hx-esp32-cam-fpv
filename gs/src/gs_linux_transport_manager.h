#pragma once

#include "linux_apfpv_transport.h"
#include "linux_raw_broadcast_transport.h"
#include "linux_wifi_scan_transport.h"
#include "core/transport_manager_base.h"
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

    LinuxRawBroadcastTransport m_raw_broadcast;
    LinuxApfpvTransport        m_apfpv_transport;
    GSTestTransport            m_test_transport;
    LinuxWifiScanTransport     m_wifi_scan_transport;
    bool m_raw_broadcast_initialized      = false;
    bool m_apfpv_transport_initialized    = false;
    bool m_test_transport_initialized     = false;
    bool m_wifi_scan_transport_initialized = false;
};

LinuxTransportManager& getLinuxTransportManager();
