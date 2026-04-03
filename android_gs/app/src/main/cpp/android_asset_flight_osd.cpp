#include "android_asset_flight_osd.h"

#include "gs_flight_osd_render_shared.h"

AndroidAssetFlightOsd::AndroidAssetFlightOsd()
{
}

AndroidAssetFlightOsd::~AndroidAssetFlightOsd() = default;

void AndroidAssetFlightOsd::draw(int surface_width,
                      int surface_height,
                      int frame_width,
                      int frame_height,
                      int screen_mode,
                      bool vr_mode)
{
    if (!ensureFont())
    {
        return;
    }
    gs::render::drawFlightOsd(*this, surface_width, surface_height, frame_width, frame_height, screen_mode, vr_mode);
}
