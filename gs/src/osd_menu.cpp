#include "osd_menu.h"

#include "Comms.h"
#include "frame_packets_debug.h"
#include "gs_runtime_osd_font_storage.h"
#include "lodepng.h"
#include "linux_osd.h"
#include "gs_linux_runtime.h"
#include "gs_osd_font_shared.h"
#include "gs_shared_runtime.h"

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

    const char* currentOSDFontName() const override
    {
        static thread_local std::string font_name;
        font_name = getSelectedOsdFontName();
        return font_name.c_str();
    }

    void selectOSDFont(Ground2Air_Config_Packet& config, const std::string& font_name) override
    {
        setSelectedOsdFontName(font_name);
        s_settingsStorage.save();
        config.misc.osdFontCRC32 = lodepng_crc32(reinterpret_cast<const unsigned char*>(font_name.c_str()),
                                                 font_name.length());
        s_reload_osd_font = true;
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
