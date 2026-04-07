#include "android_transport_manager.h"

//===================================================================================
//===================================================================================
// Returns the concrete Android APFPV transport for bridge-specific state injection.
AndroidAPFPVTransport& AndroidTransportManager::apfpvTransport()
{
    return m_apfpv_transport;
}

//===================================================================================
//===================================================================================
// Returns the concrete Android APFPV transport for read-only bridge state access.
const AndroidAPFPVTransport& AndroidTransportManager::apfpvTransport() const
{
    return m_apfpv_transport;
}

//===================================================================================
//===================================================================================
// Resolves the transport instance that should back a given transport kind on Android.
gs::core::ITransport& AndroidTransportManager::resolveTransport(gs::core::TransportKind kind)
{
    switch (kind)
    {
    case gs::core::TransportKind::RawBroadcast:
        return m_raw_broadcast_stub;

    case gs::core::TransportKind::APFPV:
        return m_apfpv_transport;

    case gs::core::TransportKind::TestTransport:
        return m_test_transport;
    }

    return m_apfpv_transport;
}

//===================================================================================
//===================================================================================
// Reports whether the resolved Android transport kind has already been initialized.
bool AndroidTransportManager::isTransportInitialized(gs::core::TransportKind kind) const
{
    switch (kind)
    {
    case gs::core::TransportKind::RawBroadcast:
        return m_raw_broadcast_stub_initialized;

    case gs::core::TransportKind::APFPV:
        return m_apfpv_initialized;

    case gs::core::TransportKind::TestTransport:
        return m_test_transport_initialized;
    }

    return false;
}

//===================================================================================
//===================================================================================
// Marks whether the resolved Android transport kind has been initialized.
void AndroidTransportManager::setTransportInitialized(gs::core::TransportKind kind, bool initialized)
{
    switch (kind)
    {
    case gs::core::TransportKind::RawBroadcast:
        m_raw_broadcast_stub_initialized = initialized;
        break;

    case gs::core::TransportKind::APFPV:
        m_apfpv_initialized = initialized;
        break;

    case gs::core::TransportKind::TestTransport:
        m_test_transport_initialized = initialized;
        break;
    }
}

//===================================================================================
//===================================================================================
// Resolves the transport instance that should back a given transport kind on Android.
const gs::core::ITransport& AndroidTransportManager::resolveTransport(gs::core::TransportKind kind) const
{
    switch (kind)
    {
    case gs::core::TransportKind::RawBroadcast:
        return m_raw_broadcast_stub;

    case gs::core::TransportKind::APFPV:
        return m_apfpv_transport;

    case gs::core::TransportKind::TestTransport:
        return m_test_transport;
    }

    return m_apfpv_transport;
}
