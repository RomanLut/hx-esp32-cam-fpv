#pragma once

#include "main.h"
#include "imgui.h"

//=======================================================
//=======================================================
class OSDMenu
{
public:
    OSDMenu();
    
    void init();
    void draw();

private:
    bool visible;
};

extern OSDMenu g_osdMenu;
