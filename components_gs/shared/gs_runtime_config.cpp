#include "gs_runtime_config.h"

#include "../../components/common/Clock.h"
#include "Log.h"
#include "core/osd_menu_common.h"
#include "core/transport_manager.h"
#include "core/transport_manager_base.h"
#include "gs_runtime_core.h"
#include "gs_runtime_platform_services.h"
#include "gs_runtime_state.h"
#include "gs_shared_runtime.h"
#include "settings_storage.h"
#include <thread>

namespace
{

std::atomic<bool> s_selected_transport_reconnect_pending = false;
std::atomic<bool> s_selected_transport_reconnect_worker_running = false;
std::atomic<bool> s_transport_reconnect_pause_requested = false;
std::atomic<bool> s_transport_reconnect_pause_observed = false;
std::mutex s_transport_interface_cache_mutex;
std::vector<std::string> s_transport_interface_cache;

//===================================================================================
//===================================================================================
// Stores the latest active transport RX interface snapshot for lock-free UI reads.
void updateTransportInterfaceCacheLocked()
{
    if (s_transport == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> cache_lock(s_transport_interface_cache_mutex);
    s_transport_interface_cache = s_transport->getRXDescriptor().interfaces;
}

//===================================================================================
//===================================================================================
// Returns the last cached active transport RX interface snapshot without touching the transport mutex.
std::vector<std::string> copyCachedTransportInterfaces()
{
    std::lock_guard<std::mutex> cache_lock(s_transport_interface_cache_mutex);
    return s_transport_interface_cache;
}

//===================================================================================
//===================================================================================
// Waits briefly until the comms thread acknowledges the requested reconnect/switch pause.
bool waitForTransportReconnectPauseObserved(Clock::duration timeout)
{
    const Clock::time_point wait_deadline = Clock::now() + timeout;
    while (!isTransportReconnectPauseObserved() && Clock::now() < wait_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    return isTransportReconnectPauseObserved();
}

//===================================================================================
//===================================================================================
// Runs the selected transport reconnect sequence while the transport mutex is already held.
bool requestSelectedTransportReconnectLocked()
{
    if (s_transport == nullptr)
    {
        LOGW("requestSelectedTransportReconnect failed: no active transport");
        return false;
    }

    LOGI("requestSelectedTransportReconnect kind={}", static_cast<int>(currentTransportKind()));
    const bool reconnect_requested = s_transport->requestImmediateReconnect();
    if (!reconnect_requested)
    {
        LOGW("requestSelectedTransportReconnect transport returned false");
        return false;
    }

    if (s_RuntimePlatformServices != nullptr)
    {
        s_RuntimePlatformServices->invalidateDisplayedVideoFrame();
    }
    return true;
}

//===================================================================================
//===================================================================================
// Runs the selected transport reconnect sequence while the transport-processing loop is explicitly paused.
bool requestSelectedTransportReconnectWhilePaused()
{
    if (s_transport == nullptr)
    {
        LOGW("requestSelectedTransportReconnect failed: no active transport");
        return false;
    }

    LOGI("requestSelectedTransportReconnect while paused kind={}", static_cast<int>(currentTransportKind()));
    const bool reconnect_requested = s_transport->requestImmediateReconnect();
    if (!reconnect_requested)
    {
        LOGW("requestSelectedTransportReconnect transport returned false");
        return false;
    }

    if (s_RuntimePlatformServices != nullptr)
    {
        s_RuntimePlatformServices->invalidateDisplayedVideoFrame();
    }
    return true;
}

//===================================================================================
//===================================================================================
// Processes queued reconnect requests on a background worker so menu rendering is never blocked by transport mutex contention.
void selectedTransportReconnectWorker()
{
    for (;;)
    {
        if (!s_selected_transport_reconnect_pending.exchange(false))
        {
            break;
        }

        LOGI("selectedTransportReconnectWorker processing request");
        requestTransportReconnectPause();
        waitForTransportReconnectPauseObserved(std::chrono::milliseconds(250));
        requestSelectedTransportReconnectWhilePaused();
        releaseTransportReconnectPause();
    }

    s_selected_transport_reconnect_worker_running.store(false);
    if (s_selected_transport_reconnect_pending.load() &&
        !s_selected_transport_reconnect_worker_running.exchange(true))
    {
        std::thread(selectedTransportReconnectWorker).detach();
    }
}

} // namespace

void initializeGroundStationConfigDefaults(uint16_t gs_device_id)
{
    s_groundstation_config.socket_fd = 0;
    s_groundstation_config.wifi_channel = DEFAULT_WIFI_CHANNEL_2_4GHZ;
    s_groundstation_config.wifiBand = DEFAULT_GS_WIFI_BAND;
    s_groundstation_config.screenAspectRatio = ScreenAspectRatio::LETTERBOX;
    s_groundstation_config.txPower = gs::menu::kDefaultTxPower;
    s_groundstation_config.vsync = true;
    s_groundstation_config.txInterface = "auto";
    s_groundstation_config.apfpvInterface = "auto";
    s_groundstation_config.transportKind = gs::core::TransportKind::RawBroadcast;
    s_groundstation_config.GPIOKeysLayout = 0;
    s_groundstation_config.stats = false;
    s_groundstation_config.vrMode = false;
    s_groundstation_config.deviceId = gs_device_id;
    setApfpvPreferredCameraId(s_groundstation_config.apfpvPreferredCameraId);
}

void loadSharedSettings(uint16_t gs_device_id)
{
    initializeGroundStationConfigDefaults(gs_device_id);
    s_settingsStorage.read();
    s_settingsStorage.loadGroundStationConfig();
    s_settingsStorage.loadGround2AirConfig();
    setApfpvPreferredCameraId(s_groundstation_config.apfpvPreferredCameraId);
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
    if (s_transportManager == nullptr)
    {
        return false;
    }

    requestTransportReconnectPause();
    waitForTransportReconnectPauseObserved(std::chrono::milliseconds(250));
    std::lock_guard<std::mutex> transport_lock(s_transport_mutex);

    if (!s_transportManager->switchTransport(kind))
    {
        releaseTransportReconnectPause();
        LOGE("switchActiveTransport failed kind={}", static_cast<int>(kind));
        return false;
    }

    LOGI("switchActiveTransport ok kind={}", static_cast<int>(kind));
    updateTransportInterfaceCacheLocked();

    s_groundstation_config.transportKind = kind;
    s_settingsStorage.saveGroundStationConfig();
    s_runtimeCore.gs_device_id = s_groundstation_config.deviceId;
    s_runtimeCore.resetTransportRuntime(*s_transport, Clock::now());
    if (s_RuntimePlatformServices != nullptr)
    {
        s_RuntimePlatformServices->invalidateDisplayedVideoFrame();
    }
    releaseTransportReconnectPause();
    return true;
}

//===================================================================================
//===================================================================================
// Requests an immediate reconnect on the currently active transport after menu-driven changes.
bool requestSelectedTransportReconnect()
{
    std::lock_guard<std::mutex> transport_lock(s_transport_mutex);
    return requestSelectedTransportReconnectLocked();
}

//===================================================================================
//===================================================================================
// Queues one selected-transport reconnect request to be consumed by the runtime transport loop.
void queueSelectedTransportReconnect()
{
    LOGI("queueSelectedTransportReconnect");
    s_selected_transport_reconnect_pending.store(true);
    if (!s_selected_transport_reconnect_worker_running.exchange(true))
    {
        std::thread(selectedTransportReconnectWorker).detach();
    }
}

//===================================================================================
//===================================================================================
// Processes one queued selected-transport reconnect request when the runtime loop reaches a safe point.
bool processPendingSelectedTransportReconnect()
{
    if (!s_selected_transport_reconnect_pending.exchange(false))
    {
        return false;
    }

    std::lock_guard<std::mutex> transport_lock(s_transport_mutex);
    LOGI("processPendingSelectedTransportReconnect");
    return requestSelectedTransportReconnectLocked();
}

//===================================================================================
//===================================================================================
// Requests that the transport-processing loop temporarily stop taking the transport mutex so reconnect work can proceed.
void requestTransportReconnectPause()
{
    s_transport_reconnect_pause_requested.store(true);
}

//===================================================================================
//===================================================================================
// Releases a previously requested transport-processing pause after reconnect work finishes.
void releaseTransportReconnectPause()
{
    s_transport_reconnect_pause_requested.store(false);
    s_transport_reconnect_pause_observed.store(false);
}

//===================================================================================
//===================================================================================
// Returns whether a reconnect worker has requested a temporary pause of the transport-processing loop.
bool isTransportReconnectPauseRequested()
{
    return s_transport_reconnect_pause_requested.load();
}

//===================================================================================
//===================================================================================
// Records whether the transport-processing loop has observed the requested reconnect pause.
void setTransportReconnectPauseObserved(bool observed)
{
    s_transport_reconnect_pause_observed.store(observed);
}

//===================================================================================
//===================================================================================
// Returns whether the transport-processing loop has acknowledged the requested reconnect pause.
bool isTransportReconnectPauseObserved()
{
    return s_transport_reconnect_pause_observed.load();
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
    std::unique_lock<std::mutex> transport_lock(s_transport_mutex, std::try_to_lock);
    if (!transport_lock.owns_lock() || s_transport == nullptr)
    {
        return copyCachedTransportInterfaces();
    }

    updateTransportInterfaceCacheLocked();
    return copyCachedTransportInterfaces();
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
