#include "gs_linux_runtime_platform_services.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include "cpu_temp.h"
#include "gpio_buttons.h"
#include "gs_linux_runtime.h"
#include "gs_shared_runtime.h"

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
// Returns Linux ground storage capacity and free space tracked by the runtime.
GroundStorageStatus LinuxRuntimePlatformServices::getGroundStorageStatus() const
{
    return {
        s_GSSDFreeSpaceBytes,
        s_GSSDTotalSpaceBytes,
    };
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
// Applies Linux VSync immediately through the HAL backend.
void LinuxRuntimePlatformServices::setVsync(bool enabled)
{
    s_hal->set_vsync(enabled, true);
}

//===================================================================================
//===================================================================================
// Exits the Linux application using the existing shared runtime action.
void LinuxRuntimePlatformServices::exitApp()
{
    ::exitApp();
}

//===================================================================================
//===================================================================================
// Restarts Linux GPIO buttons using the existing platform controls.
void LinuxRuntimePlatformServices::restartGPIOButtons()
{
    gpio_buttons_stop();
    gpio_buttons_start();
}
