#pragma once

#include "main.h"
#include "imgui.h"
#include "packets.h"

//=======================================================
//=======================================================
enum class OSDMenuId
{
    Main,
    PictureSettings,
    Resolution
};

//=======================================================
//=======================================================
class OSDMenu
{
public:
    OSDMenu();
    
    void init();
    void draw( Ground2Air_Config_Packet& config );

private:
    bool visible;

    OSDMenuId menuId;
    int selectedItem;

    int bWidth;
    int bHeight;

    void drawMenuTitle( const char* caption );
    void drawMenuItem( const char* caption, int itemIndex );

    void drawMainMenu();
    void drawPictureSettingsMenu();
    void drawResolutionMenu();
};

extern OSDMenu g_osdMenu;
