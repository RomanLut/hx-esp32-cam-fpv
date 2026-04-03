#include "osd_menu.h"

#include "Comms.h"
#include "frame_packets_debug.h"
#include "flight_osd.h"
#include "gs_runtime_osd_font_storage.h"
#include "gs_runtime_config.h"
#include "lodepng.h"
#include "gs_linux_runtime.h"

namespace
{

class LinuxOSDMenuPlatform final : public gs::menu::IOSDMenuPlatform
{
public:
    gs::core::ITransport& transport() override
    {
        return s_transport;
    }

    const gs::core::ITransport& transport() const override
    {
        return s_transport;
    }

    void applyWifiChannel(Ground2Air_Config_Packet& config) override
    {
        applyWifiChannelToSession(config);
        s_change_channel = Clock::now() + std::chrono::milliseconds(3000);
    }

    void applyWifiChannelInstant(Ground2Air_Config_Packet& config) override
    {
        applyWifiChannelInstantToSession(config, s_transport);
    }

    void applyGSTxPower(Ground2Air_Config_Packet& config) override
    {
        (void)config;
        applyGSTxPowerToTransport(s_transport);
    }

    void airUnpair() override
    {
        performAirUnpair(s_groundstation_config.deviceId, s_transport);
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
