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
// Computed positions and dimensions for the touch navigation pad cells.
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

//===================================================================================
//===================================================================================
// Axis-aligned ROI rectangle in the same screen pixel space as VideoQuad x/y.
struct StabilizationRoiScreenRect
{
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
};

bool computeStabilizationRoiScreenRect(float layout_rect_x,
                                       float layout_rect_y,
                                       float layout_rect_w,
                                       float layout_rect_h,
                                       int frame_width,
                                       int frame_height,
                                       int screen_mode,
                                       float screen_zoom,
                                       float roi_divisor,
                                       StabilizationRoiScreenRect& out_rect);

bool computeStabilizationRoiScreenRectLetterboxed(int layout_quad_x,
                                                  int layout_y,
                                                  int layout_width,
                                                  int layout_height,
                                                  float video_aspect,
                                                  ScreenAspectRatio screen_aspect_ratio,
                                                  float screen_zoom,
                                                  int frame_width,
                                                  int frame_height,
                                                  float roi_divisor,
                                                  StabilizationRoiScreenRect& out_rect);
}
