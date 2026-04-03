#include "core/osd_menu_controller.h"
#include <cmath>
#include <cstring>
#include "util.h"
#include "gs_runtime_platform_services.h"
#include "gs_shared_runtime.h"
#include "gs_runtime_config.h"
#include "gs_runtime_osd_font_storage.h"
#include "gs_runtime_state.h"
#include "gs_storage_status_shared.h"

#define SEARCH_TIME_STEP_MS 1000

using OSDMenuController = gs::menu::OSDMenuController;

namespace
{

gs::menu::imgui::MenuFrameLayout offsetMenuLayout(const gs::menu::imgui::MenuFrameLayout& layout, float origin_x)
{
    gs::menu::imgui::MenuFrameLayout shifted = layout;
    shifted.window_x += origin_x;
    return shifted;
}

int getVisibleScreenAspectSelection(const gs::menu::IOSDMenuPlatform& platform, ScreenAspectRatio ratio)
{
    if (!platform.supportsCustomScreenAspectModes())
    {
        return ratio == ScreenAspectRatio::STRETCH ? 0 : 1;
    }
    return clamp(static_cast<int>(ratio), 0, 5);
}

const char* getVisibleScreenAspectLabel(const gs::menu::IOSDMenuPlatform& platform, ScreenAspectRatio ratio)
{
    if (!platform.supportsCustomScreenAspectModes())
    {
        return ratio == ScreenAspectRatio::STRETCH ? "Stretch" : "Letterbox";
    }

    const char* modes[] = {"Stretch", "Letterbox", "Screen is 5:4", "Screen is 4:3", "Screen is 16:9", "Screen is 16:10"};
    return modes[clamp(static_cast<int>(ratio), 0, 5)];
}

}

//=======================================================
//=======================================================
OSDMenuController::OSDMenuController(gs::menu::IOSDMenuPlatform& platform)
    : m_platform(platform)
{
    this->visible = false;
}

void OSDMenuController::open()
{
    this->visible = true;
    this->menuId = OSDMenuId::Main;
    this->selectedItem = 0;
    this->backMenuIds.clear();
    this->backMenuItems.clear();
}

void OSDMenuController::close()
{
    this->visible = false;
}

bool OSDMenuController::isVisible() const
{
    return this->visible;
}

//=======================================================
//=======================================================
void OSDMenuController::drawMenuTitle( const char* caption )
{
    gs::menu::imgui::drawMenuTitle(caption, m_imgui_layout);
    this->itemsCount = 0;
    this->keyHandled = false;
    drawLargeGapIfTallScreen();
}

//=======================================================
//=======================================================
void OSDMenuController::drawStatus( const char* caption )
{
    gs::menu::imgui::drawMenuStatus(caption, m_imgui_layout);
}

void OSDMenuController::drawSpacing()
{
    gs::menu::imgui::drawSmallGap(m_imgui_layout);
}

void OSDMenuController::drawLargeGapIfTallScreen()
{
    gs::menu::imgui::drawLargeGap(m_imgui_layout);
}

//=======================================================
//=======================================================
bool OSDMenuController::drawMenuItem( const char* caption, int itemIndex, bool clip )
{
    int d = itemIndex - this->selectedItem;
    int d1 = 2;
    int d2 = -2;
    if ( this->selectedItem == 0) d1+=2;
    if ( this->selectedItem == 1) d1+=1;
    if ( clip  && ((d>d1)|| (d<d2)))
    {
        return false;
    }

    bool focused = this->selectedItem == itemIndex;
    bool res = false;

    if (m_draw_mode == DrawMode::Interactive)
    {
        res = focused && ( ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) && !this->keyHandled;
    }

    if (gs::menu::imgui::drawMenuItem(caption, m_imgui_layout, focused))
    {
        res = true;
    }

    this->itemsCount = std::max(this->itemsCount, itemIndex + 1);

    this->keyHandled |= res;

    if ( res )
    {
        this->selectedItem = itemIndex;
    }

    return res;
}

void OSDMenuController::drawMenuWindow(const char* window_name,
                                       const gs::menu::imgui::MenuFrameLayout& layout,
                                       Ground2Air_Config_Packet& config,
                                       DrawMode mode,
                                       ImGuiWindowFlags extra_flags)
{
    m_imgui_layout = layout;

    this->itemsCount = 0;
    this->keyHandled = false;
    this->m_draw_mode = mode;

    gs::menu::imgui::beginMenuWindow(window_name, m_imgui_layout, extra_flags);
    drawCurrentMenu(config);
    gs::menu::imgui::endMenuWindow();

    this->m_draw_mode = DrawMode::Interactive;
}

//=======================================================
//=======================================================
void OSDMenuController::draw(Ground2Air_Config_Packet& config)
{
    if (!this->visible) 
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_MouseRight))
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
        gs::menu::imgui::buildMenuFrameLayout(primary_width, screenSize.y, false, 29.0f),
        0.0f);

    drawMenuWindow("OSD_MENU", primary_layout, config, DrawMode::Interactive);

    if ( ImGui::IsKeyPressed(ImGuiKey_UpArrow) && this->selectedItem > 0 )
    {
        this->selectedItem--;
    }

    if ( ImGui::IsKeyPressed(ImGuiKey_DownArrow) && this->selectedItem < (this->itemsCount - 1) )
    {
        this->selectedItem++;
    }

    if (vr_mode)
    {
        const auto clone_layout = offsetMenuLayout(
            gs::menu::imgui::buildMenuFrameLayout(primary_width, screenSize.y, false, 29.0f),
            primary_width);
        drawMenuWindow("OSD_MENU_VR_CLONE",
                       clone_layout,
                       config,
                       DrawMode::Passive,
                       ImGuiWindowFlags_NoInputs |
                           ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoSavedSettings);
    }
}

void OSDMenuController::drawCurrentMenu(Ground2Air_Config_Packet& config)
{
    switch (this->menuId)
    {
        case OSDMenuId::Main: this->drawMainMenu(config); break;
        case OSDMenuId::CameraSettings: this->drawCameraSettingsMenu(config); break;
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
        case OSDMenuId::OSDFont: this->drawOSDFontMenu(config); break;
        case OSDMenuId::Search: this->drawSearchMenu(config); break;
        case OSDMenuId::GSTxPower: this->drawGSTxPowerMenu(config); break;
        case OSDMenuId::GSTxInterface: this->drawGSTxInterfaceMenu(config); break;
        case OSDMenuId::Image: this->drawImageSettingsMenu(config); break;
        case OSDMenuId::CameraStopCH: this->drawCameraStopCHMenu(config); break;
        case OSDMenuId::Debug: this->drawDebugMenu(config); break;
    }
}

//=======================================================
//=======================================================
void OSDMenuController::searchNextWifiChannel(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    this->search_tp = Clock::now() + std::chrono::milliseconds(SEARCH_TIME_STEP_MS);
    
    gs_config.wifi_channel = getBandAwareWifiChannel(gs_config.wifi_channel, gs_config.wifiBand);
    int channel_index = getWifiChannelIndex(gs_config.wifi_channel);

    for (int i = 0; i < WIFI_CHANNELS_COUNT; i++)
    {
        channel_index++;
        if (channel_index >= WIFI_CHANNELS_COUNT) channel_index = 0;
        int nextChannel = WIFI_CHANNELS_BY_INDEX[channel_index];
        if (isWifiChannelAllowedByBand(nextChannel, gs_config.wifiBand))
        {
            gs_config.wifi_channel = nextChannel;
            break;
        }
    }
    
    m_platform.applyWifiChannelInstant(config);
}

//=======================================================
//=======================================================
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
            this->searchNextWifiChannel(config);
            this->searchDone = false;
            m_platform.airUnpair();
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

    if ( this->drawMenuItem( "Camera Settings...", 5) )
    {
        this->goForward( OSDMenuId::CameraSettings, 0 );

        if ( s_isOV5640 && config.camera.vflip )
        {
            config.camera.vflip = false;
            commitGround2AirConfig(config);
        }
    }

    if ( this->drawMenuItem( "Ground Station Settings...", 6) )
    {
        this->goForward( OSDMenuId::GSSettings, 0 );
    }

    //this->drawMenuItem( "OSD...", 5);

    drawLargeGapIfTallScreen();

    {
        const AirStorageStatusView air_storage = {
            s_SDDetected,
            s_SDError,
            s_SDSlow,
            s_SDFreeSpaceGB16,
            s_SDTotalSpaceGB16,
        };
        std::string line = formatAirStorageStatusLine(air_storage);
        line += "##status0";
        this->drawStatus(line.c_str());
    }

    {
        const auto gs_storage = s_RuntimePlatformServices->getGroundStorageStatus();
        std::string line = formatGroundStorageStatusLine(gs_storage);
        line += "##status1";
        this->drawStatus(line.c_str());
    }

    if ( this->exitKeyPressed())
    {
        this->visible = false;
    }
}

//=======================================================
//=======================================================
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

//=======================================================
//=======================================================
void OSDMenuController::drawCameraSettingsMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Menu -> Camera Settings" );
    
    {
        if ( this->drawMenuItem( "Image Settings...", 0 ) )
        {
            this->goForward( OSDMenuId::Image, 0 );
        }
    }

    {
        char buf[512];
        sprintf(buf, "OSD Font: %s", m_platform.currentOSDFontName());
        if (strlen(buf) > 30 )
        {
            buf[28]='.'; buf[29]='.'; buf[30]='.'; buf[31]=0;
        }
        strcat(buf, "##1");
        if ( this->drawMenuItem( buf, 1) )
        {
            const auto& fonts = s_OSDFontStorage->osdFontsList();
            auto it = std::find(fonts.begin(), fonts.end(), m_platform.currentOSDFontName());
            this->goForward( OSDMenuId::OSDFont, it != fonts.end() ? static_cast<int>(std::distance(fonts.begin(), it)) : 0 );
        }
    }


    {
        char buf[256];
        sprintf(buf, "Autostart recording: %s", config.misc.autostartRecord == 1? "On" : "Off");
        if ( this->drawMenuItem( buf, 2) )
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
        if ( this->drawMenuItem( buf, 3) )
        {
            this->goForward( OSDMenuId::CameraStopCH, (int)config.misc.cameraStopChannel );
        }
    }

    {
        char buf[256];
        sprintf(buf, "Mavlink2 to Msp RC: %s", config.misc.mavlink2mspRC == 1? "On" : "Off");
        if ( this->drawMenuItem( buf, 4) )
        {
            config.misc.mavlink2mspRC ^= 1;
            
        }
    }

    {
        char buf[256];
        sprintf(buf, "Air to GS MTU: %d", config.dataChannel.fec_codec_mtu);
        if ( this->drawMenuItem( buf, 5) )
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

//=======================================================
//=======================================================
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

//=======================================================
//=======================================================
bool OSDMenuController::exitKeyPressed()
{
    if (m_draw_mode != DrawMode::Interactive)
    {
        return false;
    }

    return ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_R) || ImGui::IsKeyPressed(ImGuiKey_G) 
    || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_MouseRight);
}

//=======================================================
//=======================================================
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


//=======================================================
//=======================================================
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

//=======================================================
//=======================================================
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

//=======================================================
//=======================================================
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

//=======================================================
//=======================================================
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

//=======================================================
//=======================================================
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

//=======================================================
//=======================================================
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

    if (m_platform.supportsCustomScreenAspectModes())
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

//=======================================================
//=======================================================
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

//=======================================================
//=======================================================
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
                m_platform.applyWifiChannel(config);
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

//=======================================================
//=======================================================
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

//=======================================================
//=======================================================
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
                m_platform.applyGSTxPower(config);
            }
            bExit = true;
        }
    }

    if ( bExit || this->exitKeyPressed() )
    {
        this->goBack();
    }
}


//=======================================================
//=======================================================
void OSDMenuController::drawGSTxInterfaceMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    this->drawMenuTitle( "GS Settings -> TX Interface" );
    drawSpacing();

    bool saveAndExit = false;

    auto rx_descriptor = m_platform.transport().getRXDescriptor();

    if ( this->drawMenuItem( "auto", 0) )
    {
        gs_config.txInterface = "auto";

        m_platform.transport().setTxInterface(rx_descriptor.interfaces[0]);
        m_platform.transport().setTxPower(gs_config.txPower);

        saveAndExit = true;
    }

    for ( unsigned int i = 0; i < rx_descriptor.interfaces.size(); i++ )
    {
        if ( this->drawMenuItem( rx_descriptor.interfaces[i].c_str(), i+1) )
        {
            gs_config.txInterface = rx_descriptor.interfaces[i];
            m_platform.transport().setTxInterface(gs_config.txInterface);
            m_platform.transport().setTxPower(gs_config.txPower);
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


//=======================================================
//=======================================================
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

//=======================================================
//=======================================================
void OSDMenuController::drawGSSettingsMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    this->drawMenuTitle( "Menu -> GS Settings" );
    drawSpacing();

    {
        char buf[256];
        sprintf(buf, "Screen Settings...##1");
        if ( this->drawMenuItem( buf, 0) )
        {
            this->goForward( OSDMenuId::GSScreen, 0 );
        }
    }

    {
        char buf[256];
        sprintf(buf, "Wifi Settings...##2");
        if ( this->drawMenuItem( buf, 1) )
        {
            this->goForward( OSDMenuId::GSWifiSettings, 0 );
        }
    }

    if ( this->drawMenuItem( "Debuging...", 2) )
    {
        this->goForward( OSDMenuId::Debug, 0 );
    }

    {
        char buf[256];
        const char* layout = gs_config.GPIOKeysLayout == 0 ? "DIY VRX" : "Runcam VRX";
        sprintf(buf, "GPIO Keys Layout: %s##4", layout);
        if ( this->drawMenuItem( buf, 3) )
        {
            gs_config.GPIOKeysLayout = gs_config.GPIOKeysLayout == 0 ? 1 : 0;
            s_settingsStorage.saveGroundStationConfig();
            s_RuntimePlatformServices->restartGPIOButtons();
        }
    }

    if ( this->drawMenuItem( "Exit To Shell##7", 4) )
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

//=======================================================
//=======================================================
void OSDMenuController::drawGSScreenMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    this->drawMenuTitle( "Menu -> GS Screen" );
    drawSpacing();

    {
        char buf[256];
        sprintf(buf, "Letterbox: %s##1", getVisibleScreenAspectLabel(m_platform, gs_config.screenAspectRatio));
        if ( this->drawMenuItem( buf, 0) )
        {
            this->goForward( OSDMenuId::Letterbox, getVisibleScreenAspectSelection(m_platform, gs_config.screenAspectRatio) );
        }
    }

    {
        char buf[256];
        sprintf(buf, "VR Mode: %s##2", gs_config.vrMode ? "ON" : "OFF");
        if ( this->drawMenuItem( buf, 1) )
        {
            gs_config.vrMode = !gs_config.vrMode;
            s_settingsStorage.saveGroundStationConfig();
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

    if ( this->exitKeyPressed())
    {
        this->goBack();
    }
}

//=======================================================
//=======================================================
void OSDMenuController::drawGSWifiSettingsMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    this->drawMenuTitle( "Menu -> GS Wifi Settings" );
    drawSpacing();
    auto rx_descriptor = m_platform.transport().getRXDescriptor();

    {
        char buf[256];
        const char* bands[] = {"2.4GHz", "5.8GHz", "2.4GHz & 5.8GHz"};
        int band_index = clamp((int)gs_config.wifiBand, 0, 2);
        sprintf(buf, "Band: %s##0", bands[band_index]);
        if ( this->drawMenuItem( buf, 0) )
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
                m_platform.applyWifiChannel(config);
            }
        }
    }

    {
        char buf[256];
        sprintf(buf, "TX Interface: %s##1", gs_config.txInterface.c_str());
        if ( this->drawMenuItem( buf, 1) )
        {
            size_t index = 0;
            for( size_t i = 0; i < rx_descriptor.interfaces.size(); i++ )
            {
                if ( rx_descriptor.interfaces[i] == gs_config.txInterface )
                {
                    index = i + 1;
                }
            }
            this->goForward( OSDMenuId::GSTxInterface, index );
        }
    }

    {
        char buf[256];
        sprintf(buf, "TX Power: %d##2", gs_config.txPower);
        if ( this->drawMenuItem( buf, 2) )
        {
            this->goForward( OSDMenuId::GSTxPower, gs_config.txPower - gs::menu::kMinTxPower);
        }
    }

    drawLargeGapIfTallScreen();

    if ( rx_descriptor.interfaces.empty() )
    {
        this->drawStatus("No network interfaces detected##if_status_empty");
    }
    else
    {
        for (size_t i = 0; i < rx_descriptor.interfaces.size(); i++)
        {
            std::string summary = getInterfaceSummary(rx_descriptor.interfaces[i]);
            char buf[512];
            snprintf(buf, sizeof(buf), "%s##if_status_%zu", summary.c_str(), i);
            this->drawStatus(buf);
        }
    }

    if ( this->exitKeyPressed())
    {
        this->goBack();
    }
}

void OSDMenuController::drawRestartMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Restarting..." );
}

//=======================================================
//=======================================================
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
            if ( strcmp( m_platform.currentOSDFontName(), fonts[i].c_str())!= 0)
            {
                m_platform.selectOSDFont(config, fonts[i]);
            }
            bExit = true;
        }
    }

    if ( bExit || this->exitKeyPressed() )
    {
        this->goBack();
    }
}

//=======================================================
//=======================================================
void OSDMenuController::drawSearchMenu(Ground2Air_Config_Packet& config)
{
    auto& gs_config = s_groundstation_config;
    char title[128];
    const char* band = gs_config.wifiBand == GS_WIFI_BAND_2_4_GHZ ? "2.4GHz" :
                       gs_config.wifiBand == GS_WIFI_BAND_5_8_GHZ ? "5.8GHz" :
                       "2.4 & 5.8 GHz";
    sprintf(title, "Menu -> Search (%s)...", band);
    this->drawMenuTitle( title );
    drawSpacing();

    bool bExit = false;

    char buf[512];
    sprintf(buf, searchDone ? "Found Channel: %d" : "Searching: Channel %d...", gs_config.wifi_channel);
    this->drawStatus(buf);

    if ( std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - this->search_tp).count() > SEARCH_TIME_STEP_MS )
    {
        if ( this->searchDone )
        {
            bExit = true;
        }
        else if ( std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s_last_packet_tp).count() < SEARCH_TIME_STEP_MS/2 )
        {
            this->searchDone = true;
            this->search_tp = Clock::now() + std::chrono::milliseconds(SEARCH_TIME_STEP_MS);
            s_settingsStorage.saveGroundStationConfig();  //save Wifi channel
        }
        else
        {
            this->searchNextWifiChannel(config);
        }
    }

    if ( bExit || this->exitKeyPressed() )
    {
        s_settingsStorage.saveGroundStationConfig();
        this->goBack();
    }
}

//=======================================================
//=======================================================
//=======================================================
//=======================================================
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
            m_platform.captureFrameDebug(false);
        }
    }

    {
        char buf[256];
        sprintf(buf, "Draw packets till loss##2");
        if ( this->drawMenuItem( buf, 2) )
        {
            m_platform.captureFrameDebug(true);
        }
    }

    {
        char buf[256];
        sprintf(buf, "Hide packets##3");
        if ( this->drawMenuItem( buf, 3) )
        {
            m_platform.disableFrameDebug();
        }
    }

    if ( this->exitKeyPressed())
    {
        this->goBack();
    }
}

//=======================================================
//=======================================================
void OSDMenuController::goForward(OSDMenuId newMenuId, int newItem)
{
    this->backMenuIds.push_back(this->menuId);
    this->backMenuItems.push_back(this->selectedItem);

    this->menuId = newMenuId;
    this->selectedItem = newItem;
}
//=======================================================
//=======================================================
void OSDMenuController::goBack()
{
    if ( this->backMenuIds.size() > 0 )
    {
        this->menuId = this->backMenuIds.back();
        this->backMenuIds.pop_back();
        this->selectedItem = this->backMenuItems.back();
        this->backMenuItems.pop_back();
    }
}
