#include "android_runtime_platform_services.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Log.h"
#include "gs_runtime_core.h"
#include "gs_runtime_config.h"
#include "gs_shared_state.h"
#include "gs_video_renderer.h"

namespace
{

Clock::time_point s_pending_channel_change_tp = Clock::now() + std::chrono::hours(10000);
Clock::time_point s_raw_broadcast_control_burst_until_tp = Clock::time_point::min();

//===================================================================================
//===================================================================================
// Owns the Android runtime platform services instance for explicit shared binding.
AndroidRuntimePlatformServices s_android_runtime_platform_services;
GsVideoRenderer* s_android_runtime_renderer = nullptr;
std::atomic<bool> s_android_renderer_invalidate_requested = false;
std::atomic<int> s_android_thermal_status = 0;

//===================================================================================
//===================================================================================
// Detects the active Android IPv4 address from non-loopback interfaces.
std::string detectSystemIPv4()
{
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
    {
        return "0.0.0.0";
    }

    std::string result = "0.0.0.0";
    std::array<char, 8192> buffer = {};
    struct ifconf ifc = {};
    ifc.ifc_len = static_cast<int>(buffer.size());
    ifc.ifc_buf = buffer.data();
    if (ioctl(socket_fd, SIOCGIFCONF, &ifc) != 0)
    {
        close(socket_fd);
        return result;
    }

    const int interface_count = ifc.ifc_len / static_cast<int>(sizeof(struct ifreq));
    struct ifreq* interfaces = ifc.ifc_req;
    for (int index = 0; index < interface_count; ++index)
    {
        struct ifreq flags_request = {};
        std::strncpy(flags_request.ifr_name, interfaces[index].ifr_name, IFNAMSIZ - 1);
        if (ioctl(socket_fd, SIOCGIFFLAGS, &flags_request) != 0)
        {
            continue;
        }
        if ((flags_request.ifr_flags & IFF_UP) == 0 || (flags_request.ifr_flags & IFF_LOOPBACK) != 0)
        {
            continue;
        }

        const sockaddr_in* addr_in = reinterpret_cast<const sockaddr_in*>(&interfaces[index].ifr_addr);
        if (addr_in->sin_family != AF_INET)
        {
            continue;
        }

        char addr[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &addr_in->sin_addr, addr, sizeof(addr)) != nullptr &&
            std::strcmp(addr, "0.0.0.0") != 0)
        {
            result = addr;
            break;
        }
    }

    close(socket_fd);
    return result;
}

} // namespace

//===================================================================================
//===================================================================================
// Returns the shared Android runtime platform services instance for explicit binding.
IRuntimePlatformServices& getAndroidRuntimePlatformServices()
{
    return s_android_runtime_platform_services;
}

//===================================================================================
//===================================================================================
// Binds the Android renderer used by shared runtime platform services.
void bindAndroidRuntimeRenderer(GsVideoRenderer* renderer)
{
    s_android_runtime_renderer = renderer;
}

//===================================================================================
//===================================================================================
// Returns and clears the deferred Android renderer invalidation request flag.
bool consumeAndroidRendererInvalidateRequest()
{
    return s_android_renderer_invalidate_requested.exchange(false);
}

//===================================================================================
//===================================================================================
// Stores the latest Android thermal status for shared UI and stats rendering.
void setAndroidThermalStatus(int thermal_status)
{
    s_android_thermal_status.store(std::max(0, thermal_status));
}

//===================================================================================
//===================================================================================
// Returns the Android ground-station CPU temperature when available.
float AndroidRuntimePlatformServices::getCpuTemperatureCelsius() const
{
    // Android currently reuses the legacy GS temperature field to display the
    // thermal severity id in the existing stats panel until that UI gets a
    // dedicated thermal-status label.
    return static_cast<float>(s_android_thermal_status.load());
}

//===================================================================================
//===================================================================================
// Returns the cached Android IPv4 address, refreshing it periodically.
std::string AndroidRuntimePlatformServices::getSystemIPv4() const
{
    const Clock::time_point now = Clock::now();
    if (m_cached_ipv4.empty() || now >= m_next_ipv4_refresh_tp)
    {
        m_cached_ipv4 = detectSystemIPv4();
        m_next_ipv4_refresh_tp = now + std::chrono::seconds(1);
    }
    return m_cached_ipv4;
}

//===================================================================================
//===================================================================================
// Reports that Android only supports the simplified screen-aspect menu variants.
bool AndroidRuntimePlatformServices::supportsCustomScreenAspectModes() const
{
    return false;
}

//===================================================================================
//===================================================================================
// Reports that Android supports runtime decode-pipeline mode selection.
bool AndroidRuntimePlatformServices::supportsPipelineModeSelection() const
{
    return true;
}

//===================================================================================
//===================================================================================
// Updates the Android VSync setting consumed by the renderer path.
void AndroidRuntimePlatformServices::setVsync(bool enabled)
{
    s_groundstation_config.vsync = enabled;
}

//===================================================================================
//===================================================================================
// Requests Android runtime shutdown through the shared runtime core.
void AndroidRuntimePlatformServices::exitApp()
{
    s_runtimeCore.exit_requested = true;
}

//===================================================================================
//===================================================================================
// Restarts Android GPIO buttons when supported; currently a no-op.
void AndroidRuntimePlatformServices::restartGPIOButtons()
{
}

//===================================================================================
//===================================================================================
// Invalidates the currently displayed Android video frame in the shared renderer.
void AndroidRuntimePlatformServices::invalidateDisplayedVideoFrame()
{
    s_android_renderer_invalidate_requested.store(true);
}

//===================================================================================
//===================================================================================
// Applies the selected GS Wi-Fi channel through the shared session helper before
// persisting the Android config packet so APFPV control packets always inherit the
// exact channel that the GS menu committed.
void AndroidRuntimePlatformServices::applyGroundStationWifiChannel(Ground2Air_Config_Packet& config)
{
    LOGI("applyGroundStationWifiChannel menu={} gs={} apfpv={}",
         static_cast<unsigned int>(config.dataChannel.wifi_channel),
         s_groundstation_config.wifi_channel,
         static_cast<int>(config.misc.apfpv));
    applyWifiChannelToSession(config);
    s_runtimeCore.config_packet = config;
    commitGround2AirConfig(s_runtimeCore.config_packet);

    if (currentTransportKind() == gs::core::TransportKind::RawBroadcast)
    {
        const Clock::time_point now = Clock::now();
        s_pending_channel_change_tp = now + std::chrono::milliseconds(3000);
        // Keep sending config/control packets faster until RX silence confirms
        // the air unit changed channel and the GS adapter retune is applied.
        s_raw_broadcast_control_burst_until_tp = Clock::time_point::max();
    }
}

//===================================================================================
//===================================================================================
// Applies the selected GS TX power by committing the Android config packet immediately.
void AndroidRuntimePlatformServices::applyGroundStationTxPower(Ground2Air_Config_Packet& config)
{
    s_runtimeCore.config_packet = config;
    commitGround2AirConfig(s_runtimeCore.config_packet);
}

//===================================================================================
//===================================================================================
// Retunes the RTL8812AU adapter once the air unit has had time to switch channels.
// Called each background loop tick: waits 3 s after a channel change for config
// packets to reach the air unit, then defers until the link goes quiet (< 300 ms
// since the last received packet), then applies the new channel to the adapter.
void processPendingRawBroadcastChannelChange(gs::core::ITransport& transport)
{
    if (Clock::now() <= s_pending_channel_change_tp)
    {
        return;
    }

    const auto silence_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - s_runtimeCore.last_packet_tp).count();
    if (silence_ms < 300)
    {
        s_pending_channel_change_tp = Clock::now() + std::chrono::milliseconds(3000);
        return;
    }

    s_pending_channel_change_tp = Clock::now() + std::chrono::hours(10000);
    LOGI("Applying deferred channel change to {} (silence={} ms)",
         s_groundstation_config.wifi_channel,
         static_cast<long long>(silence_ms));
    transport.setChannel(s_groundstation_config.wifi_channel);
    s_raw_broadcast_control_burst_until_tp = Clock::time_point::min();
}

//===================================================================================
//===================================================================================
// Reports whether raw-broadcast should send control/config packets faster until
// the deferred silence-gated channel change is applied locally.
bool rawBroadcastControlBurstActive(Clock::time_point now)
{
    return now <= s_raw_broadcast_control_burst_until_tp;
}
