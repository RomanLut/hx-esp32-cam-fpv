#pragma once

#include <vector>

#include "Clock.h"
#include "imgui.h"
#include "packets.h"
#include "core/osd_menu_common.h"
#include "core/osd_menu_imgui_shared.h"
#include "core/transport_kind.h"
#include "gs_recordings_storage.h"
#include "gs_shared_state.h"

namespace gs::menu
{

class OSDMenuController
{
public:
    OSDMenuController();

    static int getTransportModeMenuIndex(gs::core::TransportKind kind);
    static int getVisibleScreenAspectSelection(ScreenAspectRatio ratio);
    static const char* getVisibleScreenAspectLabel(ScreenAspectRatio ratio);
    static gs::menu::imgui::MenuFrameLayout offsetMenuLayout(const gs::menu::imgui::MenuFrameLayout& layout, float origin_x);

    bool visible;

    void init();
    void draw( Ground2Air_Config_Packet& config );
    void open();
    void close();
    bool isVisible() const;

private:
    enum class DrawMode
    {
        Interactive,
        Passive,
    };

    DrawMode m_draw_mode = DrawMode::Interactive;

    OSDMenuId menuId;
    int selectedItem;
    int itemsCount;
    int keyHandled;

    std::vector<OSDMenuId> backMenuIds;
    std::vector<int> backMenuItems;

    Clock::time_point search_tp = Clock::now();
    bool searchDone;

    gs::menu::imgui::MenuFrameLayout m_imgui_layout;
    void drawMenuTitle( const char* caption );
    bool drawMenuItem( const char* caption, int itemIndex, bool clip = false);
    void drawStatus( const char* caption );
    void drawSpacing();
    void drawLargeGapIfTallScreen();
    std::string formatAirStorageStatusLine(bool detected,
                                           bool error,
                                           bool slow,
                                           uint16_t free_space_gb16,
                                           uint16_t total_space_gb16,
                                           const char* detected_label = "Ok",
                                           const char* missing_label = "?",
                                           const char* trailing_suffix = "") const;
    std::string formatGroundStorageStatusLine(const GroundStorageStatus& status) const;

    static bool isMenuItemActivatePressed();
    static bool isMenuOpenPressed();
    static bool isMenuRightClickPressed();
    bool exitKeyPressed();
    static gs::core::TransportKind getTransportKindForMenuIndex(int menu_index);

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
    void drawConnectMenu(Ground2Air_Config_Packet& config);
    void drawGSTxPowerMenu(Ground2Air_Config_Packet& config);
    void drawGSTxInterfaceMenu(Ground2Air_Config_Packet& config);
    void drawImageSettingsMenu(Ground2Air_Config_Packet& config);
    void drawCameraStopCHMenu(Ground2Air_Config_Packet& config);
    void drawDebugMenu(Ground2Air_Config_Packet& config);
    void drawSearchModeMenu(Ground2Air_Config_Packet& config);
    void drawSearchRunMenu(Ground2Air_Config_Packet& config);
    void drawCurrentMenu(Ground2Air_Config_Packet& config);
    void drawMenuWindow(const char* window_name,
                        const gs::menu::imgui::MenuFrameLayout& layout,
                        Ground2Air_Config_Packet& config,
                        DrawMode mode,
                        ImGuiWindowFlags extra_flags = 0);
};

extern OSDMenuController g_osdMenuController;

} // namespace gs::menu
