#include "core/osd_menu_common.h"

#include <algorithm>

namespace
{

const char* kResolutionName2640[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 30fps",
    "640x360 30fps",
    "800x600 30fps",
    "800x456 30fps",
    "1024x768",
    "1024x576 13fps",
    "1280x960",
    "1280x720 13fps",
    "1600x1200"
};

const char* kResolutionName2640Hi[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 40fps",
    "640x360 40fps",
    "800x600 30fps",
    "800x456 40fps",
    "1024x768",
    "1024x576 13fps",
    "1280x960",
    "1280x720 13fps",
    "1600x1200"
};

const char* kResolutionName5640[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 30fps",
    "640x360 30fps",
    "800x600 30fps",
    "800x456 30fps",
    "1024x768",
    "1024x576 30fps",
    "1280x960",
    "1280x720 30fps",
    "1600x1200"
};

const char* kResolutionName5640Hi[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 40fps",
    "640x360 50fps",
    "800x600 30fps",
    "800x456 50fps",
    "1024x768",
    "1024x576 30fps",
    "1280x960",
    "1280x720 30fps",
    "1600x1200"
};

const char* kResolutionName2640Aspect[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 30fps (4:3)",
    "640x360 30fps (16:9)",
    "800x600 30fps (4:3)",
    "800x456 30fps (16:9)",
    "1024x768",
    "1024x576 13fps (16:9)",
    "1280x960",
    "1280x720 13fps (16:9)",
    "1600x1200"
};

const char* kResolutionName2640HiAspect[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 40fps (4:3)",
    "640x360 40fps (16:9)",
    "800x600 30fps (4:3)",
    "800x456 40fps (16:9)",
    "1024x768",
    "1024x576 13fps (16:9)",
    "1280x960",
    "1280x720 13fps (16:9)",
    "1600x1200"
};

const char* kResolutionName5640Aspect[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 30fps (4:3)",
    "640x360 30fps (16:9)",
    "800x600 30fps (4:3)",
    "800x456 30fps (16:9)",
    "1024x768",
    "1024x576 30fps (16:9)",
    "1280x960",
    "1280x720 30fps (16:9)",
    "1600x1200"
};

const char* kResolutionName5640HiAspect[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 40fps (4:3)",
    "640x360 50fps (16:9)",
    "800x600 30fps (4:3)",
    "800x456 50fps (16:9)",
    "1024x768",
    "1024x576 30fps (16:9)",
    "1280x960",
    "1280x720 30fps (16:9)",
    "1600x1200"
};

const char* const* getResolutionNames(bool is_ov5640, bool high_fps, bool aspect_variant)
{
    if (is_ov5640)
    {
        if (aspect_variant)
        {
            return high_fps ? kResolutionName5640HiAspect : kResolutionName5640Aspect;
        }
        return high_fps ? kResolutionName5640Hi : kResolutionName5640;
    }

    if (aspect_variant)
    {
        return high_fps ? kResolutionName2640HiAspect : kResolutionName2640Aspect;
    }
    return high_fps ? kResolutionName2640Hi : kResolutionName2640;
}

constexpr const char* kWifiRateLabels[] =
{
    "OFDM 18Mbps",
    "OFDM 24Mbps",
    "OFDM 36Mbps",
    "MCS2L 19.5Mbps",
    "MCS3L 26Mbps",
    "MCS4L 39Mbps",
    "Other"
};

constexpr WIFI_Rate kWifiRates[] =
{
    WIFI_Rate::RATE_G_18M_ODFM,
    WIFI_Rate::RATE_G_24M_ODFM,
    WIFI_Rate::RATE_G_36M_ODFM,
    WIFI_Rate::RATE_N_19_5M_MCS2,
    WIFI_Rate::RATE_N_26M_MCS3,
    WIFI_Rate::RATE_N_39M_MCS4
};

constexpr const char* kWifiRateRawLabels[] =
{
    "2M_L", "2M_S", "5M_L", "5M_S", "11M_L", "11M_S",
    "6M", "9M", "12M", "18M", "24M", "36M", "48M", "54M",
    "MCS0_6.5ML", "MCS0_7.2MS", "MCS1L_13M", "MCS1S_14.4M", "MCS2L_19.5M", "MCS2S_21.7M",
    "MCS3L_26M", "MCS3S_28.9M", "MCS4L_39M", "MCS4S_43.3M", "MCS5L_52M",
    "MCS5S_57.8M", "MCS6L_58.5M", "MCS6S_65M", "MCS7L_65", "MCS7S_72.2"
};

constexpr const char* kFecLabels[] =
{
    "Weak (6/8)",
    "Medium (6/10)",
    "Strong (6/12)"
};

constexpr Resolution kResolutionCycle[] =
{
    Resolution::VGA16,
    Resolution::VGA,
    Resolution::SVGA16,
    Resolution::SVGA,
    Resolution::XGA16,
    Resolution::HD
};

}

namespace gs::menu
{

std::string getResolutionSummary(const Ground2Air_Config_Packet& config, bool is_ov5640)
{
    const bool high_fps = is_ov5640 ? config.camera.ov5640HighFPS : config.camera.ov2640HighFPS;
    const char* const* names = getResolutionNames(is_ov5640, high_fps, false);
    const int index = std::clamp(static_cast<int>(config.camera.resolution), 0, static_cast<int>(Resolution::COUNT) - 1);
    return names[index];
}

const char* getResolutionOptionLabel(const Ground2Air_Config_Packet& config, bool is_ov5640, int menu_index, bool aspect_variant)
{
    const bool high_fps = is_ov5640 ? config.camera.ov5640HighFPS : config.camera.ov2640HighFPS;
    const char* const* names = getResolutionNames(is_ov5640, high_fps, aspect_variant);
    switch (menu_index)
    {
    case 0: return names[static_cast<int>(Resolution::VGA16)];
    case 1: return names[static_cast<int>(Resolution::VGA)];
    case 2: return names[static_cast<int>(Resolution::SVGA16)];
    case 3: return names[static_cast<int>(Resolution::SVGA)];
    case 4: return names[static_cast<int>(Resolution::XGA16)];
    case 5: return names[static_cast<int>(Resolution::HD)];
    default: return names[static_cast<int>(Resolution::VGA16)];
    }
}

int getResolutionMenuIndex(Resolution resolution)
{
    switch (resolution)
    {
    case Resolution::VGA16: return 0;
    case Resolution::VGA: return 1;
    case Resolution::SVGA16: return 2;
    case Resolution::SVGA: return 3;
    case Resolution::XGA16: return 4;
    case Resolution::HD: return 5;
    default: return 0;
    }
}

Resolution getResolutionForMenuIndex(int menu_index)
{
    switch (menu_index)
    {
    case 0: return Resolution::VGA16;
    case 1: return Resolution::VGA;
    case 2: return Resolution::SVGA16;
    case 3: return Resolution::SVGA;
    case 4: return Resolution::XGA16;
    case 5: return Resolution::HD;
    default: return Resolution::VGA16;
    }
}

std::string getWifiRateSummary(const Ground2Air_Config_Packet& config)
{
    return kWifiRateLabels[getWifiRateMenuIndex(config.dataChannel.wifi_rate)];
}

const char* getWifiRateLabel(WIFI_Rate rate)
{
    const int index = std::clamp(static_cast<int>(rate), 0, static_cast<int>(std::size(kWifiRateRawLabels)) - 1);
    return kWifiRateRawLabels[index];
}

const char* getWifiRateOptionLabel(int menu_index)
{
    return kWifiRateLabels[std::clamp(menu_index, 0, 5)];
}

int getWifiRateMenuIndex(WIFI_Rate rate)
{
    for (int i = 0; i < 6; ++i)
    {
        if (kWifiRates[i] == rate)
        {
            return i;
        }
    }
    return 6;
}

WIFI_Rate getWifiRateForMenuIndex(int menu_index)
{
    return kWifiRates[std::clamp(menu_index, 0, 5)];
}

std::string getFecSummary(const Ground2Air_Config_Packet& config)
{
    return kFecLabels[getFecMenuIndex(config)];
}

const char* getFecOptionLabel(int menu_index)
{
    return kFecLabels[std::clamp(menu_index, 0, 2)];
}

int getFecMenuIndex(const Ground2Air_Config_Packet& config)
{
    return config.dataChannel.fec_codec_n == 8 ? 0 : config.dataChannel.fec_codec_n == 12 ? 2 : 1;
}

uint8_t getFecNForMenuIndex(int menu_index)
{
    switch (menu_index)
    {
    case 0: return 8;
    case 2: return 12;
    default: return 10;
    }
}

int getResolutionCycleSize()
{
    return static_cast<int>(std::size(kResolutionCycle));
}

Resolution getResolutionCycleValue(int index)
{
    return kResolutionCycle[std::clamp(index, 0, static_cast<int>(std::size(kResolutionCycle)) - 1)];
}

Resolution getDefaultCyclingResolution()
{
    return Resolution::SVGA16;
}

}
