#pragma once

#include "main.h"
#include "imgui.h"
#include "packets.h"

//=======================================================
//=======================================================
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
    OSDFont,
    Search,
    GSTxPower,
    GSTxInterface, 
    Image,
    CameraStopCH
};

//=======================================================
//=======================================================
class OSDMenu
{
public:
    OSDMenu();

    bool visible;

    void init();
    void draw( Ground2Air_Config_Packet& config );

private:

    OSDMenuId menuId;
    int selectedItem;
    int itemsCount;
    int keyHandled;

    std::vector<OSDMenuId> backMenuIds;
    std::vector<int> backMenuItems;

    Clock::time_point search_tp = Clock::now();
    bool searchDone;

    int bWidth;
    int sWidth;
    int bHeight;

    void drawMenuTitle( const char* caption );
    bool drawMenuItem( const char* caption, int itemIndex, bool clip = false);
    void drawStatus( const char* caption );

    bool exitKeyPressed();

    void goForward(OSDMenuId newMenuId, int newItem);
    void goBack();

    void drawMainMenu(Ground2Air_Config_Packet& config);
    void drawCameraSettingsMenu(Ground2Air_Config_Packet& config);
    void drawResolutionMenu(Ground2Air_Config_Packet& config);
    void drawBrightnessMenu(Ground2Air_Config_Packet& config);
    void drawContrastMenu(Ground2Air_Config_Packet& config);
    void drawExposureMenu(Ground2Air_Config_Packet& config);
    void drawSaturationMenu(Ground2Air_Config_Packet& config);
    void drawSharpnessMenu(Ground2Air_Config_Packet& config);
    void drawExitToShellMenu(Ground2Air_Config_Packet& config);
    void drawLetterboxMenu(Ground2Air_Config_Packet& config);
    void drawWifiRateMenu(Ground2Air_Config_Packet& config);
    void drawWifiChannelMenu(Ground2Air_Config_Packet& config);
    void drawRestartMenu(Ground2Air_Config_Packet& config);
    void drawFECMenu(Ground2Air_Config_Packet& config);
    void drawGSSettingsMenu(Ground2Air_Config_Packet& config);
    void drawOSDFontMenu(Ground2Air_Config_Packet& config);
    void drawSearchMenu(Ground2Air_Config_Packet& config);
    void drawGSTxPowerMenu(Ground2Air_Config_Packet& config);
    void drawGSTxInterfaceMenu(Ground2Air_Config_Packet& config);
    void searchNextWifiChannel(Ground2Air_Config_Packet& config);
    void drawImageSettingsMenu(Ground2Air_Config_Packet& config);
    void drawCameraStopCHMenu(Ground2Air_Config_Packet& config);
};

extern OSDMenu g_osdMenu;
