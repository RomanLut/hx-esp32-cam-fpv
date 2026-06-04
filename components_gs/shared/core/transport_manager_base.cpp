#include "core/transport_manager_base.h"

#include "gs_runtime_config.h"
#include "gs_runtime_core.h"
#include "gs_runtime_state.h"
#include "gs_shared_runtime.h"
#include "wifi_channels.h"

namespace
{

constexpr int kSearchTimeStepMs = 1000;

//===================================================================================
//===================================================================================
// Advances the configured Wi-Fi channel to the next allowed value and applies it immediately.
void advanceSearchWifiChannel(Ground2Air_Config_Packet& config,
                              gs::core::ITransport& transport,
                              Clock::time_point& search_tp)
{
    auto& gs_config = s_groundstation_config;
    search_tp = Clock::now() + std::chrono::milliseconds(kSearchTimeStepMs);

    gs_config.wifi_channel = getBandAwareWifiChannel(gs_config.wifi_channel, gs_config.wifiBand);
    int channel_index = getWifiChannelIndex(gs_config.wifi_channel);

    for (int i = 0; i < WIFI_CHANNELS_COUNT; i++)
    {
        channel_index++;
        if (channel_index >= WIFI_CHANNELS_COUNT)
        {
            channel_index = 0;
        }

        const int next_channel = WIFI_CHANNELS_BY_INDEX[channel_index];
        if (isWifiChannelAllowedByBand(next_channel, gs_config.wifiBand))
        {
            gs_config.wifi_channel = next_channel;
            break;
        }
    }

    applyWifiChannelInstantToSession(config, transport);
}

}

namespace gs::core
{

//===================================================================================
//===================================================================================
// Returns the user-facing label for the selected transport mode.
const char* TransportManagerBase::transportModeLabel(TransportKind kind)
{
    switch (kind)
    {
    case TransportKind::RawBroadcast:
        return "RAW Broadcast";

    case TransportKind::APFPV:
        return "APFPV";

    case TransportKind::TestTransport:
        return "Test";

    case TransportKind::WifiChannelScan:
        return "Wifi Channel Scan";

    case TransportKind::Count:
        break;
    }

    return "RAW Broadcast";
}

//===================================================================================
//===================================================================================
// Reports whether the selected transport kind uses Wi-Fi channel search.
bool TransportManagerBase::transportKindUsesChannelSearch(TransportKind kind)
{
    return kind == TransportKind::RawBroadcast;
}

//===================================================================================
//===================================================================================
// Initializes the requested transport and caches descriptors for future switches.
bool TransportManagerBase::init(TransportKind initial_kind,
                                const RXDescriptor& rx_descriptor,
                                const TXDescriptor& tx_descriptor)
{
    m_rx_descriptor = rx_descriptor;
    m_tx_descriptor = tx_descriptor;
    m_descriptors_ready = true;
    return switchTransport(initial_kind);
}

//===================================================================================
//===================================================================================
// Switches the active transport implementation and initializes it once with cached descriptors.
bool TransportManagerBase::switchTransport(TransportKind kind)
{
    ITransport& transport = resolveTransport(kind);
    const bool switching_transport = m_active_transport != nullptr && m_active_transport != &transport;
    if (m_active_transport != nullptr && m_active_transport != &transport)
    {
        m_active_transport->deactivate();
    }
    if (switching_transport)
    {
        // Linux transport switches can leave backend-specific device state behind, especially
        // when the same Wi-Fi adapter changes mode between managed APFPV and monitor raw-broadcast.
        // Reinitializing the destination transport on every real switch matches a fresh process
        // startup more closely than reusing partially stale backend state.
        setTransportInitialized(kind, false);
    }
    if (!isTransportInitialized(kind) && m_descriptors_ready && !transport.init(m_rx_descriptor, m_tx_descriptor))
    {
        return false;
    }

    setTransportInitialized(kind, true);
    transport.activate();
    m_active_transport = &transport;
    m_active_kind = kind;
    s_transport = m_active_transport;
    return true;
}

//===================================================================================
//===================================================================================
// Returns the currently active transport kind.
TransportKind TransportManagerBase::activeKind() const
{
    return m_active_kind;
}

//===================================================================================
//===================================================================================
// Starts the active transport's search-or-connect lifecycle.
void TransportManagerBase::beginSearchOrConnect(Ground2Air_Config_Packet& config,
                                                Clock::time_point& search_tp,
                                                bool& search_done)
{
    search_done = false;

    if (m_active_transport != nullptr && m_active_transport->supportsMenuSearchOrConnect())
    {
        m_active_transport->beginMenuSearchOrConnect();
        search_tp = Clock::now();
        return;
    }

    if (m_active_transport != nullptr && m_active_transport->usesChannelSearch())
    {
        advanceSearchWifiChannel(config, *m_active_transport, search_tp);
        performAirUnpair(s_groundstation_config.deviceId, *m_active_transport);
        return;
    }

    search_tp = Clock::now();
    search_done = isConnected();
}

//===================================================================================
//===================================================================================
// Advances the active transport's search-or-connect lifecycle.
void TransportManagerBase::advanceSearchOrConnect(Ground2Air_Config_Packet& config,
                                                  Clock::time_point& search_tp,
                                                  bool& search_done)
{
    if (m_active_transport != nullptr && m_active_transport->supportsMenuSearchOrConnect())
    {
        search_done = m_active_transport->advanceMenuSearchOrConnect();
        return;
    }

    if (m_active_transport == nullptr || !m_active_transport->usesChannelSearch())
    {
        search_done = isConnected();
        return;
    }

    if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - search_tp).count() <= kSearchTimeStepMs)
    {
        return;
    }

    if (search_done)
    {
        return;
    }

    if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s_last_packet_tp).count() < kSearchTimeStepMs / 2)
    {
        search_done = true;
        search_tp = Clock::now() + std::chrono::milliseconds(kSearchTimeStepMs);
        s_settingsStorage.saveGroundStationConfig();
        return;
    }

    advanceSearchWifiChannel(config, *m_active_transport, search_tp);
}

//===================================================================================
//===================================================================================
// Cancels any active search-or-connect lifecycle.
void TransportManagerBase::cancelSearchOrConnect()
{
    if (m_active_transport != nullptr && m_active_transport->supportsMenuSearchOrConnect())
    {
        m_active_transport->cancelMenuSearchOrConnect();
    }
}

//===================================================================================
//===================================================================================
// Reports whether the runtime session is connected to an air device.
bool TransportManagerBase::isConnected() const
{
    return s_runtimeCore.session.connectedAirDeviceId() != 0;
}

//===================================================================================
//===================================================================================
// Returns the currently active transport implementation.
ITransport& TransportManagerBase::activeTransport()
{
    return *m_active_transport;
}

//===================================================================================
//===================================================================================
// Returns the currently active transport implementation.
const ITransport& TransportManagerBase::activeTransport() const
{
    return *m_active_transport;
}

}
