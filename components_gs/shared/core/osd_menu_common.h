#pragma once

#include <string>

#include "packets.h"

enum class OSDMenuId
{
    Main,
    CameraSettings,
    Resolution,
    Brightness,
    Contrast,
    Exposure,
    Saturation,
    Sharpness,
    ExitToShell,
    Letterbox,
    WifiRate,
    WifiChannel,
    Restart,
    FEC,
    GSSettings,
    GSWifiSettings,
    GSScreen,
    OSDFont,
    Search,
    GSTxPower,
    GSTxInterface,
    Image,
    CameraStopCH,
    Debug
};

namespace gs::menu
{

std::string getResolutionSummary(const Ground2Air_Config_Packet& config, bool is_ov5640);
const char* getResolutionOptionLabel(const Ground2Air_Config_Packet& config, bool is_ov5640, int menu_index, bool aspect_variant);
int getResolutionMenuIndex(Resolution resolution);
Resolution getResolutionForMenuIndex(int menu_index);

std::string getWifiRateSummary(const Ground2Air_Config_Packet& config);
const char* getWifiRateLabel(WIFI_Rate rate);
const char* getWifiRateOptionLabel(int menu_index);
int getWifiRateMenuIndex(WIFI_Rate rate);
WIFI_Rate getWifiRateForMenuIndex(int menu_index);

std::string getFecSummary(const Ground2Air_Config_Packet& config);
const char* getFecOptionLabel(int menu_index);
int getFecMenuIndex(const Ground2Air_Config_Packet& config);
uint8_t getFecNForMenuIndex(int menu_index);

int getResolutionCycleSize();
Resolution getResolutionCycleValue(int index);
Resolution getDefaultCyclingResolution();

}
