#include "gs_linux_runtime_platform_services.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <ifaddrs.h>
#include <net/if.h>

#include "cpu_temp.h"
#include "gpio_buttons.h"
#include "gs_recordings_storage.h"
#include "gs_linux_runtime.h"
#include "gs_runtime_config.h"
#include "core/transport_manager.h"

namespace
{

//===================================================================================
//===================================================================================
// Owns the Linux runtime platform services instance for explicit shared binding.
LinuxRuntimePlatformServices s_linux_runtime_platform_services;

} // namespace

//===================================================================================
//===================================================================================
// Returns the shared Linux runtime platform services instance for explicit binding.
IRuntimePlatformServices& getLinuxRuntimePlatformServices()
{
    return s_linux_runtime_platform_services;
}

//===================================================================================
//===================================================================================
// Returns the current Linux CPU temperature in Celsius.
float LinuxRuntimePlatformServices::getCpuTemperatureCelsius() const
{
    return g_CPUTemp.getTemperature();
}

//===================================================================================
//===================================================================================
// Returns the current Linux IPv4 address of the active non-loopback interface.
std::string LinuxRuntimePlatformServices::getSystemIPv4() const
{
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0 || ifaddr == nullptr)
    {
        return "0.0.0.0";
    }

    std::string result = "0.0.0.0";
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0)
        {
            continue;
        }

        char host[INET_ADDRSTRLEN] = {0};
        const auto* addr = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
        if (inet_ntop(AF_INET, &addr->sin_addr, host, sizeof(host)) != nullptr)
        {
            result = host;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return result;
}

//===================================================================================
//===================================================================================
// Reports that Linux supports the extended screen-aspect menu variants.
bool LinuxRuntimePlatformServices::supportsCustomScreenAspectModes() const
{
    return true;
}

//===================================================================================
//===================================================================================
// Applies Linux VSync immediately through the HAL backend.
void LinuxRuntimePlatformServices::setVsync(bool enabled)
{
    s_hal->set_vsync(enabled, true);
}

//===================================================================================
//===================================================================================
// Exits the Linux application, stopping recording first when needed.
void LinuxRuntimePlatformServices::exitApp()
{
    if (s_recordingsStorage->isRecording())
    {
        s_recordingsStorage->toggleRecording(0, 0, "exit_app");
    }
    std::abort();
}

//===================================================================================
//===================================================================================
// Restarts Linux GPIO buttons using the existing platform controls.
void LinuxRuntimePlatformServices::restartGPIOButtons()
{
    gpio_buttons_stop();
    gpio_buttons_start();
}

//===================================================================================
//===================================================================================
// Invalidates the currently displayed Linux video frame and drops stale decoded output.
void LinuxRuntimePlatformServices::invalidateDisplayedVideoFrame()
{
    s_decoder.invalidate_displayed_frame();
}

//===================================================================================
//===================================================================================
// Applies the selected GS Wi-Fi channel by committing the new config immediately and
// deferring only the local raw-broadcast monitor retune when needed.
void LinuxRuntimePlatformServices::applyGroundStationWifiChannel(Ground2Air_Config_Packet& config)
{
    applyWifiChannelToSession(config);

    if (s_transportManager != nullptr &&
        s_transportManager->activeKind() == gs::core::TransportKind::RawBroadcast)
    {
        s_change_channel = Clock::now() + std::chrono::milliseconds(3000);
    }
}

//===================================================================================
//===================================================================================
// Applies the selected GS TX power to the active Linux transport.
void LinuxRuntimePlatformServices::applyGroundStationTxPower(Ground2Air_Config_Packet& /* config */)
{
    applyGSTxPowerToTransport(*s_transport);
}
