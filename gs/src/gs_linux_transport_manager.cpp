#include "gs_linux_transport_manager.h"

//===================================================================================
//===================================================================================
// Resolves the transport instance that should back a given transport kind on Linux.
gs::core::ITransport& LinuxTransportManager::resolveTransport(gs::core::TransportKind kind)
{
    switch (kind)
    {
    case gs::core::TransportKind::RawBroadcast:
        return m_raw_broadcast;

    case gs::core::TransportKind::APFPV:
        return m_apfpv_transport;

    case gs::core::TransportKind::TestTransport:
        return m_test_transport;

    case gs::core::TransportKind::WifiChannelScan:
        return m_wifi_scan_transport;
    }

    return m_raw_broadcast;
}

//===================================================================================
//===================================================================================
// Reports whether the resolved Linux transport kind has already been initialized.
bool LinuxTransportManager::isTransportInitialized(gs::core::TransportKind kind) const
{
    switch (kind)
    {
    case gs::core::TransportKind::RawBroadcast:
        return m_raw_broadcast_initialized;

    case gs::core::TransportKind::APFPV:
        return m_apfpv_transport_initialized;

    case gs::core::TransportKind::TestTransport:
        return m_test_transport_initialized;

    case gs::core::TransportKind::WifiChannelScan:
        return m_wifi_scan_transport_initialized;
    }

    return false;
}

//===================================================================================
//===================================================================================
// Marks whether the resolved Linux transport kind has been initialized.
void LinuxTransportManager::setTransportInitialized(gs::core::TransportKind kind, bool initialized)
{
    switch (kind)
    {
    case gs::core::TransportKind::RawBroadcast:
        m_raw_broadcast_initialized = initialized;
        break;

    case gs::core::TransportKind::APFPV:
        m_apfpv_transport_initialized = initialized;
        break;

    case gs::core::TransportKind::TestTransport:
        m_test_transport_initialized = initialized;
        break;

    case gs::core::TransportKind::WifiChannelScan:
        m_wifi_scan_transport_initialized = initialized;
        break;
    }
}

//===================================================================================
//===================================================================================
// Resolves the transport instance that should back a given transport kind on Linux.
const gs::core::ITransport& LinuxTransportManager::resolveTransport(gs::core::TransportKind kind) const
{
    switch (kind)
    {
    case gs::core::TransportKind::RawBroadcast:
        return m_raw_broadcast;

    case gs::core::TransportKind::APFPV:
        return m_apfpv_transport;

    case gs::core::TransportKind::TestTransport:
        return m_test_transport;

    case gs::core::TransportKind::WifiChannelScan:
        return m_wifi_scan_transport;
    }

    return m_raw_broadcast;
}

//===================================================================================
//===================================================================================
// Returns the shared Linux transport-manager instance used by the process globals.
LinuxTransportManager& getLinuxTransportManager()
{
    static LinuxTransportManager s_manager;
    return s_manager;
}
