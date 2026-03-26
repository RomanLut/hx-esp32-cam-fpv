#pragma once

#include "main.h"
#include "imgui.h"
#include "packets.h"
#include "core/osd_menu_common.h"
#include "core/osd_menu_imgui_shared.h"
#include "core/osd_menu_platform.h"

namespace gs::menu
{

struct OSDMenuSnapshot
{
    bool visible = false;
    std::string title;
    std::vector<std::string> items;
    std::vector<int> item_indices;
    std::vector<std::string> statuses;
    int selected_item = 0;
};

class OSDMenuController
{
public:
    explicit OSDMenuController(IOSDMenuPlatform& platform);

    bool visible;

    void init();
    void draw( Ground2Air_Config_Packet& config );
    void open();
    void close();
    bool isVisible() const;
    void moveSelection(int delta);
    void goBackPublic();
    void activateSelected(Ground2Air_Config_Packet& config);
    void activateItemByVisibleIndex(Ground2Air_Config_Packet& config, int visible_index);
    OSDMenuSnapshot buildSnapshot(Ground2Air_Config_Packet& config);

private:
    IOSDMenuPlatform& m_platform;
    bool m_capture_snapshot = false;
    bool m_replay_activation = false;
    int m_replay_item_index = -1;
    OSDMenuSnapshot m_snapshot;

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
    gs::menu::imgui::MenuFrameLayout m_imgui_layout;
    void drawMenuTitle( const char* caption );
    bool drawMenuItem( const char* caption, int itemIndex, bool clip = false);
    void drawStatus( const char* caption );
    void drawSpacing();
    void drawLargeGapIfTallScreen();

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
    void drawGSWifiSettingsMenu(Ground2Air_Config_Packet& config);
    void drawGSScreenMenu(Ground2Air_Config_Packet& config);
    void drawOSDFontMenu(Ground2Air_Config_Packet& config);
    void drawSearchMenu(Ground2Air_Config_Packet& config);
    void drawGSTxPowerMenu(Ground2Air_Config_Packet& config);
    void drawGSTxInterfaceMenu(Ground2Air_Config_Packet& config);
    void searchNextWifiChannel(Ground2Air_Config_Packet& config);
    void drawImageSettingsMenu(Ground2Air_Config_Packet& config);
    void drawCameraStopCHMenu(Ground2Air_Config_Packet& config);
    void drawDebugMenu(Ground2Air_Config_Packet& config);
    void drawCurrentMenu(Ground2Air_Config_Packet& config);
    void replayItemActivation(Ground2Air_Config_Packet& config, int item_index);
};

} // namespace gs::menu
