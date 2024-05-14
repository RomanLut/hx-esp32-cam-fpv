#include "osd_menu.h"
#include <cmath>
#include "util.h"

OSDMenu g_osdMenu;

//=======================================================
//=======================================================
OSDMenu::OSDMenu()
{
    this->visible = false;
}

//=======================================================
//=======================================================
void OSDMenu::drawMenuTitle( const char* caption )
{
    ImVec4 color = (ImVec4)ImColor(97,137,105);
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.02f, 0.5f));
    ImGui::Button(caption, ImVec2( this->bWidth, this->bHeight ) );
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    
    this->itemsCount = 0;
    this->keyHandled = false;

    ImGui::Dummy(ImVec2(0.0f, 20.0f));
}

//=======================================================
//=======================================================
void OSDMenu::drawStatus( const char* caption )
{
    ImVec4 color = (ImVec4)ImColor(0,0,0);
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.02f, 0.5f));
    ImGui::Button(caption, ImVec2( this->sWidth, this->bHeight ) );
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

//=======================================================
//=======================================================
bool OSDMenu::drawMenuItem( const char* caption, int itemIndex, bool clip )
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
    bool res = focused && ( ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) && !this->keyHandled;

    ImGui::Indent();

    ImVec4 color = focused ? (ImVec4)ImColor(77,137,205) : (ImVec4)ImColor(37,51,88);
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.02f, 0.5f));
    if ( ImGui::Button(caption, ImVec2( this->bWidth, this->bHeight ) ) )
    {
        res = true;
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    ImGui::Unindent();

    this->itemsCount = itemIndex + 1;

    this->keyHandled |= res;

    return res;
}

//=======================================================
//=======================================================
void OSDMenu::draw(Ground2Air_Config_Packet& config)
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

    int windowWidth = 500;
    int windowHeight = 600;

    this->bWidth = windowWidth - 58;
    this->sWidth = windowWidth;
    this->bHeight = 35;

    ImVec2 screenSize = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(floor( (screenSize.x - windowWidth) / 2) , floor( (screenSize.y - windowHeight) /2 + 120 )), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight));
    ImGui::Begin("OSD_MENU", NULL, ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoMove |
                            ImGuiWindowFlags_NoScrollbar |
                            ImGuiWindowFlags_NoCollapse |
                            ImGuiWindowFlags_NoNav |
                            ImGuiWindowFlags_NoTitleBar |
                            ImGuiWindowFlags_NoBackground
                            );

    switch (this->menuId)
    {
        case OSDMenuId::Main: this->drawMainMenu(config); break;
        case OSDMenuId::PictureSettings: this->drawPictureSettingsMenu(config); break;
        case OSDMenuId::Resolution: this->drawResolutionMenu(config); break;
        case OSDMenuId::Brightness: this->drawBrightnessMenu(config); break;
        case OSDMenuId::Contrast: this->drawContrastMenu(config); break;
        case OSDMenuId::Exposure: this->drawExposureMenu(config); break;
        case OSDMenuId::Saturation: this->drawSaturationMenu(config); break;
        case OSDMenuId::Sharpness: this->drawSharpnessMenu(config); break;
        case OSDMenuId::VerticalFlip: this->drawVerticalFlipMenu(config); break;
        case OSDMenuId::Letterbox: this->drawLetterboxMenu(config); break;
        case OSDMenuId::WifiRate: this->drawWifiRateMenu(config); break;
        case OSDMenuId::WifiChannel: this->drawWifiChannelMenu(config); break;
        case OSDMenuId::Restart: this->drawRestartMenu(config); break;
    }

    if ( ImGui::IsKeyPressed(ImGuiKey_UpArrow) && this->selectedItem > 0 )
    {
        this->selectedItem--;
    }

    if ( ImGui::IsKeyPressed(ImGuiKey_DownArrow) && this->selectedItem < (this->itemsCount - 1) )
    {
        this->selectedItem++;
    }

    ImGui::End();
}

//=======================================================
//=======================================================
void OSDMenu::drawMainMenu(Ground2Air_Config_Packet& config)
{
    {
        char buf[256];
        sprintf( buf, "ESP32-CAM-FPV v%s %s##title0", "0.1", s_isOV5640 ? "OV5640" : "OV2640");
        this->drawMenuTitle( buf );
    }

    {
        char buf[256];
        sprintf(buf, "Resolution: %s##0", resolutionName[(int)config.camera.resolution]);
        if ( this->drawMenuItem( buf, 0) )
        {
            this->menuId = OSDMenuId::Resolution;
            if ( config.camera.resolution == Resolution::VGA16) this->selectedItem = 0;
            else if ( config.camera.resolution == Resolution::VGA) this->selectedItem = 1;
            else if ( config.camera.resolution == Resolution::SVGA16) this->selectedItem = 2;
            else if ( config.camera.resolution == Resolution::SVGA) this->selectedItem = 3;
            else if ( config.camera.resolution == Resolution::HD) this->selectedItem = 4;
            else if ( config.camera.resolution == Resolution::SXGA) this->selectedItem = 5;
            else selectedItem = 0;
        }
    }
    
    {
        char buf[256];
        sprintf(buf, "Wifi Channel: %d##1", s_groundstation_config.wifi_channel);
        if ( this->drawMenuItem( buf, 1) )
        {
            this->menuId = OSDMenuId::WifiChannel;
            this->selectedItem = s_groundstation_config.wifi_channel-1;
        }
    }

    {
        char buf[256];
        int i = clamp( (int)config.wifi_rate - (int)WIFI_Rate::RATE_G_12M_ODFM, 0, 3);
        const char* rates[] = {"12Mbps", "18Mbps", "24Mbps", "36Mbps"};
        sprintf(buf, "Wifi Rate: %s##2", rates[i]);
        if ( this->drawMenuItem( buf, 2) )
        {
            this->menuId = OSDMenuId::WifiRate;
            this->selectedItem = i;
        }
    }

    if ( this->drawMenuItem( "Picture Settings...", 3) )
    {
        this->menuId = OSDMenuId::PictureSettings;
        this->selectedItem = 0;
    }
    {
        char buf[256];
        const char* modes[] = {"Stretch", "Screen is 4:3", "Screen is 16:9"};
        sprintf(buf, "Letterbox: %s##4", modes[clamp((int)s_groundstation_config.screenAspectRatio,0,2)]);
        if ( this->drawMenuItem( buf, 4) )
        {
            this->menuId = OSDMenuId::Letterbox;
            this->selectedItem = (int)s_groundstation_config.screenAspectRatio;
        }
    }
    //this->drawMenuItem( "OSD...", 5);

    ImGui::Dummy(ImVec2(0.0f, 20.0f));

    {
        char buf[256];
        sprintf( buf, "AIR SD: %s%s%s %.2fGB/%.2fGB##status0", 
            s_SDDetected && !s_SDError? "Ok" : "?", s_SDError ? " Error" :"",  s_SDSlow ? " Slow" : "",
            s_SDFreeSpaceGB16 / 16.0f, s_SDTotalSpaceGB16 / 16.0f
        );

        this->drawStatus( buf );
    }

    if ( this->exitKeyPressed())
    {
        this->visible = false;
    }
}

//=======================================================
//=======================================================
void OSDMenu::drawPictureSettingsMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Menu -> Picture Settings" );
    
    {
        char buf[256];
        sprintf(buf, "Brightness: %d##0", config.camera.brightness);
        if ( this->drawMenuItem( buf, 0) )
        {
            this->menuId = OSDMenuId::Brightness;
            this->selectedItem = config.camera.brightness + 2;
        }
    }

    {
        char buf[256];
        sprintf(buf, "Contrast: %d##1", config.camera.contrast);
        if ( this->drawMenuItem( buf, 1) )
        {
            this->menuId = OSDMenuId::Contrast;
            this->selectedItem = config.camera.contrast + 2;
        }
    }

    {
        char buf[256];
        sprintf(buf, "Exposure: %d##2", config.camera.ae_level);
        if ( this->drawMenuItem( buf, 2) )
        {
            this->menuId = OSDMenuId::Exposure;
            this->selectedItem = config.camera.ae_level + 2;
        }
    }

    {
        char buf[256];
        sprintf(buf, "Saturation: %d##3", config.camera.saturation);
        if ( this->drawMenuItem( buf, 3) )
        {
            this->menuId = OSDMenuId::Saturation;
            this->selectedItem = config.camera.saturation + 2;
        }
    }

    {
        char buf[256];
        const char* sharpnessLevels[] = {"Blur more", "Blur", "Normal", "Sharpen", "Sharpen more", "Adaptive"};
        sprintf(buf, "Sharpness: %s##4", sharpnessLevels[clamp((int)config.camera.sharpness,-2,3)+2]);
        if ( this->drawMenuItem( buf, 4) )
        {
            this->menuId = OSDMenuId::Sharpness;
            this->selectedItem = config.camera.sharpness + 2;
        }
    }

    {
        if ( this->drawMenuItem( config.camera.vflip ? "Vertical Flip: Enabled##5" : "Vertical flip: Disabled##5", 5) )
        {
            this->menuId = OSDMenuId::VerticalFlip;
            this->selectedItem = config.camera.vflip ? 1: 0;
        }
    }

    if ( this->exitKeyPressed())
    {
        this->menuId = OSDMenuId::Main;
        this->selectedItem = 3;
    }

}

//=======================================================
//=======================================================
void OSDMenu::drawResolutionMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Menu -> Resolution" );
    ImGui::Spacing();

    bool saveAndExit = false;

    if ( this->drawMenuItem( "640x360 30fps (16:9)", 0) )
    {
        config.camera.resolution = Resolution::VGA16;
        saveAndExit = true;
    }
    
    if ( this->drawMenuItem( "640x480 30fps (4:3)", 1) )
    {
        config.camera.resolution = Resolution::VGA;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "800x456 30fps (16:9)", 2) )
    {
        config.camera.resolution = Resolution::SVGA16;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "800x600 30fps (4:3)", 3) )
    {
        config.camera.resolution = Resolution::SVGA;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( s_isOV5640 ? "1280x720 30fps (16:9)" : "1280x720 13fps (16:9)", 4) )
    {
        config.camera.resolution = Resolution::HD;
        saveAndExit = true;
    }
    
    if (this->drawMenuItem( s_isOV5640 ? "1280x1024 30fps (4:3)" : "1280x1024 13fps (4:3)", 5) )
    {
        config.camera.resolution = Resolution::SXGA;
        saveAndExit = true;
    }

    if ( saveAndExit )
    {
        saveGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->menuId = OSDMenuId::Main;
        this->selectedItem = 0;
    }
}

//=======================================================
//=======================================================
bool OSDMenu::exitKeyPressed()
{
    return ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_R) || ImGui::IsKeyPressed(ImGuiKey_G) 
    || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_MouseRight);
}

//=======================================================
//=======================================================
void OSDMenu::drawBrightnessMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Picture Settings -> Brightness" );
    ImGui::Spacing();

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
        saveGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->menuId = OSDMenuId::PictureSettings;
        this->selectedItem = 0;
    }
}


//=======================================================
//=======================================================
void OSDMenu::drawContrastMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Picture Settings -> Contrast" );
    ImGui::Spacing();

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
        saveGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->menuId = OSDMenuId::PictureSettings;
        this->selectedItem = 1;
    }
}

//=======================================================
//=======================================================
void OSDMenu::drawExposureMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Picture Settings -> Exposure" );
    ImGui::Spacing();

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
        saveGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->menuId = OSDMenuId::PictureSettings;
        this->selectedItem = 2;
    }
}

//=======================================================
//=======================================================
void OSDMenu::drawSaturationMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Picture Settings -> Saturation" );
    ImGui::Spacing();

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
        saveGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->menuId = OSDMenuId::PictureSettings;
        this->selectedItem = 3;
    }
}

//=======================================================
//=======================================================
void OSDMenu::drawSharpnessMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Picture Settings -> Sharpness" );
    ImGui::Spacing();

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

    if ( this->drawMenuItem( "Adaptive", 5) )
    {
        config.camera.sharpness = 3;
        saveAndExit = true;
    }

    if ( saveAndExit )
    {
        saveGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->menuId = OSDMenuId::PictureSettings;
        this->selectedItem = 4;
    }
}

//=======================================================
//=======================================================
void OSDMenu::drawVerticalFlipMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Picture Settings -> Vert.Flip" );
    ImGui::Spacing();

    bool saveAndExit = false;

    if ( this->drawMenuItem( "Disabled", 0) )
    {
        config.camera.vflip = false;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "Enabled", 1) )
    {
        config.camera.vflip = true;
        saveAndExit = true;
    }

    if ( saveAndExit )
    {
        saveGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->menuId = OSDMenuId::PictureSettings;
        this->selectedItem = 5;
    }
}

//=======================================================
//=======================================================
void OSDMenu::drawLetterboxMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Menu -> Letterbox" );
    ImGui::Spacing();

    bool saveAndExit = false;

    if ( this->drawMenuItem( "Stretch", 0) )
    {
        s_groundstation_config.screenAspectRatio = ScreenAspectRatio::STRETCH;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "Screen is 4:3", 1) )
    {
        s_groundstation_config.screenAspectRatio = ScreenAspectRatio::ASPECT4X3;
        saveAndExit = true;
    }


    if ( this->drawMenuItem( "Screen is 16:9", 2) )
    {
        s_groundstation_config.screenAspectRatio = ScreenAspectRatio::ASPECT16X9;
        saveAndExit = true;
    }

    if ( saveAndExit )
    {
        saveGroundStationConfig();
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->menuId = OSDMenuId::Main;
        this->selectedItem = 4;
    }
}

//=======================================================
//=======================================================
void OSDMenu::drawWifiRateMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Menu -> Wifi Rate" );
    ImGui::Spacing();

    bool saveAndExit = false;

    if ( this->drawMenuItem( "12Mbps", 0) )
    {
        config.wifi_rate = WIFI_Rate::RATE_G_12M_ODFM;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "18Mbps", 1) )
    {
        config.wifi_rate = WIFI_Rate::RATE_G_18M_ODFM;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "24Mbps", 2) )
    {
        config.wifi_rate = WIFI_Rate::RATE_G_24M_ODFM;
        saveAndExit = true;
    }

    if ( this->drawMenuItem( "36Mbps", 3) )
    {
        config.wifi_rate = WIFI_Rate::RATE_G_36M_ODFM;
        saveAndExit = true;
    }

    if ( saveAndExit )
    {
        saveGround2AirConfig(config);
    }

    if ( saveAndExit || this->exitKeyPressed())
    {
        this->menuId = OSDMenuId::Main;
        this->selectedItem = 2;
    }
}

//=======================================================
//=======================================================
void OSDMenu::drawWifiChannelMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Menu -> Wifi Channel" );
    ImGui::Spacing();

    bool saveAndExit = false;
    bool bExit = false;

    for ( int i = 0; i < 13; i++ )
    {
        char buf[12];
        sprintf(buf, "%d", i+1);
        if ( this->drawMenuItem( buf, i, true) )
        {
            if ( s_groundstation_config.wifi_channel != (i+1)) 
            {
                s_groundstation_config.wifi_channel = i+1;
                saveAndExit = true;
            }
            else
            {
                bExit = true;
            }
        }
    }

    if ( saveAndExit )
    {
        saveGroundStationConfig();

        restart_tp = Clock::now();
        bRestart = true;

        this->menuId = OSDMenuId::Restart;
    }

    if ( bExit || this->exitKeyPressed() )
    {
        this->menuId = OSDMenuId::Main;
        this->selectedItem = 1;
    }
}

//=======================================================
//=======================================================
void OSDMenu::drawRestartMenu(Ground2Air_Config_Packet& config)
{
    this->drawMenuTitle( "Restarting..." );
}
