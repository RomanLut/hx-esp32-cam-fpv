#pragma once

#include "gs_shared_state.h"

namespace gs::render
{
//===================================================================================
//===================================================================================
// Integer rectangle defined by two corner points.
struct RectI
{
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
};

//===================================================================================
//===================================================================================
// Describes the position, size, and UV coordinates of a rendered video quad.
struct VideoQuad
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float u0 = 0.0f;
    float v0 = 1.0f;
    float u1 = 1.0f;
    float v1 = 0.0f;
};

//===================================================================================
//===================================================================================
// Computed positions and dimensions for the touch navigation pad buttons.
struct NavPadLayout
{
    float size = 0.0f;
    float gap = 0.0f;
    float margin = 0.0f;
    float left_x = 0.0f;
    float right_x = 0.0f;
    float center_x = 0.0f;
    float up_y = 0.0f;
    float mid_y = 0.0f;
    float down_y = 0.0f;
};

NavPadLayout buildTouchNavPadLayout(int surface_width, int surface_height);
RectI buildLetterboxedRect(int origin_x,
                           int origin_y,
                           int rect_width,
                           int rect_height,
                           float video_aspect,
                           ScreenAspectRatio screen_aspect_ratio);
VideoQuad buildVideoQuad(float rect_x,
                         float rect_y,
                         float rect_width,
                         float rect_height,
                         int frame_width,
                         int frame_height,
                         int screen_mode);
}
