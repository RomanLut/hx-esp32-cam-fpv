#pragma once

#include "gs_asset_flight_osd.h"

#include <vector>
#include <stdint.h>
#include <string>

#include "packets.h"

//======================================================
//======================================================
class LinuxOSD : public GsAssetFlightOsd
{
public:
    LinuxOSD();
    void init();
    void loadFont(const  char* fontName);
    void draw();
    void draw(int surface_width,
              int surface_height,
              int frame_width,
              int frame_height,
              int screen_mode,
              bool vr_mode) override;
    bool isFontError();
};

extern LinuxOSD g_osd;


