#include "core/osd_menu_shared.h"

#include <algorithm>
#include <array>
#include <cstdio>

#include "wifi_channels.h"

namespace
{

std::string getWifiChannelLabel(int channel)
{
    char buf[64];
    if (channel >= 36 && channel <= 48)
    {
        std::snprintf(buf, sizeof(buf), "%d  (5.8GHz,ETSI,FCC)", channel);
    }
    else if (channel >= 52 && channel <= 144)
    {
        std::snprintf(buf, sizeof(buf), "%d  (5.8GHz,ETSI,FCC,DFS)", channel);
    }
    else if (channel >= 149 && channel <= 165)
    {
        std::snprintf(buf, sizeof(buf), "%d  (5.8GHz,FCC)", channel);
    }
    else if (channel == 12 || channel == 13)
    {
        std::snprintf(buf, sizeof(buf), "%d  (2.4GHz,ETSI)", channel);
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "%d  (2.4GHz,ETSI,FCC)", channel);
    }
    return buf;
}

}

namespace gs::menu
{

void resetMenuState(MenuState& state)
{
    state.visible = false;
    state.menu_id = OSDMenuId::Main;
    state.selected_item = 0;
    state.back_menu_ids.clear();
    state.back_menu_items.clear();
}

void goForward(MenuState& state, OSDMenuId new_menu_id, int new_item)
{
    state.back_menu_ids.push_back(state.menu_id);
    state.back_menu_items.push_back(state.selected_item);
    state.menu_id = new_menu_id;
    state.selected_item = new_item;
    state.visible = true;
}

bool goBack(MenuState& state)
{
    if (state.back_menu_ids.empty())
    {
        state.visible = false;
        return false;
    }

    state.menu_id = state.back_menu_ids.back();
    state.back_menu_ids.pop_back();
    state.selected_item = state.back_menu_items.back();
    state.back_menu_items.pop_back();
    return true;
}

MenuScreen buildMenuScreen(const MenuState& state, const MenuViewContext& context)
{
    MenuScreen screen;
    screen.selected_item = state.selected_item;
    screen.can_go_back = !state.back_menu_ids.empty();

    if (context.config == nullptr)
    {
        return screen;
    }

    const auto& config = *context.config;
    constexpr std::array<const char*, 3> kScreenModes = {
        "Stretch",
        "Letterbox",
        "Zoom to Fill"
    };
    constexpr std::array<const char*, 3> kWifiBands = {
        "2.4GHz",
        "5.8GHz",
        "2.4GHz & 5.8GHz"
    };
    constexpr std::array<const char*, 2> kGpioLayouts = {
        "DIY VRX",
        "Runcam VRX"
    };
    constexpr std::array<const char*, 1> kOsdFonts = {
        "Built-in"
    };
    constexpr std::array<const char*, 5> kSharpnessLabels = {
        "Blur more",
        "Blur",
        "Normal",
        "Sharpen",
        "Sharpen more"
    };

    switch (state.menu_id)
    {
    case OSDMenuId::Main:
        screen.title = "ESP32-CAM-FPV";
        screen.items.push_back("Search & Connect...");
        screen.items.push_back("Resolution: " + getResolutionSummary(config, context.is_ov5640));
        screen.items.push_back("Wifi Channel: " + std::to_string(config.dataChannel.wifi_channel));
        screen.items.push_back("Wifi Rate: " + getWifiRateSummary(config));
        screen.items.push_back("FEC: " + getFecSummary(config));
        screen.items.push_back("Camera Settings...");
        screen.items.push_back("Ground Station Settings...");
        screen.status_lines.push_back(context.air_sd_status);
        screen.status_lines.push_back(context.gs_sd_status);
        break;

    case OSDMenuId::Search:
        screen.title = std::string("Menu -> Search (") + kWifiBands[std::clamp(context.gs_wifi_band, 0, 2)] + ")...";
        if (context.connected_air_device_id != 0)
        {
            screen.status_lines.push_back("Found Channel: " + std::to_string(config.dataChannel.wifi_channel));
        }
        else
        {
            screen.status_lines.push_back("Searching: Channel " + std::to_string(config.dataChannel.wifi_channel) + "...");
        }
        break;

    case OSDMenuId::GSSettings:
        screen.title = "Menu -> GS Settings";
        screen.items.push_back("Screen Settings...");
        screen.items.push_back("Wifi Settings...");
        screen.items.push_back("Debuging...");
        screen.items.push_back("GPIO Keys Layout: " + std::string(kGpioLayouts[std::clamp(context.gs_gpio_keys_layout, 0, 1)]));
        screen.items.push_back("Exit To Shell");
        screen.status_lines.push_back("IP: " + context.gs_ip);
        break;

    case OSDMenuId::GSScreen:
        screen.title = "Menu -> GS Screen";
        screen.items.push_back("Screen Mode: " + std::string(kScreenModes[std::clamp(context.gs_screen_aspect_ratio, 0, 2)]));
        break;

    case OSDMenuId::Letterbox:
        screen.title = "GS Settings -> Screen Mode";
        for (const char* label : kScreenModes)
        {
            screen.items.push_back(label);
        }
        break;

    case OSDMenuId::GSWifiSettings:
        screen.title = "Menu -> GS Wifi Settings";
        screen.items.push_back("Band: " + std::string(kWifiBands[std::clamp(context.gs_wifi_band, 0, 2)]));
        screen.items.push_back("TX Interface: " + context.gs_tx_interface);
        screen.items.push_back("TX Power: " + std::to_string(context.gs_tx_power));
        if (context.transport_interfaces.empty())
        {
            screen.status_lines.push_back("No network interfaces detected");
        }
        else
        {
            screen.status_lines = context.transport_interfaces;
        }
        break;

    case OSDMenuId::GSTxInterface:
        screen.title = "GS Settings -> TX Interface";
        screen.items.push_back("auto");
        screen.items.insert(screen.items.end(), context.transport_interfaces.begin(), context.transport_interfaces.end());
        break;

    case OSDMenuId::GSTxPower:
        screen.title = "Menu -> Tx Power";
        for (int power = 5; power <= 63; ++power)
        {
            screen.items.push_back(std::to_string(power));
        }
        break;

    case OSDMenuId::Debug:
        screen.title = "Menu -> Debugging";
        screen.items.push_back("Toggle Statistics");
        screen.items.push_back("Draw packets");
        screen.items.push_back("Draw packets till loss");
        screen.items.push_back("Hide packets");
        break;

    case OSDMenuId::CameraSettings:
        screen.title = "Menu -> Camera Settings";
        screen.items.push_back("Image Settings...");
        screen.items.push_back("OSD Font: " + std::string(kOsdFonts[std::clamp(context.gs_osd_font_index, 0, static_cast<int>(kOsdFonts.size() - 1))]));
        screen.items.push_back(std::string("Autostart recording: ") + (config.misc.autostartRecord ? "On" : "Off"));
        screen.items.push_back(std::string("Camera Off RC Channel: ") + (config.misc.cameraStopChannel == 0 ? "None" : std::to_string(config.misc.cameraStopChannel)));
        screen.items.push_back(std::string("Mavlink2 to Msp RC: ") + (config.misc.mavlink2mspRC ? "On" : "Off"));
        screen.items.push_back("Air to GS MTU: " + std::to_string(config.dataChannel.fec_codec_mtu));
        break;

    case OSDMenuId::OSDFont:
        screen.title = "GS -> Displayport OSD Font";
        for (const char* font : kOsdFonts)
        {
            screen.items.push_back(font);
        }
        break;

    case OSDMenuId::Image:
        screen.title = "Menu -> Image Settings";
        screen.items.push_back("Brightness: " + std::to_string(config.camera.brightness));
        screen.items.push_back("Contrast: " + std::to_string(config.camera.contrast));
        screen.items.push_back("Exposure: " + std::to_string(config.camera.ae_level));
        screen.items.push_back("Saturation: " + std::to_string(config.camera.saturation));
        screen.items.push_back("Sharpness: " + std::string(kSharpnessLabels[std::clamp<int>(config.camera.sharpness, -2, 2) + 2]));
        if (!context.is_ov5640)
        {
            screen.items.push_back(std::string("Vertical Flip: ") + (config.camera.vflip ? "Enabled" : "Disabled"));
            screen.items.push_back(std::string("40FPS (overclock): ") + (config.camera.ov2640HighFPS ? "Enabled" : "Disabled"));
        }
        else
        {
            screen.items.push_back(std::string("50fps Modes: ") + (config.camera.ov5640HighFPS ? "Enabled" : "Disabled"));
        }
        break;

    case OSDMenuId::Brightness:
    case OSDMenuId::Contrast:
    case OSDMenuId::Exposure:
    case OSDMenuId::Saturation:
        screen.title = state.menu_id == OSDMenuId::Brightness ? "Camera Settings -> Brightness" :
                       state.menu_id == OSDMenuId::Contrast ? "Camera Settings -> Contrast" :
                       state.menu_id == OSDMenuId::Exposure ? "Camera Settings -> Exposure" :
                                                              "Camera Settings -> Saturation";
        for (int value = -2; value <= 2; ++value)
        {
            screen.items.push_back(std::to_string(value));
        }
        break;

    case OSDMenuId::Sharpness:
        screen.title = "Camera Settings -> Sharpness";
        screen.items.assign(kSharpnessLabels.begin(), kSharpnessLabels.end());
        break;

    case OSDMenuId::Resolution:
        screen.title = "Menu -> Resolution";
        for (int i = 0; i < 6; ++i)
        {
            screen.items.push_back(getResolutionOptionLabel(config, context.is_ov5640, i, true));
        }
        break;

    case OSDMenuId::WifiRate:
        screen.title = "Menu -> Wifi Rate";
        for (int i = 0; i < 6; ++i)
        {
            screen.items.push_back(getWifiRateOptionLabel(i));
        }
        break;

    case OSDMenuId::WifiChannel:
        screen.title = "Menu -> Wifi Channel";
        for (int i = 0; i < WIFI_CHANNELS_COUNT; ++i)
        {
            const int channel = WIFI_CHANNELS_BY_INDEX[i];
            if (!isWifiChannelAllowedByBand(channel, context.gs_wifi_band))
            {
                continue;
            }
            screen.items.push_back(getWifiChannelLabel(channel));
        }
        break;

    case OSDMenuId::CameraStopCH:
        screen.title = "Menu -> Camera Stop Channel";
        screen.items.push_back("None");
        for (int i = 1; i <= 18; ++i)
        {
            screen.items.push_back(std::to_string(i));
        }
        break;

    case OSDMenuId::ExitToShell:
        screen.title = "Exit To Shell ?";
        screen.items.push_back("Exit");
        screen.items.push_back("Cancel");
        break;

    case OSDMenuId::FEC:
        screen.title = "Menu -> FEC";
        for (int i = 0; i < 3; ++i)
        {
            screen.items.push_back(getFecOptionLabel(i));
        }
        break;

    case OSDMenuId::Restart:
        screen.title = "Restart ?";
        break;
    }

    return screen;
}

}
