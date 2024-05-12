#include "osd_menu.h"


OSDMenu g_OSDMenu;

//=======================================================
//=======================================================
OSDMenu::OSDMenu()
{
    this->visible = true;
}

//=======================================================
//=======================================================
void OSDMenu::draw()
{
    if (!this->visible) return;

/*
    ImGui::Begin(buf);
    {
        {
            int value = config.wifi_power;
            ImGui::SliderInt("WIFI Power", &value, 0, 20); 
            config.wifi_power = value;
        }
    }
    ImGui::End();
*/
    
}