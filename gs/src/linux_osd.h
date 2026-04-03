#pragma once

#include <vector>
#include <stdint.h>
#include <string>

#include "core/osd_base.h"
#include "fontwalksnail.h"
#include "packets.h"

//======================================================
//======================================================
class LinuxOSD : public gs::core::OSDBase
{
private:
    FontWalksnail* font;

    std::vector<std::string> getFontsList();

public:
    char currentFontName[256];
    std::vector<std::string> fontsList;

    LinuxOSD();
    void init();
    void loadFont(const  char* fontName);
    void draw();
    bool isFontError();

protected:
    void drawChar(uint16_t code, int x, int y, int width, int height) override;
};

extern LinuxOSD g_osd;


