#pragma once

#include <vector>
#include <stdint.h>
#include <string>

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
    void update(const uint8_t* pData, uint16_t size);
    bool isFontError();
    void clear();
    void setLowChar( int row, int col, uint8_t c); //high part is not updated
};

extern OSD g_osd;


