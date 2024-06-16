#pragma once

#include <vector>

#include "fontwalksnail.h"
#include "packets.h"

//======================================================
//======================================================
class OSD
{
private:
    FontWalksnail* font;
    OSDBuffer buffer;

    std::vector<std::string> getFontsList();

public:
    char currentFontName[256];
    std::vector<std::string> fontsList;

    OSD();
    void init();
    void loadFont(const  char* fontName);
    void draw();
    void update(void* pScreen);
    bool isFontError();
};

extern OSD g_osd;


