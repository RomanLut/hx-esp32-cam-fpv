#include "android_runtime_platform_services.h"

#include <array>
#include <chrono>
#include <cstring>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "gs_runtime_core.h"
#include "gs_shared_state.h"

namespace
{

//===================================================================================
//===================================================================================
// Owns the Android runtime platform services instance for explicit shared binding.
AndroidRuntimePlatformServices s_android_runtime_platform_services;

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
// Returns the Android ground-station CPU temperature when available.
float AndroidRuntimePlatformServices::getCpuTemperatureCelsius() const
{
    return 0.0f;
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
