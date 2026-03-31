#include "osd_menu.h"

#include <arpa/inet.h>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>

#include "Comms.h"
#include "frame_packets_debug.h"
#include "gpio_buttons.h"
#include "lodepng.h"
#include "desktop_osd.h"

namespace
{

class LinuxOSDMenuPlatform final : public gs::menu::IOSDMenuPlatform
{
public:
    TGroundstationConfig& groundstationConfig() override
    {
        return s_groundstation_config;
    }

    const TGroundstationConfig& groundstationConfig() const override
    {
        return s_groundstation_config;
    }

    gs::core::ITransport& transport() override
    {
        return s_transport;
    }

    const gs::core::ITransport& transport() const override
    {
        return s_transport;
    }

    bool isOV5640() const override
    {
        return s_isOV5640;
    }

    bool isDualCamera() const override
    {
        return s_isDual;
    }

    gs::menu::AirStorageStatus airStorageStatus() const override
    {
        return {
            s_SDDetected,
            s_SDError,
            s_SDSlow,
            s_SDFreeSpaceGB16,
            s_SDTotalSpaceGB16,
        };
    }

    gs::menu::GroundStorageStatus groundStorageStatus() const override
    {
        return {
            s_GSSDFreeSpaceBytes,
            s_GSSDTotalSpaceBytes,
        };
    }

    const char* currentOSDFontName() const override
    {
        return g_osd.currentFontName;
    }

    const std::vector<std::string>& osdFontsList() const override
    {
        return g_osd.fontsList;
    }

    void selectOSDFont(Ground2Air_Config_Packet& config, const std::string& font_name) override
    {
        ini["gs"]["osd_font"] = font_name;
        s_iniFile.write(ini);
        config.misc.osdFontCRC32 = lodepng_crc32(reinterpret_cast<const unsigned char*>(font_name.c_str()),
                                                 font_name.length());
        s_reload_osd_font = true;
    }

    void saveGroundStationConfig() override
    {
        ::saveGroundStationConfig();
    }

    void saveGround2AirConfig(const Ground2Air_Config_Packet& config) override
    {
        ::saveGround2AirConfig(config);
    }

    void applyWifiChannel(Ground2Air_Config_Packet& config) override
    {
        ::applyWifiChannel(config);
    }

    void applyWifiChannelInstant(Ground2Air_Config_Packet& config) override
    {
        ::applyWifiChannelInstant(config);
    }

    void applyGSTxPower(Ground2Air_Config_Packet& config) override
    {
        ::applyGSTxPower(config);
    }

    void airUnpair() override
    {
        ::airUnpair();
    }

    void exitApp() override
    {
        ::exitApp();
    }

    void restartGPIOButtons() override
    {
        gpio_buttons_stop();
        gpio_buttons_start();
    }

    void setVsync(bool enabled) override
    {
        s_hal->set_vsync(enabled, true);
    }

    std::string systemIPv4() const override
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

            char addr[INET_ADDRSTRLEN] = {0};
            const void* src = &reinterpret_cast<sockaddr_in*>(ifa->ifa_addr)->sin_addr;
            if (inet_ntop(AF_INET, src, addr, sizeof(addr)) != nullptr && strcmp(addr, "0.0.0.0") != 0)
            {
                result = addr;
                break;
            }
        }

        freeifaddrs(ifaddr);
        return result;
    }

    Clock::time_point lastPacketTime() const override
    {
        return s_last_packet_tp;
    }

    void captureFrameDebug(bool until_loss) override
    {
        g_framePacketsDebug.captureFrame(until_loss);
    }

    void disableFrameDebug() override
    {
        g_framePacketsDebug.off();
    }
};

LinuxOSDMenuPlatform s_menu_platform;

} // namespace

OSDMenu g_osdMenu(s_menu_platform);
