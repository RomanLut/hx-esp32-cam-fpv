#include "osd_menu.h"
#include <cmath>

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
}

//=======================================================
//=======================================================
void OSDMenu::drawMenuItem( const char* caption, int itemIndex )
{
    bool focused = this->selectedItem == itemIndex;

    ImGui::Indent();

    ImVec4 color = focused ? (ImVec4)ImColor(77,137,205) : (ImVec4)ImColor(37,51,88);
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.02f, 0.5f));
    ImGui::Button(caption, ImVec2( this->bWidth, this->bHeight ) );
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    ImGui::Unindent();
}

//=======================================================
//=======================================================
void OSDMenu::draw(Ground2Air_Config_Packet& config)
{
    if (!this->visible) 
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter))
        {
            this->visible = true;
            this->menuId = OSDMenuId::Main;
            this->selectedItem = 0;
        }
        else
        {
            return;
        }
    }

    char buf[256];
    sprintf(buf, "ESP32-CAM-FPV v%s###OSD_MENU", "0.1");

    int windowWidth = 500;
    int windowHeight = 350;

    this->bWidth = windowWidth - 58;
    this->bHeight = 35;

    ImVec2 screenSize = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(floor( (screenSize.x - windowWidth) / 2) , floor( (screenSize.y - windowHeight) /2 )), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight));
    ImGui::Begin(buf, NULL, ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoMove |
                            ImGuiWindowFlags_NoScrollbar |
                            ImGuiWindowFlags_NoCollapse |
                            ImGuiWindowFlags_NoNav |
                            ImGuiWindowFlags_NoTitleBar |
                            ImGuiWindowFlags_NoBackground
                            );

    switch (this->menuId)
    {
        case OSDMenuId::Main: this->drawMainMenu(); break;
        case OSDMenuId::PictureSettings: this->drawPictureSettingsMenu(); break;
        case OSDMenuId::Resolution: this->drawResolutionMenu(); break;
    }

    ImGui::End();
}

//=======================================================
//=======================================================
void OSDMenu::drawMainMenu()
{
    this->drawMenuTitle( "Main" );
    ImGui::Dummy(ImVec2(0.0f, 20.0f));
    this->drawMenuItem( "Resolution: 800x456", 0);
    this->drawMenuItem( "Wifi Channel: 7", 1);
    this->drawMenuItem( "Wifi Rate: 24Mbps", 2);
    this->drawMenuItem( "Image settings...", 3);
    this->drawMenuItem( "Letterbox: Screen is 16x9", 4);
    //this->drawMenuItem( "OSD...", 5);
}

//=======================================================
//=======================================================
void OSDMenu::drawPictureSettingsMenu()
{
    this->drawMenuTitle( "Main -> Image Settings" );
    
    this->drawMenuItem( "Brightness: 0", 0);
    this->drawMenuItem( "Contrast: 0", 1);
    this->drawMenuItem( "Exposure: 0", 2);
    this->drawMenuItem( "Saturation: 0", 3);
    this->drawMenuItem( "Sharpness: 0", 4);
    this->drawMenuItem( "Vertical flip: No", 5);
}

//=======================================================
//=======================================================
void OSDMenu::drawResolutionMenu()
{
    this->drawMenuTitle( "Main -> Resolution" );
    ImGui::Spacing();
    this->drawMenuItem( "640x360 30fps", 0);
    this->drawMenuItem( "640x480 30fps", 1);
    this->drawMenuItem( "800x456 30fps", 2);
    this->drawMenuItem( s_isOV5640 ? "1280x1024 13fps" : "1280x1024 30fps", 3);
    this->drawMenuItem( s_isOV5640 ? "1280x720 13fps" : "1280x720 30fps", 4);
}
