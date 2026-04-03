#pragma once

namespace gs::core
{
class OSDBase;
}

namespace gs::render
{
void drawFlightOsd(gs::core::OSDBase& osd,
                   int surface_width,
                   int surface_height,
                   int frame_width,
                   int frame_height,
                   int screen_mode,
                   bool vr_mode);
}
