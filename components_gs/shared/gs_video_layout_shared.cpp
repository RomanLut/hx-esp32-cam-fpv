#include "gs_video_layout_shared.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float kTouchNavButtonSize = 108.0f;
constexpr float kTouchNavGap = 15.0f;
constexpr float kTouchNavMargin = 27.0f;
}

namespace gs::render
{
//===================================================================================
//===================================================================================
// Computes a letterboxed (or pillarboxed) rectangle that fits the given video
// aspect ratio within the supplied screen region, respecting the screen aspect
// ratio override setting.
RectI buildLetterboxedRect(int origin_x,
                           int origin_y,
                           int rect_width,
                           int rect_height,
                           float video_aspect,
                           ScreenAspectRatio screen_aspect_ratio)
{
    RectI rect = {
        origin_x,
        origin_y,
        origin_x + rect_width,
        origin_y + rect_height,
    };
    if (rect_width <= 0 || rect_height <= 0 || video_aspect <= 0.0f)
    {
        return rect;
    }

    float screen_aspect = static_cast<float>(rect_width) / static_cast<float>(rect_height);
    if (screen_aspect_ratio == ScreenAspectRatio::ASPECT5X4)
    {
        screen_aspect = 5.0f / 4.0f;
    }
    else if (screen_aspect_ratio == ScreenAspectRatio::ASPECT4X3)
    {
        screen_aspect = 4.0f / 3.0f;
    }
    else if (screen_aspect_ratio == ScreenAspectRatio::ASPECT16X9)
    {
        screen_aspect = 16.0f / 9.0f;
    }
    else if (screen_aspect_ratio == ScreenAspectRatio::ASPECT16X10)
    {
        screen_aspect = 16.0f / 10.0f;
    }

    if (screen_aspect_ratio == ScreenAspectRatio::STRETCH ||
        static_cast<int>(video_aspect * 100.0f + 0.5f) == static_cast<int>(screen_aspect * 100.0f + 0.5f))
    {
        return rect;
    }

    if (video_aspect > screen_aspect)
    {
        const int scaled_height = static_cast<int>(rect_height * screen_aspect / video_aspect + 0.5f);
        rect.y1 = origin_y + (rect_height - scaled_height) / 2;
        rect.y2 = rect.y1 + scaled_height;
    }
    else
    {
        const int scaled_width = static_cast<int>(rect_width * video_aspect / screen_aspect + 0.5f);
        rect.x1 = origin_x + (rect_width - scaled_width) / 2;
        rect.x2 = rect.x1 + scaled_width;
    }

    return rect;
}

//===================================================================================
//===================================================================================
// Computes scaled positions and sizes for the touch navigation pad cells
// based on the surface dimensions.
NavPadLayout buildTouchNavPadLayout(int surface_width, int surface_height)
{
    NavPadLayout layout;
    const float ui_scale = std::min(static_cast<float>(surface_width) / 1280.0f,
                                    static_cast<float>(surface_height) / 720.0f);
    const float control_scale = std::max(0.85f, ui_scale);
    layout.size = kTouchNavButtonSize * control_scale;
    layout.gap = kTouchNavGap * control_scale;
    layout.margin = kTouchNavMargin * control_scale;
    layout.right_x = static_cast<float>(surface_width) - layout.margin - layout.size;
    layout.left_x = layout.right_x - layout.size - layout.gap - layout.size;
    layout.center_x = layout.left_x + layout.size + layout.gap * 0.5f;
    layout.down_y = static_cast<float>(surface_height) - layout.margin - layout.size;
    layout.mid_y = layout.down_y - layout.gap - layout.size;
    layout.up_y = layout.mid_y - layout.gap - layout.size;
    return layout;
}

//===================================================================================
//===================================================================================
// Builds a VideoQuad for rendering a video frame into a rectangle.
// screen_mode 1 = letterbox/pillarbox, screen_mode 2 = crop-to-fill.
VideoQuad buildVideoQuad(float rect_x,
                         float rect_y,
                         float rect_width,
                         float rect_height,
                         int frame_width,
                         int frame_height,
                         int screen_mode)
{
    VideoQuad quad;
    quad.x = rect_x;
    quad.y = rect_y;
    quad.width = rect_width;
    quad.height = rect_height;

    if (frame_width <= 0 || frame_height <= 0 || rect_width <= 0.0f || rect_height <= 0.0f)
    {
        return quad;
    }

    const float video_aspect = static_cast<float>(frame_width) / static_cast<float>(frame_height);
    const float rect_aspect = rect_width / rect_height;

    if (screen_mode == 1)
    {
        if (video_aspect > rect_aspect)
        {
            quad.height = rect_width / video_aspect;
            quad.y = rect_y + (rect_height - quad.height) * 0.5f;
        }
        else
        {
            quad.width = rect_height * video_aspect;
            quad.x = rect_x + (rect_width - quad.width) * 0.5f;
        }
    }
    else if (screen_mode == 2)
    {
        if (video_aspect > rect_aspect)
        {
            const float visible = rect_aspect / video_aspect;
            const float margin = (1.0f - visible) * 0.5f;
            quad.u0 = margin;
            quad.u1 = 1.0f - margin;
        }
        else
        {
            const float visible = video_aspect / rect_aspect;
            const float margin = (1.0f - visible) * 0.5f;
            quad.v1 = margin;
            quad.v0 = 1.0f - margin;
        }
    }

    return quad;
}

//===================================================================================
//===================================================================================
// Maps the stabilization feature ROI from frame pixels to screen pixels. Geometry
// matches OpenCV wrapper BuildRoi (VideoStabilizer.cpp); must stay in sync if that
// ROI definition changes.
bool computeStabilizationRoiScreenRect(float layout_rect_x,
                                       float layout_rect_y,
                                       float layout_rect_w,
                                       float layout_rect_h,
                                       int frame_width,
                                       int frame_height,
                                       int screen_mode,
                                       float screen_zoom,
                                       float roi_divisor,
                                       StabilizationRoiScreenRect& out_rect)
{
    if (frame_width <= 0 || frame_height <= 0 || layout_rect_w <= 0.0f || layout_rect_h <= 0.0f)
    {
        return false;
    }
    if (roi_divisor <= 1.05f)
    {
        return false;
    }

    VideoQuad quad = buildVideoQuad(layout_rect_x,
                                     layout_rect_y,
                                     layout_rect_w,
                                     layout_rect_h,
                                     frame_width,
                                     frame_height,
                                     screen_mode);
    std::swap(quad.v0, quad.v1);

    if (screen_zoom != 1.0f)
    {
        const float cx = quad.x + quad.width * 0.5f;
        const float cy = quad.y + quad.height * 0.5f;
        quad.width *= screen_zoom;
        quad.height *= screen_zoom;
        quad.x = cx - quad.width * 0.5f;
        quad.y = cy - quad.height * 0.5f;
    }

    const int fw = frame_width;
    const int fh = frame_height;
    const int margin_x = static_cast<int>(static_cast<float>(fw) / roi_divisor);
    const int margin_y = static_cast<int>(static_cast<float>(fh) / roi_divisor);
    const int rx = std::clamp(margin_x, 0, fw - 1);
    const int ry = std::clamp(margin_y, 0, fh - 1);
    const int rw = std::max(1, fw - rx * 2);
    const int rh = std::max(1, fh - ry * 2);

    const float inv_fw = 1.0f / static_cast<float>(fw);
    const float inv_fh = 1.0f / static_cast<float>(fh);
    const float nx0 = static_cast<float>(rx) * inv_fw;
    const float ny0 = static_cast<float>(ry) * inv_fh;
    const float nx1 = static_cast<float>(rx + rw) * inv_fw;
    const float ny1 = static_cast<float>(ry + rh) * inv_fh;

    out_rect.min_x = quad.x + nx0 * quad.width;
    out_rect.max_x = quad.x + nx1 * quad.width;
    out_rect.min_y = quad.y + ny0 * quad.height;
    out_rect.max_y = quad.y + ny1 * quad.height;
    return true;
}

namespace
{
//===================================================================================
//===================================================================================
// Zooms a letterboxed integer rect about its center. Matches PI_HAL applyScreenZoom
// so Linux ROI overlay aligns with GL video placement.
RectI applyScreenZoomToRectI(const RectI& rect, float zoom)
{
    if (zoom == 1.0f)
    {
        return rect;
    }

    const float center_x = (static_cast<float>(rect.x1) + static_cast<float>(rect.x2)) * 0.5f;
    const float center_y = (static_cast<float>(rect.y1) + static_cast<float>(rect.y2)) * 0.5f;
    const float width = static_cast<float>(rect.x2 - rect.x1) * zoom;
    const float height = static_cast<float>(rect.y2 - rect.y1) * zoom;

    return {
        static_cast<int>(std::round(center_x - width * 0.5f)),
        static_cast<int>(std::round(center_y - height * 0.5f)),
        static_cast<int>(std::round(center_x + width * 0.5f)),
        static_cast<int>(std::round(center_y + height * 0.5f))};
}
}

//===================================================================================
//===================================================================================
// Linux GS video uses buildLetterboxedRect + screen zoom (PI_HAL) instead of
// buildVideoQuad(screen_mode); this path must match that layout.
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
                                                  StabilizationRoiScreenRect& out_rect)
{
    if (frame_width <= 0 || frame_height <= 0 || layout_width <= 0 || layout_height <= 0)
    {
        return false;
    }
    if (roi_divisor <= 1.05f || video_aspect <= 0.0f)
    {
        return false;
    }

    const RectI letterboxed =
        buildLetterboxedRect(layout_quad_x, layout_y, layout_width, layout_height, video_aspect, screen_aspect_ratio);
    const RectI zoomed = applyScreenZoomToRectI(letterboxed, screen_zoom);
    const float qx = static_cast<float>(zoomed.x1);
    const float qy = static_cast<float>(zoomed.y1);
    const float qw = static_cast<float>(zoomed.x2 - zoomed.x1);
    const float qh = static_cast<float>(zoomed.y2 - zoomed.y1);
    if (qw <= 0.0f || qh <= 0.0f)
    {
        return false;
    }

    const int fw = frame_width;
    const int fh = frame_height;
    const int margin_x = static_cast<int>(static_cast<float>(fw) / roi_divisor);
    const int margin_y = static_cast<int>(static_cast<float>(fh) / roi_divisor);
    const int rx = std::clamp(margin_x, 0, fw - 1);
    const int ry = std::clamp(margin_y, 0, fh - 1);
    const int rw = std::max(1, fw - rx * 2);
    const int rh = std::max(1, fh - ry * 2);

    const float inv_fw = 1.0f / static_cast<float>(fw);
    const float inv_fh = 1.0f / static_cast<float>(fh);
    const float nx0 = static_cast<float>(rx) * inv_fw;
    const float ny0 = static_cast<float>(ry) * inv_fh;
    const float nx1 = static_cast<float>(rx + rw) * inv_fw;
    const float ny1 = static_cast<float>(ry + rh) * inv_fh;

    out_rect.min_x = qx + nx0 * qw;
    out_rect.max_x = qx + nx1 * qw;
    out_rect.min_y = qy + ny0 * qh;
    out_rect.max_y = qy + ny1 * qh;
    return true;
}
}
