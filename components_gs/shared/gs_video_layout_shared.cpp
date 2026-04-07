#include "gs_video_layout_shared.h"

#include <algorithm>

namespace
{
constexpr float kTouchNavButtonSize = 72.0f;
constexpr float kTouchNavGap = 10.0f;
constexpr float kTouchNavMargin = 18.0f;
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
// Computes scaled positions and sizes for the touch navigation pad buttons
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
    layout.center_x = layout.left_x + layout.size + layout.gap;
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
}
