#include "gs_runtime_config.h"

#include "Clock.h"
#include "Log.h"
#include "core/osd_menu_common.h"
#include "core/transport_manager.h"
#include "core/transport_manager_base.h"
#include "gs_runtime_core.h"
#include "gs_runtime_platform_services.h"
#include "gs_runtime_state.h"
#include "gs_shared_runtime.h"
#include "settings_storage.h"

void initializeGroundStationConfigDefaults(uint16_t gs_device_id)
{
    s_groundstation_config.socket_fd = 0;
    s_groundstation_config.wifi_channel = DEFAULT_WIFI_CHANNEL_2_4GHZ;
    s_groundstation_config.wifiBand = DEFAULT_GS_WIFI_BAND;
    s_groundstation_config.screenAspectRatio = ScreenAspectRatio::LETTERBOX;
    s_groundstation_config.txPower = gs::menu::kDefaultTxPower;
    s_groundstation_config.vsync = true;
    s_groundstation_config.txInterface = "auto";
    s_groundstation_config.transportKind = gs::core::TransportKind::RawBroadcast;
    s_groundstation_config.GPIOKeysLayout = 0;
    s_groundstation_config.stats = false;
    s_groundstation_config.vrMode = false;
    s_groundstation_config.deviceId = gs_device_id;
}

void loadSharedSettings(uint16_t gs_device_id)
{
    initializeGroundStationConfigDefaults(gs_device_id);
    s_settingsStorage.read();
    s_settingsStorage.loadGroundStationConfig();
    s_settingsStorage.loadGround2AirConfig();
}

void commitGround2AirConfig(const Ground2Air_Config_Packet& config)
{
    s_runtimeCore.session.setConfigPacket(config);
    s_settingsStorage.saveGround2AirConfig();
}

void applyWifiChannelToSession(Ground2Air_Config_Packet& config)
{
    config.dataChannel.wifi_channel = s_groundstation_config.wifi_channel;
    s_runtimeCore.session.setConfigPacket(config);
}

void applyWifiChannelInstantToSession(Ground2Air_Config_Packet& config, gs::core::ITransport& transport)
{
    applyWifiChannelToSession(config);
    std::lock_guard<std::mutex> transport_lock(s_transport_mutex);
    transport.setChannel(s_groundstation_config.wifi_channel);
}

void applyGSTxPowerToTransport(gs::core::ITransport& transport)
{
    std::lock_guard<std::mutex> transport_lock(s_transport_mutex);
    transport.setTxPower(s_groundstation_config.txPower);
}

void performAirUnpair(uint16_t gs_device_id, gs::core::ITransport& transport)
{
    s_last_packet_tp = Clock::now();
    s_last_stats_packet_tp = Clock::now();
    resetAirPairing(gs_device_id, transport);
}

void resetAirPairing(uint16_t gs_device_id, gs::core::ITransport& transport)
{
    s_runtimeCore.gs_device_id = gs_device_id;
    std::lock_guard<std::mutex> transport_lock(s_transport_mutex);
    s_runtimeCore.resetPairing(transport, Clock::now());
}

//===================================================================================
//===================================================================================
// Returns the currently active transport kind, falling back to persisted settings.
gs::core::TransportKind currentTransportKind()
{
    return s_transportManager != nullptr ? s_transportManager->activeKind()
                                         : s_groundstation_config.transportKind;
}

//===================================================================================
//===================================================================================
// Switches the active transport backend, persists the selection, and resets runtime state.
bool switchActiveTransport(gs::core::TransportKind kind)
{
    std::lock_guard<std::mutex> transport_lock(s_transport_mutex);

    if (s_transportManager == nullptr)
    {
        return false;
    }

    if (!s_transportManager->switchTransport(kind))
    {
        LOGE("switchActiveTransport failed kind={}", static_cast<int>(kind));
        return false;
    }

    LOGI("switchActiveTransport ok kind={}", static_cast<int>(kind));

    s_groundstation_config.transportKind = kind;
    s_settingsStorage.saveGroundStationConfig();
    s_runtimeCore.gs_device_id = s_groundstation_config.deviceId;
    s_runtimeCore.resetTransportRuntime(*s_transport, Clock::now());
    if (s_RuntimePlatformServices != nullptr)
    {
        s_RuntimePlatformServices->invalidateDisplayedVideoFrame();
    }
    return true;
}

//===================================================================================
//===================================================================================
// Starts the selected transport's search-or-connect flow.
void beginSelectedTransportSearchOrConnect(Ground2Air_Config_Packet& config, Clock::time_point& search_tp)
{
    bool search_done = false;
    if (s_transportManager != nullptr)
    {
        s_transportManager->beginSearchOrConnect(config, search_tp, search_done);
    }
}

//===================================================================================
//===================================================================================
// Advances the selected transport's search-or-connect flow and updates completion state.
void advanceSelectedTransportSearchOrConnect(Ground2Air_Config_Packet& config,
                                             Clock::time_point& search_tp,
                                             bool& search_done)
{
    if (s_transportManager != nullptr)
    {
        s_transportManager->advanceSearchOrConnect(config, search_tp, search_done);
    }
}

//===================================================================================
//===================================================================================
// Cancels the selected transport's search-or-connect flow.
void cancelSelectedTransportSearchOrConnect()
{
    if (s_transportManager != nullptr)
    {
        s_transportManager->cancelSearchOrConnect();
    }
}

//===================================================================================
//===================================================================================
// Reports whether the current session is connected to an air device.
bool isSelectedTransportConnected()
{
    return s_transportManager != nullptr ? s_transportManager->isConnected()
                                         : s_runtimeCore.session.connectedAirDeviceId() != 0;
}

//===================================================================================
//===================================================================================
// Returns a copy of the interfaces exposed by the active transport descriptor.
std::vector<std::string> copyCurrentTransportInterfaces()
{
    std::lock_guard<std::mutex> transport_lock(s_transport_mutex);
    return s_transport->getRXDescriptor().interfaces;
}

//===================================================================================
//===================================================================================
// Applies the selected GS TX interface and TX power to the active transport.
void applySelectedTxInterfaceToTransport()
{
    std::lock_guard<std::mutex> transport_lock(s_transport_mutex);
    const std::vector<std::string> interfaces = s_transport->getRXDescriptor().interfaces;
    if (s_groundstation_config.txInterface == "auto")
    {
        if (!interfaces.empty())
        {
            s_transport->setTxInterface(interfaces.front());
        }
    }
    else
    {
        s_transport->setTxInterface(s_groundstation_config.txInterface);
    }

    s_transport->setTxPower(s_groundstation_config.txPower);
}
