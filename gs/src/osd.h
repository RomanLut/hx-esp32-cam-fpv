#pragma once

#include <vector>
#include <stdint.h>
#include <string>

#include "core/osd_base.h"
#include "fontwalksnail.h"
#include "packets.h"

//======================================================
//======================================================
class DesktopOSD : public gs::core::OSDBase
{
private:
    FontWalksnail* font;

    std::vector<std::string> getFontsList();

public:
    char currentFontName[256];
    std::vector<std::string> fontsList;

    DesktopOSD();
    void init();
    void loadFont(const  char* fontName);
    void draw();
    bool isFontError();

protected:
    void drawChar(uint16_t code, int x, int y, int width, int height) override;
};

extern DesktopOSD g_osd;


