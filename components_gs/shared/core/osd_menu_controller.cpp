#include "core/osd_menu_controller.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include "util.h"
#include "flight_osd.h"
#include "lodepng.h"
#include "gs_recordings_storage.h"
#include "gs_playback_manager.h"
#include "gs_runtime_platform_services.h"
#include "gs_shared_runtime.h"
#include "gs_runtime_config.h"
#include "core/transport_manager.h"
#include "core/transport_manager_base.h"
#include "frame_packets_debug.h"
#include "gs_runtime_core.h"
#include "gs_runtime_osd_font_storage.h"
#include "gs_runtime_state.h"

#define SEARCH_TIME_STEP_MS 1000

namespace
{
constexpr float kScreenZoomStep = 0.01f;
constexpr float kScreenZoomStepScale = 100.0f;
constexpr float kScreenVrSeparationMin = -0.30f;
constexpr float kScreenVrSeparationMax = 0.30f;
constexpr float kScreenVrSeparationStep = 0.02f;
constexpr float kScreenVrSeparationStepScale = 50.0f;
constexpr float kScreenVrSeparationDisplayScale = 100.0f;
constexpr double kLensCorrectionRadialStep = 0.00001;
constexpr double kLensCorrectionTangentialStep = 0.000001;
constexpr double kLensCorrectionDisplayScale = 1000000.0;
constexpr int kLensCorrectionRepeatResetMs = 200;
constexpr int kLensCorrectionAccelerationStepMs = 3000;
constexpr int kLensCorrectionMaxStepMultiplier = 256;
}

using OSDMenuController = gs::menu::OSDMenuController;

namespace gs::menu
{

OSDMenuController g_osdMenuController;

}

namespace
{
//===================================================================================
//===================================================================================
// Builds one storage status line from already formatted label, state, and capacities.
std::string formatStorageLine(const char* label,
                              const char* status,
                              double free_gb,
                              double total_gb,
                              const char* suffix = "")
{
    std::ostringstream out;
    out << label << ": " << status;
    if (suffix != nullptr && suffix[0] != 0)
    {
        out << suffix;
    }
    out << ' ' << std::fixed << std::setprecision(2) << free_gb << "GB/" << total_gb << "GB";
    return out.str();
}

//===================================================================================
//===================================================================================
// Returns a coefficient rounded to the displayed six-decimal precision.
double roundLensCorrectionCoefficient(double value)
{
    return std::round(value * kLensCorrectionDisplayScale) / kLensCorrectionDisplayScale;
}
}

//===================================================================================
//===================================================================================
// Calculates the accelerated lens coefficient step multiplier for repeated input.
int OSDMenuController::getLensCorrectionStepMultiplier(int item_index, int direction)
{
    const Clock::time_point now = Clock::now();
    const bool same_repeat =
        this->m_lens_correction_last_adjust_tp != Clock::time_point{} &&
        this->m_lens_correction_repeat_item == item_index &&
        this->m_lens_correction_repeat_direction == direction &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - this->m_lens_correction_last_adjust_tp).count() < kLensCorrectionRepeatResetMs;

    if (!same_repeat)
    {
        this->m_lens_correction_repeat_start_tp = now;
        this->m_lens_correction_repeat_item = item_index;
        this->m_lens_correction_repeat_direction = direction;
        this->m_lens_correction_last_adjust_tp = now;
        return 1;
    }

    this->m_lens_correction_last_adjust_tp = now;
    const int acceleration_stage = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - this->m_lens_correction_repeat_start_tp).count() / kLensCorrectionAccelerationStepMs);
    return std::min(kLensCorrectionMaxStepMultiplier, 1 << std::min(acceleration_stage, 8));
}

//===================================================================================
//===================================================================================
// Resets lens coefficient repeat acceleration after a pause or menu transition.
void OSDMenuController::resetLensCorrectionStepMultiplier()
{
    this->m_lens_correction_last_adjust_tp = {};
    this->m_lens_correction_repeat_start_tp = {};
    this->m_lens_correction_repeat_item = -1;
    this->m_lens_correction_repeat_direction = 0;
}

//===================================================================================
//===================================================================================
//===================================================================================
//===================================================================================
// Formats the AIR SD status line from raw status flags and capacity values.
std::string OSDMenuController::formatAirStorageStatusLine(bool detected,
                                                          bool error,
                                                          bool slow,
                                                          uint16_t free_space_gb16,
                                                          uint16_t total_space_gb16,
                                                          const char* detected_label,
                                                          const char* missing_label,
                                                          const char* trailing_suffix) const
{
    const char* suffix = error ? " Error" : (slow ? " Slow" : "");
    std::string line = formatStorageLine(
        "AIR SD",
        detected && !error ? detected_label : missing_label,
        free_space_gb16 / 16.0,
        total_space_gb16 / 16.0,
        suffix);
    if (trailing_suffix != nullptr && trailing_suffix[0] != 0)
    {
        line += ' ';
        line += trailing_suffix;
    }
    return line;
}

//===================================================================================
//===================================================================================
// Formats the GS SD status line from the current ground storage snapshot.
std::string OSDMenuController::formatGroundStorageStatusLine(const GroundStorageStatus& status) const
{
    return formatStorageLine(
        "GS SD",
        status.free_space_bytes >= kGSMinFreeSpaceBytes ? "Ok" : "Low space",
        static_cast<double>(status.free_space_bytes) / (1024.0 * 1024.0 * 1024.0),
        static_cast<double>(status.total_space_bytes) / (1024.0 * 1024.0 * 1024.0));
}

//===================================================================================
//===================================================================================
// Constructor - initializes the OSD menu controller with hidden state.
OSDMenuController::OSDMenuController()
{
    this->visible = false;
    resetCapturedMenuBuffer();
}

//===================================================================================
//===================================================================================
// Maps a transport kind to the corresponding mode-menu item index.
int OSDMenuController::getTransportModeMenuIndex(gs::core::TransportKind kind)
{
    switch (kind)
    {
    case gs::core::TransportKind::RawBroadcast:
        return 0;

    case gs::core::TransportKind::APFPV:
        return 1;

    case gs::core::TransportKind::TestTransport:
        return 2;

    case gs::core::TransportKind::WifiChannelScan:
        return 3;
    }

    return 0;
}

//===================================================================================
//===================================================================================
// Maps a mode-menu item index to the corresponding transport kind.
gs::core::TransportKind OSDMenuController::getTransportKindForMenuIndex(int menu_index)
{
    switch (menu_index)
    {
    case 0:
        return gs::core::TransportKind::RawBroadcast;

    case 1:
        return gs::core::TransportKind::APFPV;

    case 2:
        return gs::core::TransportKind::TestTransport;

    case 3:
        return gs::core::TransportKind::WifiChannelScan;

    default:
        return gs::core::TransportKind::RawBroadcast;
    }
}

//===================================================================================
//===================================================================================
// Offsets a menu layout horizontally while preserving its other metrics.
gs::menu::imgui::MenuFrameLayout OSDMenuController::offsetMenuLayout(const gs::menu::imgui::MenuFrameLayout& layout, float origin_x)
{
    gs::menu::imgui::MenuFrameLayout shifted = layout;
    shifted.window_x += origin_x;
    return shifted;
}

//===================================================================================
//===================================================================================
// Maps the configured screen aspect ratio to the visible menu selection index.
int OSDMenuController::getVisibleScreenAspectSelection(ScreenAspectRatio ratio)
{
    if (s_RuntimePlatformServices != nullptr && !s_RuntimePlatformServices->supportsCustomScreenAspectModes())
    {
        return ratio == ScreenAspectRatio::STRETCH ? 0 : 1;
    }
    return clamp(static_cast<int>(ratio), 0, 5);
}

//===================================================================================
//===================================================================================
// Returns true when the current frame contains a menu activate action.
bool OSDMenuController::isMenuItemActivatePressed()
{
    return ImGui::IsKeyPressed(ImGuiKey_Enter) ||
           ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
           ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
           ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false);
}

//===================================================================================
//===================================================================================
// Returns true when the current frame contains an increase action for an inline value.
bool OSDMenuController::isMenuAdjustIncreasePressed()
{
    // Keep ImGui repeat enabled here. Adjustable rows such as screen zoom must
    // consume held arrow repeats before the generic back handler sees Left.
    return ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
           ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight);
}

//===================================================================================
//===================================================================================
// Returns true when the current frame contains a decrease action for an inline value.
bool OSDMenuController::isMenuAdjustDecreasePressed()
{
    // Keep ImGui repeat enabled here. Adjustable rows such as screen zoom must
    // consume held arrow repeats before the generic back handler sees Left.
    return ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
           ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft);
}

//===================================================================================
//===================================================================================
// Returns true when the current frame contains an upward navigation action.
bool OSDMenuController::isMenuUpPressed()
{
    return ImGui::IsKeyPressed(ImGuiKey_UpArrow) ||
           ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp);
}

//===================================================================================
//===================================================================================
// Returns true when the current frame contains a downward navigation action.
bool OSDMenuController::isMenuDownPressed()
{
    return ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
           ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown);
}

//===================================================================================
//===================================================================================
// Returns true when the current frame contains a menu open action.
bool OSDMenuController::isMenuOpenPressed()
{
    return ImGui::IsKeyPressed(ImGuiKey_Enter) ||
           ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
}

//===================================================================================
//===================================================================================
// Returns true when the current frame contains a right mouse click.
bool OSDMenuController::isMenuRightClickPressed()
{
    return ImGui::IsMouseClicked(ImGuiMouseButton_Right, false);
}

//===================================================================================
//===================================================================================
// Returns the user-facing label for the current screen aspect ratio selection.
const char* OSDMenuController::getVisibleScreenAspectLabel(ScreenAspectRatio ratio)
{
    if (s_RuntimePlatformServices != nullptr && !s_RuntimePlatformServices->supportsCustomScreenAspectModes())
    {
        return ratio == ScreenAspectRatio::STRETCH ? "Stretch" : "Letterbox";
    }

    const char* modes[] = {"Stretch", "Letterbox", "Screen is 5:4", "Screen is 4:3", "Screen is 16:9", "Screen is 16:10"};
    return modes[clamp(static_cast<int>(ratio), 0, 5)];
}

//===================================================================================
//===================================================================================
// Opens the OSD menu, navigating to the main menu with the first item selected.
void OSDMenuController::open()
{
    this->visible = true;
    this->menuId = OSDMenuId::Main;
    this->selectedItem = 0;
    this->backMenuIds.clear();
    this->backMenuItems.clear();
}

//===================================================================================
//===================================================================================
// Opens the Playback menu without changing its current selected file.
void OSDMenuController::openPlaybackMenu()
{
    this->visible = true;
    this->menuId = OSDMenuId::Playback;
    this->backMenuIds.clear();
    this->backMenuItems.clear();
    this->backMenuIds.push_back(OSDMenuId::GSSettings);
    this->backMenuItems.push_back(0);
}

//===================================================================================
//===================================================================================
// Closes the OSD menu.
void OSDMenuController::close()
{
    this->visible = false;
    this->m_lens_correction_draft_active = false;
    resetCapturedMenuBuffer();
}

//===================================================================================
//===================================================================================
// Returns true if the OSD menu is currently visible.
bool OSDMenuController::isVisible() const
{
    return this->visible;
}

//===================================================================================
//===================================================================================
// Returns a thread-safe copy of the last menu buffer captured during rendering.
gs::menu::CapturedMenuBuffer OSDMenuController::copyCapturedMenuBuffer() const
{
    std::lock_guard<std::mutex> lock(m_capture_mutex);
    return m_captured_menu_buffer;
}

//===================================================================================
//===================================================================================
// Clears the cached rendered menu lines before capturing a new interactive frame.
void OSDMenuController::resetCapturedMenuBuffer()
{
    std::lock_guard<std::mutex> lock(m_capture_mutex);
    m_captured_menu_buffer.visible = visible;
    m_captured_menu_buffer.menu_id = menuId;
    m_captured_menu_buffer.selected_item = selectedItem;
    m_captured_menu_buffer.title.clear();
    m_captured_menu_buffer.lines.clear();
}

//===================================================================================
//===================================================================================
// Appends one sanitized line to the cached rendered menu buffer.
void OSDMenuController::appendCapturedLine(const std::string& line)
{
    std::lock_guard<std::mutex> lock(m_capture_mutex);
    m_captured_menu_buffer.lines.push_back(line);
}

//===================================================================================
//===================================================================================
// Strips the hidden ImGui identifier suffix from a menu caption before exposing it.
std::string OSDMenuController::sanitizeCapturedCaption(const char* caption)
{
    if (caption == nullptr)
    {
        return {};
    }

    std::string sanitized = caption;
    const size_t id_separator = sanitized.find("##");
    if (id_separator != std::string::npos)
    {
        sanitized.erase(id_separator);
    }
    return sanitized;
}

//===================================================================================
//===================================================================================
// Draws the menu title bar and refreshes interactive capture/key tracking state.
void OSDMenuController::drawMenuTitle( const char* caption )
{
    gs::menu::imgui::drawMenuTitle(caption, m_imgui_layout);
    this->itemsCount = 0;
    if (m_draw_mode == DrawMode::Interactive)
    {
        this->keyHandled = false;
        std::lock_guard<std::mutex> lock(m_capture_mutex);
        m_captured_menu_buffer.visible = visible;
        m_captured_menu_buffer.menu_id = menuId;
        m_captured_menu_buffer.selected_item = selectedItem;
        m_captured_menu_buffer.title = sanitizeCapturedCaption(caption);
    }
    drawLargeGapIfTallScreen();
}

//===================================================================================
//===================================================================================
// Draws a status text line in the menu.
void OSDMenuController::drawStatus( const char* caption )
{
    gs::menu::imgui::drawMenuStatus(caption, m_imgui_layout);
    if (m_draw_mode == DrawMode::Interactive)
    {
        appendCapturedLine("    " + sanitizeCapturedCaption(caption));
    }
}

//===================================================================================
//===================================================================================
// Draws a small vertical gap between menu items.
void OSDMenuController::drawSpacing()
{
    gs::menu::imgui::drawSmallGap(m_imgui_layout);
}

//===================================================================================
//===================================================================================
// Draws a large vertical gap, used on tall screens to pad menu content.
void OSDMenuController::drawLargeGapIfTallScreen()
{
    gs::menu::imgui::drawLargeGap(m_imgui_layout);
}

//===================================================================================
//===================================================================================
// Draws a single menu item and returns true if it was activated this frame.
bool OSDMenuController::drawMenuItem( const char* caption, int itemIndex, bool clip )
{
    int d = itemIndex - this->selectedItem;
    int d1 = 2;
    int d2 = -2;
    if ( this->selectedItem == 0) d1+=2;
    if ( this->selectedItem == 1) d1+=1;
    if (clip)
    {
        m_has_clip_items = true;
        m_clip_total_items++;
        if ((d > d1) || (d < d2))
            return false;

        const ImVec2 cursor_screen = ImGui::GetCursorScreenPos();
        if (!m_clip_y_started)
        {
            m_clip_y_start = cursor_screen.y;
            m_clip_item_right_x = cursor_screen.x + m_imgui_layout.item_indent + m_imgui_layout.item_width;
            m_clip_y_started = true;
        }
    }

    bool focused = this->selectedItem == itemIndex;
    if (m_draw_mode == DrawMode::Interactive)
    {
        appendCapturedLine(std::string(focused ? "[*] " : "[ ] ") + sanitizeCapturedCaption(caption));
    }
    bool res = false;

    if (m_draw_mode == DrawMode::Interactive)
    {
        res = focused && isMenuItemActivatePressed() && !this->keyHandled;
    }

    if (gs::menu::imgui::drawMenuItem(caption, m_imgui_layout, focused))
    {
        res = true;
    }

    if (clip)
    {
        m_clip_y_end = ImGui::GetCursorScreenPos().y;
    }

    this->itemsCount = std::max(this->itemsCount, itemIndex + 1);

    this->keyHandled |= res;

    if ( res )
    {
        this->selectedItem = itemIndex;
    }

    return res;
}

//===================================================================================
//===================================================================================
// Draws the entire menu in an ImGui window using the given layout and mode.
void OSDMenuController::drawMenuWindow(const char* window_name,
                                       const gs::menu::imgui::MenuFrameLayout& layout,
                                       Ground2Air_Config_Packet& config,
                                       DrawMode mode,
                                       ImGuiWindowFlags extra_flags)
{
    m_imgui_layout = layout;

    this->itemsCount = 0;
    if (mode == DrawMode::Interactive)
    {
        this->keyHandled = false;
    }
    this->m_draw_mode = mode;
    if (mode == DrawMode::Interactive)
    {
        resetCapturedMenuBuffer();
    }

    m_has_clip_items = false;
    m_clip_y_started = false;
    m_clip_y_start = 0.0f;
    m_clip_y_end = 0.0f;
    m_clip_item_right_x = 0.0f;
    m_clip_total_items = 0;

    gs::menu::imgui::beginMenuWindow(window_name, m_imgui_layout, extra_flags);
    drawCurrentMenu(config);
    if (m_has_clip_items && m_clip_y_started && m_clip_total_items > 5)
    {
        const float sb_x = m_clip_item_right_x + 12.0f * m_imgui_layout.scale;
        const float sb_width = 16.0f * m_imgui_layout.scale; 
        const float sb_height = 5.0f * (m_imgui_layout.button_height + m_imgui_layout.item_gap_y) - m_imgui_layout.item_gap_y;
        gs::menu::imgui::drawScrollbar(sb_x, m_clip_y_start, sb_height,
                                       this->selectedItem, m_clip_total_items, 5, sb_width);
    }
    gs::menu::imgui::endMenuWindow();

    this->m_draw_mode = DrawMode::Interactive;
    if (mode == DrawMode::Interactive)
    {
        std::lock_guard<std::mutex> lock(m_capture_mutex);
        m_captured_menu_buffer.visible = visible;
        m_captured_menu_buffer.menu_id = menuId;
        m_captured_menu_buffer.selected_item = selectedItem;
    }
}

//===================================================================================
//===================================================================================
// Main draw entry point; handles menu open/close logic and draws one canonical UI copy.
void OSDMenuController::draw(Ground2Air_Config_Packet& config)
{
    if (!this->visible)
    {
        const bool playback_active = s_playbackManager != nullptr && s_playbackManager->status().active;
        // Linux draws the menu before its render hotkeys are consumed, so active
        // playback owns Enter for 0x/1x speed toggling and must not open Main.
        if (playback_active && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
        {
            return;
        }

        if (isMenuOpenPressed() || isMenuRightClickPressed())
        {
            this->visible = true;
            this->menuId = OSDMenuId::Main;
            this->selectedItem = 0;
            return;
        }
        else
        {
            return;
        }
    }

    ImVec2 screenSize = ImGui::GetIO().DisplaySize;
    const bool vr_mode = s_groundstation_config.vrMode;
    const float primary_width = vr_mode ? (screenSize.x * 0.5f) : screenSize.x;
    const auto primary_layout = offsetMenuLayout(
        gs::menu::imgui::buildMenuFrameLayout(primary_width, screenSize.y, true, 29.0f),
        0.0f);

    const OSDMenuId drawn_menu_id = this->menuId;
    drawMenuWindow("OSD_MENU", primary_layout, config, DrawMode::Interactive);
    if (this->menuId == drawn_menu_id && this->itemsCount > 0)
    {
        this->selectedItem = std::clamp(this->selectedItem, 0, this->itemsCount - 1);
    }

    if ( isMenuUpPressed() && this->selectedItem > 0 )
    {
        this->selectedItem--;
    }

    if ( isMenuDownPressed() && this->selectedItem < (this->itemsCount - 1) )
    {
        this->selectedItem++;
    }
}

//===================================================================================
//===================================================================================
// Dispatches rendering to the appropriate menu page based on the current menu ID.
void OSDMenuController::drawCurrentMenu(Ground2Air_Config_Packet& config)
{
    switch (this->menuId)
    {
        case OSDMenuId::Main: this->drawMainMenu(config); break;
        case OSDMenuId::CameraSettings: this->drawCameraSettingsMenu(config); break;
        case OSDMenuId::CameraMode: this->drawCameraModeMenu(config); break;
        case OSDMenuId::Resolution: this->drawResolutionMenu(config); break;
        case OSDMenuId::Brightness: this->drawBrightnessMenu(config); break;
        case OSDMenuId::Contrast: this->drawContrastMenu(config); break;
        case OSDMenuId::Exposure: this->drawExposureMenu(config); break;
        case OSDMenuId::Saturation: this->drawSaturationMenu(config); break;
        case OSDMenuId::Sharpness: this->drawSharpnessMenu(config); break;
        case OSDMenuId::ExitToShell: this->drawExitToShellMenu(config); break;
        case OSDMenuId::Letterbox: this->drawLetterboxMenu(config); break;
        case OSDMenuId::WifiRate: this->drawWifiRateMenu(config); break;
        case OSDMenuId::WifiChannel: this->drawWifiChannelMenu(config); break;
        case OSDMenuId::Restart: this->drawRestartMenu(config); break;
        case OSDMenuId::FEC: this->drawFECMenu(config); break;
        case OSDMenuId::GSSettings: this->drawGSSettingsMenu(config); break;
        case OSDMenuId::GSWifiSettings: this->drawGSWifiSettingsMenu(config); break;
        case OSDMenuId::GSScreen: this->drawGSScreenMenu(config); break;
        case OSDMenuId::GSLensCorrection: this->drawGSLensCorrectionMenu(config); break;
        case OSDMenuId::OSDFont: this->drawOSDFontMenu(config); break;
        case OSDMenuId::Search: this->drawConnectMenu(config); break;
        case OSDMenuId::SearchMode: this->drawSearchModeMenu(config); break;
        case OSDMenuId::SearchRun: this->drawSearchRunMenu(config); break;
        case OSDMenuId::GSTxPower: this->drawGSTxPowerMenu(config); break;
        case OSDMenuId::GSTxInterface: this->drawGSTxInterfaceMenu(config); break;
        case OSDMenuId::GSApfpvInterface: this->drawGSApfpvInterfaceMenu(config); break;
        case OSDMenuId::Image: this->drawImageSettingsMenu(config); break;
        case OSDMenuId::CameraStopCH: this->drawCameraStopCHMenu(config); break;
        case OSDMenuId::Debug: this->drawDebugMenu(config); break;
        case OSDMenuId::Playback: this->drawPlaybackMenu(config); break;
        case OSDMenuId::PlaybackRun: this->drawPlaybackRunMenu(config); break;
    }
}

//===================================================================================
//===================================================================================
// Draws the main top-level OSD menu with connection, resolution, and settings options.
void OSDMenuController::drawMainMenu(Ground2Air_Config_Packet& config)
{
    const auto& gs_config = s_groundstation_config;
    {
        char buf[256];
        sprintf( buf, "ESP32-CAM-FPV v%s.%d %s%s##title0", FW_VERSION, PACKET_VERSION, s_isDual ? "D " : "", s_isOV5640 ? "OV5640" : "OV2640");
        this->drawMenuTitle( buf );
    }

    {
        if ( this->drawMenuItem( "Search & Connect...", 0) )
        {
            this->goForward( OSDMenuId::Search, 0);
        }
    }

    {
        char buf[256];
        sprintf(buf, "Resolution: %s##0", gs::menu::getResolutionSummary(config, s_isOV5640).c_str());
        if ( this->drawMenuItem( buf, 1) )
        {
            int item = gs::menu::getResolutionMenuIndex(config.camera.resolution);
            this->goForward( OSDMenuId::Resolution, item );
        }
    }

    {
        char buf[256];
        int channel = getBandAwareWifiChannel(gs_config.wifi_channel, gs_config.wifiBand);
        sprintf(buf, "Wifi Channel: %d##1", channel);
        if ( this->drawMenuItem( buf, 2) )
        {
            int channelIndex = getBandAwareWifiChannelMenuIndex(channel, gs_config.wifiBand);
            this->goForward( OSDMenuId::WifiChannel, channelIndex);
        }
    }

    {
        char buf[256];
        int i = gs::menu::getWifiRateMenuIndex(config.dataChannel.wifi_rate);
        sprintf(buf, "Wifi Rate: %s##2", gs::menu::getWifiRateSummary(config).c_str());
        if ( this->drawMenuItem( buf, 3) )
        {
            this->goForward( OSDMenuId::WifiRate, i );
        }
    }

    {
        char buf[256];
        int i = gs::menu::getFecMenuIndex(config);
        sprintf(buf, "FEC: %s##3", gs::menu::getFecSummary(config).c_str());
        if ( this->drawMenuItem( buf, 4) )
        {
            this->goForward( OSDMenuId::FEC, i );
        }
    }

    if ( this->drawMenuItem( "Camera...", 5) )
    {
        this->goForward( OSDMenuId::CameraSettings, 0 );

        if ( s_isOV5640 && config.camera.vflip )
        {
            config.camera.vflip = false;
            commitGround2AirConfig(config);
        }
    }

    if ( this->drawMenuItem( "Ground Station...", 6) )
    {
        this->goForward( OSDMenuId::GSSettings, 0 );
    }

    //this->drawMenuItem( "OSD...", 5);

    drawLargeGapIfTallScreen();

    {
        std::string line = this->formatAirStorageStatusLine(
            s_SDDetected,
            s_SDError,
            s_SDSlow,
            s_SDFreeSpaceGB16,
            s_SDTotalSpaceGB16);
        line += "##status0";
        this->drawStatus(line.c_str());
    }

    {
        const auto gs_storage = s_recordingsStorage->groundStorageStatus();
        std::string line = this->formatGroundStorageStatusLine(gs_storage);
        line += "##status1";
        this->drawStatus(line.c_str());
    }

    if ( this->exitKeyPressed())
    {
        this->visible = false;
    }
}

//===================================================================================
//===================================================================================
// Draws the image settings sub-menu (brightness, contrast, exposure, etc.).
void OSDMenuController::drawImageSettingsMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Menu -> Image Settings" );

    {
        char buf[256];
        sprintf(buf, "Brightness: %d##0", config.camera.brightness);
        if ( this->drawMenuItem( buf, 0) )
        {
            this->goForward( OSDMenuId::Brightness, config.camera.brightness + 2 );
        }
    }

    {
        char buf[256];
        sprintf(buf, "Contrast: %d##1", config.camera.contrast);
        if ( this->drawMenuItem( buf, 1) )
        {
            this->goForward( OSDMenuId::Contrast, config.camera.contrast + 2 );
        }
    }

    {
        char buf[256];
        sprintf(buf, "Exposure: %d##2", config.camera.ae_level);
        if ( this->drawMenuItem( buf, 2) )
        {
            this->goForward( OSDMenuId::Exposure, config.camera.ae_level + 2);
        }
    }

    {
        char buf[256];
        sprintf(buf, "Saturation: %d##3", config.camera.saturation);
        if ( this->drawMenuItem( buf, 3) )
        {
            this->goForward( OSDMenuId::Saturation, config.camera.saturation + 2 );
        }
    }

    {
        char buf[256];
        const char* sharpnessLevels[] = {"Blur more", "Blur", "Normal", "Sharpen", "Sharpen more"};
        sprintf(buf, "Sharpness: %s##4", sharpnessLevels[clamp((int)config.camera.sharpness,-2,2)+2]);
        if ( this->drawMenuItem( buf, 4) )
        {
            this->goForward( OSDMenuId::Sharpness, config.camera.sharpness + 2 );
        }
    }

    if (!s_isOV5640)  //vertical flip drops framerate by half, useless
    {
        if ( this->drawMenuItem( config.camera.vflip ? "Vertical Flip: Enabled##5" : "Vertical Flip: Disabled##5", 5) )
        {
            config.camera.vflip = !config.camera.vflip;
            config.camera.hmirror = config.camera.vflip;
            commitGround2AirConfig(config);
        }

        if ( this->drawMenuItem( config.camera.ov2640HighFPS ? "40fps (overclock): Enabled##6" : "40FPS (overclock): Disabled##5", 6) )
        {
            config.camera.ov2640HighFPS = !config.camera.ov2640HighFPS;
            commitGround2AirConfig(config);
        }
    }
    else
    {
        if ( this->drawMenuItem( config.camera.ov5640HighFPS ? "50fps Modes: Enabled##6" : "50fps Modes: Disabled##5", 5) )
        {
            config.camera.ov5640HighFPS = !config.camera.ov5640HighFPS;
            commitGround2AirConfig(config);
        }
    }

    if ( this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the camera settings sub-menu, including the air-unit transport mode flag.
void OSDMenuController::drawCameraSettingsMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Menu -> Camera Settings" );

    {
        char buf[256];
        sprintf(buf, "Mode: %s##0", config.misc.apfpv != 0 ? "APFPV" : "RAW Broadcast");
        if ( this->drawMenuItem( buf, 0 ) )
        {
            this->goForward( OSDMenuId::CameraMode, config.misc.apfpv != 0 ? 1 : 0 );
        }
    }

    {
        if ( this->drawMenuItem( "Image Settings...", 1 ) )
        {
            this->goForward( OSDMenuId::Image, 0 );
        }
    }

    {
        char buf[512];
        sprintf(buf, "OSD Font: %s", s_flightOSD.currentFontName().c_str());
        if (strlen(buf) > 30 )
        {
            buf[28]='.'; buf[29]='.'; buf[30]='.'; buf[31]=0;
        }
        strcat(buf, "##1");
        if ( this->drawMenuItem( buf, 2) )
        {
            const auto& fonts = s_OSDFontStorage->osdFontsList();
            auto it = std::find(fonts.begin(), fonts.end(), s_flightOSD.currentFontName());
            this->goForward( OSDMenuId::OSDFont, it != fonts.end() ? static_cast<int>(std::distance(fonts.begin(), it)) : 0 );
        }
    }


    {
        char buf[256];
        sprintf(buf, "Autostart recording: %s", config.misc.autostartRecord == 1? "On" : "Off");
        if ( this->drawMenuItem( buf, 3) )
        {
            config.misc.autostartRecord ^= 1;

        }
    }

    {
        char buf[256];
        if ( config.misc.cameraStopChannel == 0 )
        {
            sprintf(buf, "Camera Off RC Channel: None" );
        }
        else
        {
            sprintf(buf, "Camera Off RC Channel: %d", (int)config.misc.cameraStopChannel );
        }
        if ( this->drawMenuItem( buf, 4) )
        {
            this->goForward( OSDMenuId::CameraStopCH, (int)config.misc.cameraStopChannel );
        }
    }

    {
        char buf[256];
        sprintf(buf, "Mavlink2 to Msp RC: %s", config.misc.mavlink2mspRC == 1? "On" : "Off");
        if ( this->drawMenuItem( buf, 5) )
        {
            config.misc.mavlink2mspRC ^= 1;

        }
    }

    {
        char buf[256];
        sprintf(buf, "Air to GS MTU: %d", config.dataChannel.fec_codec_mtu);
        if ( this->drawMenuItem( buf, 6) )
        {
            if ( config.dataChannel.fec_codec_mtu == AIR2GROUND_MAX_MTU )
            {
                config.dataChannel.fec_codec_mtu = AIR2GROUND_MIN_MTU;
            }
            else
            {
                config.dataChannel.fec_codec_mtu = AIR2GROUND_MAX_MTU;
            }
        }
    }


    if ( this->exitKeyPressed())
    {
        this->goBack();
    }

}

//===================================================================================
//===================================================================================
// Draws the camera mode submenu backed by the air config packet APFPV flag.
void OSDMenuController::drawCameraModeMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle("Camera Settings -> Mode");
    drawSpacing();

    if (this->drawMenuItem("RAW Broadcast", 0))
    {
        config.misc.apfpv = 0;
        commitGround2AirConfig(config);
        this->goBack();
        return;
    }

    if (this->drawMenuItem("APFPV", 1))
    {
        config.misc.apfpv = 1;
        commitGround2AirConfig(config);
        this->goBack();
        return;
    }

    if (this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the resolution selection menu.
void OSDMenuController::drawResolutionMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Menu -> Resolution" );
    drawSpacing();

    bool saveAndExit = false;

    if ( this->drawMenuItem( gs::menu::getResolutionOptionLabel(config, s_isOV5640, 0, true), 0) )
    {
        config.camera.resolution = Resolution::VGA16;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( gs::menu::getResolutionOptionLabel(config, s_isOV5640, 1, true), 1) )
    {
        config.camera.resolution = Resolution::VGA;
        saveAndExit = true;
    }


    if ( this->drawMenuItem( gs::menu::getResolutionOptionLabel(config, s_isOV5640, 2, true), 2) )
    {
        config.camera.resolution = Resolution::SVGA16;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( gs::menu::getResolutionOptionLabel(config, s_isOV5640, 3, true), 3) )
    {
        config.camera.resolution = Resolution::SVGA;
        saveAndExit = true;
    }

    if (this->drawMenuItem( gs::menu::getResolutionOptionLabel(config, s_isOV5640, 4, true), 4) )
    {
        config.camera.resolution = Resolution::XGA16;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( gs::menu::getResolutionOptionLabel(config, s_isOV5640, 5, true), 5) )
    {
        config.camera.resolution = Resolution::HD;
        saveAndExit = true;
    }

    if ( saveAndExit )
    {
        commitGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Returns true if the user pressed a back/exit key during an interactive frame.
bool OSDMenuController::exitKeyPressed()
{
    if (m_draw_mode != DrawMode::Interactive)
    {
        return false;
    }

    return ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
           ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) ||
           ImGui::IsKeyPressed(ImGuiKey_R) ||
           ImGui::IsKeyPressed(ImGuiKey_G) ||
           ImGui::IsKeyPressed(ImGuiKey_Escape) ||
           isMenuRightClickPressed();
}

//===================================================================================
//===================================================================================
// Draws the brightness selection menu.
void OSDMenuController::drawBrightnessMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Camera Settings -> Brightness" );
    drawSpacing();

    bool saveAndExit = false;

    if ( this->drawMenuItem( "-2", 0) )
    {
        config.camera.brightness = -2;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "-1", 1) )
    {
        config.camera.brightness = -1;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "0", 2) )
    {
        config.camera.brightness = 0;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "1", 3) )
    {
        config.camera.brightness = 1;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "2", 4) )
    {
        config.camera.brightness = 2;
        saveAndExit = true;
    }

    if ( saveAndExit )
    {
        commitGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->goBack();
    }
}


//===================================================================================
//===================================================================================
// Draws the contrast selection menu.
void OSDMenuController::drawContrastMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Camera Settings -> Contrast" );
    drawSpacing();

    bool saveAndExit = false;

    if ( this->drawMenuItem( "-2", 0) )
    {
        config.camera.contrast = -2;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "-1", 1) )
    {
        config.camera.contrast = -1;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "0", 2) )
    {
        config.camera.contrast = 0;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "1", 3) )
    {
        config.camera.contrast = 1;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "2", 4) )
    {
        config.camera.contrast = 2;
        saveAndExit = true;
    }

    if ( saveAndExit )
    {
        commitGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the exposure selection menu.
void OSDMenuController::drawExposureMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Camera Settings -> Exposure" );
    drawSpacing();

    bool saveAndExit = false;

    if ( this->drawMenuItem( "-2", 0) )
    {
        config.camera.ae_level = -2;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "-1", 1) )
    {
        config.camera.ae_level = -1;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "0", 2) )
    {
        config.camera.ae_level = 0;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "1", 3) )
    {
        config.camera.ae_level = 1;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "2", 4) )
    {
        config.camera.ae_level = 2;
        saveAndExit = true;
    }

    if ( saveAndExit )
    {
        commitGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the saturation selection menu.
void OSDMenuController::drawSaturationMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Camera Settings -> Saturation" );
    drawSpacing();

    bool saveAndExit = false;

    if ( this->drawMenuItem( "-2", 0) )
    {
        config.camera.saturation = -2;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "-1", 1) )
    {
        config.camera.saturation = -1;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "0", 2) )
    {
        config.camera.saturation = 0;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "1", 3) )
    {
        config.camera.saturation = 1;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "2", 4) )
    {
        config.camera.saturation = 2;
        saveAndExit = true;
    }

    if ( saveAndExit )
    {
        commitGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the sharpness selection menu.
void OSDMenuController::drawSharpnessMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Camera Settings -> Sharpness" );
    drawSpacing();

    bool saveAndExit = false;

    if ( this->drawMenuItem( "Blue more", 0) )
    {
        config.camera.sharpness = -2;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "Blur", 1) )
    {
        config.camera.sharpness = -1;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "Normal", 2) )
    {
        config.camera.sharpness = 0;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "Sharpen", 3) )
    {
        config.camera.sharpness = 1;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "Sharpen more", 4) )
    {
        config.camera.sharpness = 2;
        saveAndExit = true;
    }

    if ( saveAndExit )
    {
        commitGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the "Exit to Shell" confirmation menu.
void OSDMenuController::drawExitToShellMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Exit To Shell ?" );
    drawSpacing();

    if ( this->drawMenuItem( "Exit", 0) )
    {
        s_RuntimePlatformServices->exitApp();
    }

    bool b = false;
    if ( this->drawMenuItem( "Cancel", 1) )
    {
        b= true;
    }

    if ( b || this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the screen aspect ratio (letterbox) selection menu.
void OSDMenuController::drawLetterboxMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    this->drawMenuTitle( "GS Settings -> Letterbox" );
    drawSpacing();

    bool saveAndExit = false;

    if ( this->drawMenuItem( "Stretch", 0) )
    {
        gs_config.screenAspectRatio = ScreenAspectRatio::STRETCH;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "Letterbox", 1) )
    {
        gs_config.screenAspectRatio = ScreenAspectRatio::LETTERBOX;
        saveAndExit = true;
    }

    if (s_RuntimePlatformServices == nullptr || s_RuntimePlatformServices->supportsCustomScreenAspectModes())
    {
        if ( this->drawMenuItem( "Letterbox, Screen is 5:4", 2) )
        {
            gs_config.screenAspectRatio = ScreenAspectRatio::ASPECT5X4;
            saveAndExit = true;
        }

        if ( this->drawMenuItem( "Letterbox, Screen is 4:3", 3) )
        {
            gs_config.screenAspectRatio = ScreenAspectRatio::ASPECT4X3;
            saveAndExit = true;
        }

        if ( this->drawMenuItem( "Letterbox, Screen is 16:9", 4) )
        {
            gs_config.screenAspectRatio = ScreenAspectRatio::ASPECT16X9;
            saveAndExit = true;
        }

        if ( this->drawMenuItem( "Letterbox, Screen is 16:10", 5) )
        {
            gs_config.screenAspectRatio = ScreenAspectRatio::ASPECT16X10;
            saveAndExit = true;
        }
    }

    if ( saveAndExit )
    {
        s_settingsStorage.saveGroundStationConfig();
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the WiFi rate selection menu.
void OSDMenuController::drawWifiRateMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Menu -> Wifi Rate" );
    drawSpacing();

    bool saveAndExit = false;

    if ( this->drawMenuItem( "OFDM 18Mbps", 0) )
    {
        config.dataChannel.wifi_rate = WIFI_Rate::RATE_G_18M_ODFM;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "OFDM 24Mbps", 1) )
    {
        config.dataChannel.wifi_rate = WIFI_Rate::RATE_G_24M_ODFM;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "OFDM 36Mbps", 2) )
    {
        config.dataChannel.wifi_rate = WIFI_Rate::RATE_G_36M_ODFM;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "MCS2L 19.5Mbps", 3) )
    {
        config.dataChannel.wifi_rate = WIFI_Rate::RATE_N_19_5M_MCS2;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "MCS3L 26Mbps", 4) )
    {
        config.dataChannel.wifi_rate = WIFI_Rate::RATE_N_26M_MCS3;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "MCS4L 39Mbps", 5) )
    {
        config.dataChannel.wifi_rate = WIFI_Rate::RATE_N_39M_MCS4;
        saveAndExit = true;
    }

    if ( saveAndExit )
    {
        commitGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the WiFi channel selection menu.
void OSDMenuController::drawWifiChannelMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    this->drawMenuTitle( "Menu -> Wifi Channel" );
    drawSpacing();

    bool bExit = false;
    int itemIndex = 0;

    for ( int i = 0; i < WIFI_CHANNELS_COUNT; i++ )
    {
        int channel = WIFI_CHANNELS_BY_INDEX[i];
        if ( !isWifiChannelAllowedByBand(channel, gs_config.wifiBand) )
        {
            continue;
        }

        char buf[32];
        if (channel >= 36 && channel <= 48)
        {
            sprintf(buf, "%d  (5.8GHz,ETSI,FCC)", channel);
        }
        else
        if (channel >= 52 && channel <= 144)
        {
            sprintf(buf, "%d  (5.8GHz,ETSI,FCC,DFS)", channel);
        }
        else if (channel >= 149 && channel <= 165)
        {
            sprintf(buf, "%d  (5.8GHz,FCC)", channel);
        }
        else if (channel == 12 || channel == 13)
        {
            sprintf(buf, "%d  (2.4GHz,ETSI)", channel);
        }
        else
        {
            sprintf(buf, "%d  (2.4GHz,ETSI,FCC)", channel);
        }
        if ( this->drawMenuItem( buf, itemIndex, true) )
        {
            if ( gs_config.wifi_channel != channel )
            {
                gs_config.wifi_channel = channel;
                s_settingsStorage.saveGroundStationConfig();
                s_RuntimePlatformServices->applyGroundStationWifiChannel(config);
            }
            bExit = true;
        }

        itemIndex++;
    }

    if ( bExit || this->exitKeyPressed() )
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the camera stop RC channel selection menu.
void OSDMenuController::drawCameraStopCHMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Menu -> Camera Stop Channel" );
    drawSpacing();

    bool bExit = false;

    for ( int i = 0; i <= 18; i++ )
    {
        char buf[12];
        if ( i == 0 )
        {
            sprintf(buf, "None" );
        }
        else
        {
            sprintf(buf, "%d", i );
        }
        if ( this->drawMenuItem( buf, i, true) )
        {
            config.misc.cameraStopChannel = i;
            bExit = true;
        }
    }

    if ( bExit || this->exitKeyPressed() )
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the ground station TX power selection menu.
void OSDMenuController::drawGSTxPowerMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    this->drawMenuTitle( "Menu -> Tx Power" );
    drawSpacing();

    bool bExit = false;

    for ( int i = 0; i <= gs::menu::kMaxTxPower - gs::menu::kMinTxPower; i++ )
    {
        char buf[12];
        sprintf(buf, "%d", i + gs::menu::kMinTxPower);
        if ( this->drawMenuItem( buf, i, true) )
        {
            if ( gs_config.txPower != (i + gs::menu::kMinTxPower) )
            {
                gs_config.txPower = ( i + gs::menu::kMinTxPower );
                s_settingsStorage.saveGroundStationConfig();
                s_RuntimePlatformServices->applyGroundStationTxPower(config);
            }
            bExit = true;
        }
    }

    if ( bExit || this->exitKeyPressed() )
    {
        this->goBack();
    }
}


//===================================================================================
//===================================================================================
// Draws the RAW transport TX interface selection menu.
void OSDMenuController::drawGSTxInterfaceMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    this->drawMenuTitle( "GS Settings -> RAW TX Interface" );
    drawSpacing();

    bool saveAndExit = false;

    const auto interfaces = copyCurrentTransportInterfaces();

    if ( this->drawMenuItem( "auto", 0) )
    {
        gs_config.txInterface = "auto";
        applySelectedTxInterfaceToTransport();
        saveAndExit = true;
    }

    for ( unsigned int i = 0; i < interfaces.size(); i++ )
    {
        if ( this->drawMenuItem( interfaces[i].c_str(), i+1) )
        {
            gs_config.txInterface = interfaces[i];
            applySelectedTxInterfaceToTransport();
            saveAndExit = true;
        }
    }

    if ( saveAndExit )
    {
        s_settingsStorage.saveGroundStationConfig();
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the APFPV interface selection menu and applies the new interface immediately.
void OSDMenuController::drawGSApfpvInterfaceMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    this->drawMenuTitle( "GS Settings -> APFPV Interface" );
    drawSpacing();

    bool saveAndExit = false;
    const auto interfaces = copyCurrentTransportInterfaces();

    if ( this->drawMenuItem( "auto", 0) )
    {
        gs_config.apfpvInterface = "auto";
        saveAndExit = true;
    }

    for ( unsigned int i = 0; i < interfaces.size(); i++ )
    {
        if ( this->drawMenuItem( interfaces[i].c_str(), i+1) )
        {
            gs_config.apfpvInterface = interfaces[i];
            saveAndExit = true;
        }
    }

    if ( saveAndExit )
    {
        s_settingsStorage.saveGroundStationConfig();
        if (gs_config.transportKind == gs::core::TransportKind::APFPV)
        {
            queueSelectedTransportReconnect();
        }
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->goBack();
    }
}


//===================================================================================
//===================================================================================
// Draws the FEC (Forward Error Correction) strength selection menu.
void OSDMenuController::drawFECMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Menu -> FEC" );
    drawSpacing();

    bool saveAndExit = false;

    if ( this->drawMenuItem( "Weak (6/8)", 0) )
    {
        config.dataChannel.fec_codec_n = 8;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "Medium (6/10)", 1) )
    {
        config.dataChannel.fec_codec_n = 10;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "Strong (6/12)", 2) )
    {
        config.dataChannel.fec_codec_n = 12;
        saveAndExit = true;
    }

    if ( saveAndExit )
    {
        commitGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the ground station settings menu.
void OSDMenuController::drawGSSettingsMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    this->drawMenuTitle( "Menu -> GS Settings" );
    drawSpacing();

    if ( this->drawMenuItem( "Playback...", 0) )
    {
        this->goForward( OSDMenuId::Playback, 0 );
    }

    {
        char buf[256];
        sprintf(buf, "Screen Settings...##1");
        if ( this->drawMenuItem( buf, 1) )
        {
            this->goForward( OSDMenuId::GSScreen, 0 );
        }
    }

    {
        char buf[256];
        sprintf(buf, "Wifi Settings...##2");
        if ( this->drawMenuItem( buf, 2) )
        {
            this->goForward( OSDMenuId::GSWifiSettings, 0 );
        }
    }

    if ( this->drawMenuItem( "Debuging...", 3) )
    {
        this->goForward( OSDMenuId::Debug, 0 );
    }

    int next_item_index = 4;
    if (s_RuntimePlatformServices->supportsGPIOKeys())
    {
        char buf[256];
        const char* layout = gs_config.GPIOKeysLayout == 0 ? "DIY VRX" : "Runcam VRX";
        sprintf(buf, "GPIO Keys Layout: %s##gpio_keys", layout);
        if ( this->drawMenuItem( buf, next_item_index) )
        {
            gs_config.GPIOKeysLayout = gs_config.GPIOKeysLayout == 0 ? 1 : 0;
            s_settingsStorage.saveGroundStationConfig();
            s_RuntimePlatformServices->restartGPIOButtons();
        }
        next_item_index++;
    }

    // Keep indices contiguous when GPIO controls are hidden, otherwise up/down can focus an invisible slot.
    if ( this->drawMenuItem( "Exit To Shell##exit_shell", next_item_index) )
    {
        this->goForward( OSDMenuId::ExitToShell, 0 );
    }

    drawLargeGapIfTallScreen();

    {
        char buf[256];
        sprintf(buf, "IP: %s##status_ip", s_RuntimePlatformServices->getSystemIPv4().c_str());
        this->drawStatus( buf );
    }

    if ( this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the ground station screen settings menu.
void OSDMenuController::drawGSScreenMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    this->drawMenuTitle( "Menu -> GS Screen" );
    drawSpacing();

    {
        char buf[256];
        sprintf(buf, "Letterbox: %s##1", getVisibleScreenAspectLabel(gs_config.screenAspectRatio));
        if ( this->drawMenuItem( buf, 0) )
        {
            this->goForward( OSDMenuId::Letterbox, getVisibleScreenAspectSelection(gs_config.screenAspectRatio) );
        }
    }

    {
        if ( this->drawMenuItem( "Lens Correction...##lens_correction", 1) )
        {
            this->m_lens_correction_draft = s_lensCorrectionState;
            this->m_lens_correction_original = s_lensCorrectionState;
            this->m_lens_correction_draft_active = true;
            this->resetLensCorrectionStepMultiplier();
            this->goForward( OSDMenuId::GSLensCorrection, 0 );
        }
    }

    {
        char buf[256];
        sprintf(buf, "Vertical Sync: %s##3", gs_config.vsync ? "Enabled" :"Disabled");
        if ( this->drawMenuItem( buf, 2) )
        {
            gs_config.vsync = !gs_config.vsync;
            s_RuntimePlatformServices->setVsync(gs_config.vsync);
            s_settingsStorage.saveGroundStationConfig();
        }
    }

    {
        char buf[256];
        sprintf(buf, "Vertical Flip: %s##4", gs_config.screenFlipV ? "ON" : "OFF");
        if ( this->drawMenuItem( buf, 3) )
        {
            gs_config.screenFlipV = !gs_config.screenFlipV;
            s_settingsStorage.saveGroundStationConfig();
        }
    }

    bool zoom_handled = false;
    {
        const bool zoom_focused = (this->selectedItem == 4);
        if (m_draw_mode == DrawMode::Interactive && zoom_focused && !this->keyHandled)
        {
            if (isMenuAdjustIncreasePressed())
            {
                gs_config.screenZoom = std::roundf(std::clamp(gs_config.screenZoom + kScreenZoomStep, 0.5f, 1.5f) * kScreenZoomStepScale) / kScreenZoomStepScale;
                s_settingsStorage.saveGroundStationConfig();
                this->keyHandled = true;
                zoom_handled = true;
            }
            else if (isMenuAdjustDecreasePressed())
            {
                gs_config.screenZoom = std::roundf(std::clamp(gs_config.screenZoom - kScreenZoomStep, 0.5f, 1.5f) * kScreenZoomStepScale) / kScreenZoomStepScale;
                s_settingsStorage.saveGroundStationConfig();
                this->keyHandled = true;
                zoom_handled = true;
            }
        }
        char buf[256];
        sprintf(buf, "Zoom: <>%d%%##5", static_cast<int>(std::roundf(gs_config.screenZoom * 100.0f)));
        this->drawMenuItem( buf, 4);
    }

    {
        char buf[256];
        sprintf(buf, "VR Mode: %s##2", gs_config.vrMode ? "ON" : "OFF");
        if ( this->drawMenuItem( buf, 5) )
        {
            gs_config.vrMode = !gs_config.vrMode;
            s_settingsStorage.saveGroundStationConfig();
        }
    }

    bool vr_separation_handled = false;
    {
        const bool vr_sep_focused = (this->selectedItem == 6);
        if (m_draw_mode == DrawMode::Interactive && vr_sep_focused && !this->keyHandled)
        {
            if (isMenuAdjustIncreasePressed())
            {
                gs_config.screenVrSeparation = std::roundf(std::clamp(gs_config.screenVrSeparation + kScreenVrSeparationStep, kScreenVrSeparationMin, kScreenVrSeparationMax) * kScreenVrSeparationStepScale) / kScreenVrSeparationStepScale;
                s_settingsStorage.saveGroundStationConfig();
                this->keyHandled = true;
                vr_separation_handled = true;
            }
            else if (isMenuAdjustDecreasePressed())
            {
                gs_config.screenVrSeparation = std::roundf(std::clamp(gs_config.screenVrSeparation - kScreenVrSeparationStep, kScreenVrSeparationMin, kScreenVrSeparationMax) * kScreenVrSeparationStepScale) / kScreenVrSeparationStepScale;
                s_settingsStorage.saveGroundStationConfig();
                this->keyHandled = true;
                vr_separation_handled = true;
            }
        }
        char buf[256];
        sprintf(buf, "VR Separation: <>%+d%%##6", static_cast<int>(std::roundf(gs_config.screenVrSeparation * kScreenVrSeparationDisplayScale)));
        this->drawMenuItem( buf, 6);
    }

    if (!zoom_handled && !vr_separation_handled && this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws GS lens correction controls with live preview until Apply or Back.
void OSDMenuController::drawGSLensCorrectionMenu(Ground2Air_Config_Packet& config)
{
    (void)config;

    if (!this->m_lens_correction_draft_active)
    {
        this->m_lens_correction_draft = s_lensCorrectionState;
        this->m_lens_correction_original = s_lensCorrectionState;
        this->m_lens_correction_draft_active = true;
        this->resetLensCorrectionStepMultiplier();
    }

    this->drawMenuTitle( "GS Screen -> Lens Correction" );
    drawSpacing();

    bool coefficient_handled = false;
    auto draw_coefficient = [&](const char* label, const char* imgui_id, int item_index, double& value, double step)
    {
        const bool focused = (this->selectedItem == item_index);
        if (m_draw_mode == DrawMode::Interactive && focused && !this->keyHandled)
        {
            if (isMenuAdjustIncreasePressed())
            {
                value = roundLensCorrectionCoefficient(
                    value + step * this->getLensCorrectionStepMultiplier(item_index, +1));
                this->keyHandled = true;
                coefficient_handled = true;
            }
            else if (isMenuAdjustDecreasePressed())
            {
                value = roundLensCorrectionCoefficient(
                    value - step * this->getLensCorrectionStepMultiplier(item_index, -1));
                this->keyHandled = true;
                coefficient_handled = true;
            }
        }

        char buf[256];
        snprintf(buf, sizeof(buf), "%s: <>%.6f##%s", label, value, imgui_id);
        this->drawMenuItem(buf, item_index);
    };

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Enabled: %s##enabled", this->m_lens_correction_draft.enabled ? "On" : "Off");
        if ( this->drawMenuItem( buf, 0) )
        {
            this->m_lens_correction_draft.enabled = !this->m_lens_correction_draft.enabled;
            s_lensCorrectionState = this->m_lens_correction_draft;
        }
    }

    draw_coefficient("k1", "k1", 1, this->m_lens_correction_draft.k1, kLensCorrectionRadialStep);
    draw_coefficient("k2", "k2", 2, this->m_lens_correction_draft.k2, kLensCorrectionRadialStep);
    draw_coefficient("k3", "k3", 3, this->m_lens_correction_draft.k3, kLensCorrectionRadialStep);
    draw_coefficient("p1", "p1", 4, this->m_lens_correction_draft.p1, kLensCorrectionTangentialStep);
    draw_coefficient("p2", "p2", 5, this->m_lens_correction_draft.p2, kLensCorrectionTangentialStep);

    if ( this->drawMenuItem( "Apply##apply", 6) )
    {
        s_lensCorrectionState = this->m_lens_correction_draft;
        this->m_lens_correction_draft_active = false;
        this->resetLensCorrectionStepMultiplier();
        this->goBack();
        return;
    }

    if (coefficient_handled)
    {
        s_lensCorrectionState = this->m_lens_correction_draft;
    }

    if (!coefficient_handled && this->exitKeyPressed())
    {
        s_lensCorrectionState = this->m_lens_correction_original;
        this->m_lens_correction_draft_active = false;
        this->resetLensCorrectionStepMultiplier();
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the ground station WiFi settings menu.
void OSDMenuController::drawGSWifiSettingsMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    this->drawMenuTitle( "Menu -> GS Wifi Settings" );
    drawSpacing();
    const auto interfaces = copyCurrentTransportInterfaces();
    int item_index = 0;

    {
        char buf[256];
        const char* bands[] = {"2.4GHz", "5.8GHz", "2.4GHz & 5.8GHz"};
        int band_index = clamp((int)gs_config.wifiBand, 0, 2);
        sprintf(buf, "Band: %s##0", bands[band_index]);
        if ( this->drawMenuItem( buf, item_index) )
        {
            gs_config.wifiBand = (uint8_t)((band_index + 1) % 3);

            int channel = getValidWifiChannel(gs_config.wifi_channel);
            if ( !isWifiChannelAllowedByBand(channel, gs_config.wifiBand) )
            {
                channel = gs_config.wifiBand == GS_WIFI_BAND_5_8_GHZ
                    ? DEFAULT_WIFI_CHANNEL_5_8_GHZ
                    : DEFAULT_WIFI_CHANNEL_2_4GHZ;
            }

            bool channelChanged = gs_config.wifi_channel != channel;
            gs_config.wifi_channel = channel;
            s_settingsStorage.saveGroundStationConfig();
            if ( channelChanged )
            {
                s_RuntimePlatformServices->applyGroundStationWifiChannel(config);
            }
        }
        item_index++;
    }

    if (gs_config.transportKind == gs::core::TransportKind::RawBroadcast)
    {
        char buf[256];
        sprintf(buf, "RAW TX Interface: %s##1", gs_config.txInterface.c_str());
        if ( this->drawMenuItem( buf, item_index) )
        {
            size_t index = 0;
            for( size_t i = 0; i < interfaces.size(); i++ )
            {
                if ( interfaces[i] == gs_config.txInterface )
                {
                    index = i + 1;
                }
            }
            this->goForward( OSDMenuId::GSTxInterface, index );
        }
        item_index++;
    }
    else if (gs_config.transportKind == gs::core::TransportKind::APFPV &&
             (!s_transport || s_transport->supportsApfpvInterfaceSelection()))
    {
        char buf[256];
        sprintf(buf, "APFPV Interface: %s##1", gs_config.apfpvInterface.c_str());
        if ( this->drawMenuItem( buf, item_index) )
        {
            size_t index = 0;
            for( size_t i = 0; i < interfaces.size(); i++ )
            {
                if ( interfaces[i] == gs_config.apfpvInterface )
                {
                    index = i + 1;
                }
            }
            this->goForward( OSDMenuId::GSApfpvInterface, index );
        }
        item_index++;
    }

    if (!s_transport || s_transport->supportsTxPowerControl())
    {
        char buf[256];
        sprintf(buf, "TX Power: %d##2", gs_config.txPower);
        if ( this->drawMenuItem( buf, item_index) )
        {
            this->goForward( OSDMenuId::GSTxPower, gs_config.txPower - gs::menu::kMinTxPower);
        }
    }

    drawLargeGapIfTallScreen();

    if (!s_transport || s_transport->supportsNetworkInterfaceStatus())
    {
        if ( interfaces.empty() )
        {
            this->drawStatus("No network interfaces detected##if_status_empty");
        }
        else
        {
            for (size_t i = 0; i < interfaces.size(); i++)
            {
                std::string summary = getInterfaceSummary(interfaces[i]);
                char buf[512];
                snprintf(buf, sizeof(buf), "%s##if_status_%zu", summary.c_str(), i);
                this->drawStatus(buf);
            }
        }
    }

    if ( this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the restarting status screen.
void OSDMenuController::drawRestartMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Restarting..." );
}

//===================================================================================
//===================================================================================
// Draws the OSD font selection menu.
void OSDMenuController::drawOSDFontMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "GS -> Displayport OSD Font" );
    drawSpacing();

    bool bExit = false;

    const auto& fonts = s_OSDFontStorage->osdFontsList();
    for ( unsigned int i = 0; i < fonts.size(); i++ )
    {
        char buf[512];
        sprintf(buf, "%s", fonts[i].c_str());
        if (strlen(buf) > 30 )
        {
            buf[28]='.'; buf[29]='.'; buf[30]='.'; buf[31]=0;
        }
        if ( this->drawMenuItem( buf, i, true) )
        {
            if (s_flightOSD.currentFontName() != fonts[i])
            {
                s_flightOSD.setSelectedFontName(fonts[i]);
                s_settingsStorage.save();
                config.misc.osdFontCRC32 = lodepng_crc32(reinterpret_cast<const unsigned char*>(fonts[i].c_str()),
                                                         fonts[i].length());
                pendingOsdFontReload();
            }
            bExit = true;
        }
    }

    if ( bExit || this->exitKeyPressed() )
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the connection mode and channel search menu.
void OSDMenuController::drawConnectMenu(Ground2Air_Config_Packet& config)
{
    const gs::core::TransportKind transport_kind = currentTransportKind();
    const bool uses_channel_search = gs::core::TransportManagerBase::transportKindUsesChannelSearch(transport_kind);
    const ApfpvCameraStateSnapshot apfpv_camera_state = copyApfpvCameraState();
    this->drawMenuTitle("Menu -> Connect");
    drawSpacing();

    {
        char buf[256];
        sprintf(buf, "Mode: %s##0", gs::core::TransportManagerBase::transportModeLabel(transport_kind));
        if (this->drawMenuItem(buf, 0))
        {
            this->goForward(OSDMenuId::SearchMode, getTransportModeMenuIndex(transport_kind));
            return;
        }
    }

    if (transport_kind == gs::core::TransportKind::APFPV)
    {
        int item_index = 1;

        if (this->drawMenuItem("Search...##apfpv_search", item_index))
        {
            this->searchDone = false;
            beginSelectedTransportSearchOrConnect(config, this->search_tp);
            this->goForward(OSDMenuId::SearchRun, 0);
            return;
        }
        item_index++;

        if (apfpv_camera_state.active_camera_id != 0)
        {
            const std::string active_caption =
                "Active Air Id: " + formatApfpvCameraId(apfpv_camera_state.active_camera_id) + "##apfpv_active";
            (void)this->drawMenuItem(active_caption.c_str(), item_index);
            item_index++;
        }

        for (const ApfpvCameraDescriptor& camera : apfpv_camera_state.discovered_cameras)
        {
            if (camera.device_id == 0 || camera.device_id == apfpv_camera_state.active_camera_id)
            {
                continue;
            }

            const std::string connect_caption =
                "Connect to: " + formatApfpvCameraId(camera.device_id) + "##apfpv_connect_" + std::to_string(camera.device_id);
            if (this->drawMenuItem(connect_caption.c_str(), item_index))
            {
                s_groundstation_config.apfpvPreferredCameraId = camera.device_id;
                setApfpvPreferredCameraId(camera.device_id);
                s_settingsStorage.saveGroundStationConfig();
                this->close();
                queueSelectedTransportReconnect();
                return;
            }
            item_index++;
        }

        if (apfpv_camera_state.active_camera_id == 0 && apfpv_camera_state.discovered_cameras.empty())
        {
            this->drawStatus("No APFPV cameras found##apfpv_empty");
        }
    }

    if (uses_channel_search)
    {
        {
            char buf[256];
            sprintf(buf, "Search...##1");
            if (this->drawMenuItem(buf, 1))
            {
                if (!switchActiveTransport(transport_kind))
                {
                    return;
                }

                this->searchDone = false;
                beginSelectedTransportSearchOrConnect(config, this->search_tp);

                this->goForward(OSDMenuId::SearchRun, 0);
            }
        }
    }

    if (!uses_channel_search && transport_kind != gs::core::TransportKind::APFPV && this->selectedItem > 0)
    {
        this->selectedItem = 0;
    }

    if (this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the transport mode selection menu.
void OSDMenuController::drawSearchModeMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle("Search -> Mode");
    drawSpacing();

    for (int item_index = 0; item_index < static_cast<int>(gs::core::TransportKind::Count); ++item_index)
    {
        const gs::core::TransportKind kind = OSDMenuController::getTransportKindForMenuIndex(item_index);
        if (this->drawMenuItem(gs::core::TransportManagerBase::transportModeLabel(kind), item_index))
        {
            if (!switchActiveTransport(kind))
            {
                return;
            }

            if (!gs::core::TransportManagerBase::transportKindUsesChannelSearch(kind) &&
                kind != gs::core::TransportKind::APFPV)
            {
                this->searchDone = false;
                beginSelectedTransportSearchOrConnect(config, this->search_tp);
            }

            this->goBack();
            return;
        }
    }

    if (this->exitKeyPressed())
    {
        if (this->backMenuIds.empty())
        {
            this->menuId = OSDMenuId::Main;
            this->selectedItem = 0;
        }
        else
        {
            this->goBack();
        }
    }
}

//===================================================================================
//===================================================================================
// Draws the active channel search or connect progress screen.
void OSDMenuController::drawSearchRunMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    const bool uses_channel_search = gs::core::TransportManagerBase::transportKindUsesChannelSearch(currentTransportKind());
    const bool is_apfpv = currentTransportKind() == gs::core::TransportKind::APFPV;

    if (uses_channel_search)
    {
        char title[128];
        const char* band = gs_config.wifiBand == GS_WIFI_BAND_2_4_GHZ ? "2.4GHz" :
                           gs_config.wifiBand == GS_WIFI_BAND_5_8_GHZ ? "5.8GHz" :
                           "2.4 & 5.8 GHz";
        sprintf(title, "Menu -> Search (%s)...", band);
        this->drawMenuTitle(title);
    }
    else
    {
        this->drawMenuTitle(is_apfpv ? "Menu -> Search" : "Menu -> Connect");
    }
    drawSpacing();

    bool bExit = false;

    char buf[512];
    if (uses_channel_search)
    {
        sprintf(buf, searchDone ? "Found Channel: %d" : "Searching: Channel %d...", gs_config.wifi_channel);
    }
    else if (is_apfpv)
    {
        const ApfpvCameraStateSnapshot apfpv_camera_state = copyApfpvCameraState();
        if (apfpv_camera_state.discovered_cameras.size() >= 2)
        {
            sprintf(buf, "Found %d APFPV cameras.", static_cast<int>(apfpv_camera_state.discovered_cameras.size()));
        }
        else if (apfpv_camera_state.active_camera_id != 0 || isSelectedTransportConnected())
        {
            sprintf(buf, "Connected.");
        }
        else if (apfpv_camera_state.discovered_cameras.size() == 1)
        {
            sprintf(buf, "Connecting to %s...", formatApfpvCameraId(apfpv_camera_state.discovered_cameras.front().device_id).c_str());
        }
        else
        {
            sprintf(buf, "Searching for APFPV cameras...");
        }
    }
    else
    {
        sprintf(buf, "%s", isSelectedTransportConnected() ? "Connected." : "Connecting...");
    }
    if (!(is_apfpv && std::strcmp(buf, "Connected.") == 0))
    {
        this->drawStatus(buf);
    }

    advanceSelectedTransportSearchOrConnect(config, this->search_tp, this->searchDone);

    if (uses_channel_search)
    {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - this->search_tp).count() > SEARCH_TIME_STEP_MS &&
            this->searchDone)
        {
            bExit = true;
        }
    }
    else if (is_apfpv)
    {
        bExit = this->searchDone;
    }
    else if (isSelectedTransportConnected())
    {
        bExit = true;
    }

    if ( bExit || this->exitKeyPressed() )
    {
        cancelSelectedTransportSearchOrConnect();
        if (uses_channel_search)
        {
            s_settingsStorage.saveGroundStationConfig();
        }
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
//===================================================================================
// Draws the playback menu listing recordings in the GS recordings directory.
void OSDMenuController::drawPlaybackMenu(Ground2Air_Config_Packet& /*config*/)
{
    this->drawMenuTitle("Menu -> Playback");
    drawSpacing();

    const auto recordings = s_recordingsStorage->listRecordings();

    if (recordings.empty())
    {
        this->drawStatus("No recordings found");
    }

    for (unsigned int i = 0; i < recordings.size(); i++)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s (%zu KB)", recordings[i].name.c_str(), recordings[i].size_kb);
        if (this->drawMenuItem(buf, static_cast<int>(i), true))
        {
            if (s_playbackManager != nullptr)
            {
                s_playbackManager->startPlayback(recordings[i]);
                this->close();
            }
        }
    }

    if (this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Draws the active playback screen and lets Up/Down return to the Playback file list.
void OSDMenuController::drawPlaybackRunMenu(Ground2Air_Config_Packet& /*config*/)
{
    this->drawMenuTitle("Menu -> Playback");
    drawSpacing();

    PlaybackStatus status = {};
    if (s_playbackManager != nullptr)
    {
        status = s_playbackManager->status();
    }

    if (status.broken)
    {
        this->drawStatus("Broken Video Sequence");
    }
    else
    {
        this->drawStatus("Playing...");
    }

    if (isMenuUpPressed() || isMenuDownPressed() || this->exitKeyPressed())
    {
        if (s_playbackManager != nullptr)
        {
            s_playbackManager->stopPlayback();
        }
        this->keyHandled = true;
        this->goBack();
    }
}

//===================================================================================
// Draws the debugging tools menu.
void OSDMenuController::drawDebugMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    this->drawMenuTitle( "Menu -> Debugging" );
    drawSpacing();

    {
        char buf[256];
        sprintf(buf, "Toggle Statistics##0");
        if ( this->drawMenuItem( buf, 0) )
        {
            gs_config.stats = !gs_config.stats;
        }
    }

    {
        char buf[256];
        sprintf(buf, "Draw packets##1");
        if ( this->drawMenuItem( buf, 1) )
        {
            g_framePacketsDebug.captureFrame(false);
        }
    }

    {
        char buf[256];
        sprintf(buf, "Draw packets till loss##2");
        if ( this->drawMenuItem( buf, 2) )
        {
            g_framePacketsDebug.captureFrame(true);
        }
    }

    {
        char buf[256];
        sprintf(buf, "Hide packets##3");
        if ( this->drawMenuItem( buf, 3) )
        {
            g_framePacketsDebug.off();
        }
    }

    if ( this->exitKeyPressed())
    {
        this->goBack();
    }
}

//===================================================================================
//===================================================================================
// Navigates forward to a new menu page, saving the current page to the back stack.
void OSDMenuController::goForward(OSDMenuId newMenuId, int newItem)
{
    this->backMenuIds.push_back(this->menuId);
    this->backMenuItems.push_back(this->selectedItem);

    this->menuId = newMenuId;
    this->selectedItem = newItem;
}

//===================================================================================
//===================================================================================
// Navigates back to the previous menu page, or closes when no parent remains.
void OSDMenuController::goBack()
{
    if ( this->backMenuIds.size() > 0 )
    {
        this->menuId = this->backMenuIds.back();
        this->backMenuIds.pop_back();
        this->selectedItem = this->backMenuItems.back();
        this->backMenuItems.pop_back();
    }
    else
    {
        this->close();
    }
}
