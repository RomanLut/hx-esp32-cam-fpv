#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "../../../components/common/Clock.h"
#include "imgui.h"
#include "packets.h"
#include "core/osd_menu_common.h"
#include "core/osd_menu_imgui_shared.h"
#include "core/transport_kind.h"
#include "gs_recordings_storage.h"
#include "gs_shared_state.h"

namespace gs::menu
{

//===================================================================================
//===================================================================================
// Stores the last menu content rendered by the controller for debug and MCP reads.
struct CapturedMenuBuffer
{
    bool visible = false;
    OSDMenuId menu_id = OSDMenuId::Main;
    int selected_item = 0;
    std::string title;
    std::vector<std::string> lines;
};

//===================================================================================
//===================================================================================
// Draws and tracks the GS on-screen menu state and navigation.
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
    void openPlaybackMenu();
    void close();
    bool isVisible() const;
    OSDMenuId currentMenuId() const { return menuId; }
    CapturedMenuBuffer copyCapturedMenuBuffer() const;
    bool tryGetImageStabilizationRoiOverlaySettings(float& out_roi_divisor) const;

private:
    enum class DrawMode
    {
        Interactive,
        Passive,
    };

    DrawMode m_draw_mode = DrawMode::Interactive;

    OSDMenuId menuId = OSDMenuId::Main;
    int selectedItem = 0;
    int itemsCount = 0;
    int keyHandled = false;

    std::vector<OSDMenuId> backMenuIds;
    std::vector<int> backMenuItems;

    Clock::time_point search_tp = Clock::now();
    bool searchDone = false;
    LensCorrectionState m_lens_correction_original = {};
    LensCorrectionState m_lens_correction_draft = {};
    bool m_lens_correction_draft_active = false;
    Clock::time_point m_lens_correction_last_adjust_tp = {};
    Clock::time_point m_lens_correction_repeat_start_tp = {};
    int m_lens_correction_repeat_item = -1;
    int m_lens_correction_repeat_direction = 0;
    ImageStabilizationState m_image_stabilization_original = {};
    ImageStabilizationState m_image_stabilization_draft = {};
    bool m_image_stabilization_draft_active = false;
    Clock::time_point m_image_stabilization_last_adjust_tp = {};
    Clock::time_point m_image_stabilization_repeat_start_tp = {};
    int m_image_stabilization_repeat_item = -1;
    int m_image_stabilization_repeat_direction = 0;

    gs::menu::imgui::MenuFrameLayout m_imgui_layout;

    bool m_has_clip_items = false;
    bool m_clip_y_started = false;
    float m_clip_y_start = 0.0f;
    float m_clip_y_end = 0.0f;
    float m_clip_item_right_x = 0.0f;
    int m_clip_total_items = 0;
    mutable std::mutex m_capture_mutex;
    CapturedMenuBuffer m_captured_menu_buffer = {};
    void resetCapturedMenuBuffer();
    void appendCapturedLine(const std::string& line);
    static std::string sanitizeCapturedCaption(const char* caption);
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
    static bool isMenuAdjustIncreasePressed();
    static bool isMenuAdjustDecreasePressed();
    static bool isMenuUpPressed();
    static bool isMenuDownPressed();
    static bool isMenuOpenPressed();
    static bool isMenuRightClickPressed();
    bool exitKeyPressed();
    int getLensCorrectionStepMultiplier(int item_index, int direction);
    void resetLensCorrectionStepMultiplier();
    int getImageStabilizationStepMultiplier(int item_index, int direction);
    void resetImageStabilizationStepMultiplier();
    static gs::core::TransportKind getTransportKindForMenuIndex(int menu_index);

    void goForward(OSDMenuId newMenuId, int newItem);
    void goBack();

    void drawMainMenu(Ground2Air_Config_Packet& config);
    void drawCameraSettingsMenu(Ground2Air_Config_Packet& config);
    void drawCameraModeMenu(Ground2Air_Config_Packet& config);
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
    void drawGSPostprocessingMenu(Ground2Air_Config_Packet& config);
    void drawGSVRModeMenu(Ground2Air_Config_Packet& config);
    void drawGSImageStabilizationMenu(Ground2Air_Config_Packet& config);
    void drawGSImageStabilizationParametersMenu(Ground2Air_Config_Packet& config);
    void drawGSLensCorrectionMenu(Ground2Air_Config_Packet& config);
    void drawGSLensCorrectionCoefficientsMenu(Ground2Air_Config_Packet& config);
    void drawOSDFontMenu(Ground2Air_Config_Packet& config);
    void drawConnectMenu(Ground2Air_Config_Packet& config);
    void drawGSTxPowerMenu(Ground2Air_Config_Packet& config);
    void drawGSTxInterfaceMenu(Ground2Air_Config_Packet& config);
    void drawGSApfpvInterfaceMenu(Ground2Air_Config_Packet& config);
    void drawImageSettingsMenu(Ground2Air_Config_Packet& config);
    void drawCameraRCMenu(Ground2Air_Config_Packet& config);
    void drawCameraStopCHMenu(Ground2Air_Config_Packet& config);
    void drawImageStabilizationCHMenu(Ground2Air_Config_Packet& config);
    void drawDebugMenu(Ground2Air_Config_Packet& config);
    void drawPlaybackMenu(Ground2Air_Config_Packet& config);
    void drawPlaybackRunMenu(Ground2Air_Config_Packet& config);
    void drawPlaybackDeleteMenu(Ground2Air_Config_Packet& config);

    int m_playback_delete_index = 0;
    std::string m_playback_delete_path;
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
