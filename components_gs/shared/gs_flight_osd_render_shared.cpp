#include "gs_flight_osd_render_shared.h"

#include "core/osd_base.h"

namespace
{
void computeVideoBounds(int surface_width,
                        int surface_height,
                        int frame_width,
                        int frame_height,
                        int screen_mode,
                        int& x1,
                        int& y1,
                        int& x2,
                        int& y2)
{
    x1 = 0;
    y1 = 0;
    x2 = surface_width;
    y2 = surface_height;

    if (surface_width <= 0 || surface_height <= 0 || frame_width <= 0 || frame_height <= 0)
    {
        return;
    }

    if (screen_mode != 1)
    {
        return;
    }

    const float video_aspect = static_cast<float>(frame_width) / static_cast<float>(frame_height);
    const float screen_aspect = static_cast<float>(surface_width) / static_cast<float>(surface_height);

    if (video_aspect > screen_aspect)
    {
        const int fitted_height = static_cast<int>(static_cast<float>(surface_width) / video_aspect);
        const int margin_y = (surface_height - fitted_height) / 2;
        y1 = margin_y;
        y2 = margin_y + fitted_height;
    }
    else
    {
        const int fitted_width = static_cast<int>(static_cast<float>(surface_height) * video_aspect);
        const int margin_x = (surface_width - fitted_width) / 2;
        x1 = margin_x;
        x2 = margin_x + fitted_width;
    }
}
}

namespace gs::render
{
void drawFlightOsd(gs::core::OSDBase& osd,
                   int surface_width,
                   int surface_height,
                   int frame_width,
                   int frame_height,
                   int screen_mode,
                   bool vr_mode)
{
    auto drawInRect = [&osd, frame_width, frame_height, screen_mode](int origin_x, int width, int height)
    {
        int x1 = origin_x;
        int y1 = 0;
        int x2 = origin_x + width;
        int y2 = height;
        computeVideoBounds(width, height, frame_width, frame_height, screen_mode, x1, y1, x2, y2);
        x1 += origin_x;
        x2 += origin_x;
        osd.draw(x1, y1, x2, y2);
    };

    if (vr_mode)
    {
        const int half_width = surface_width / 2;
        drawInRect(0, half_width, surface_height);
        drawInRect(half_width, surface_width - half_width, surface_height);
        return;
    }

    int x1 = 0;
    int y1 = 0;
    int x2 = surface_width;
    int y2 = surface_height;
    computeVideoBounds(surface_width, surface_height, frame_width, frame_height, screen_mode, x1, y1, x2, y2);
    osd.draw(x1, y1, x2, y2);
}
}
