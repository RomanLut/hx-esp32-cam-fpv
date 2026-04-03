#pragma once

#include "gs_asset_flight_osd.h"

#include <vector>

class AndroidAssetFlightOsd final : public GsAssetFlightOsd
{
public:
    AndroidAssetFlightOsd();
    ~AndroidAssetFlightOsd() override;

    void draw(int surface_width,
              int surface_height,
              int frame_width,
              int frame_height,
              int screen_mode,
              bool vr_mode) override;
};
