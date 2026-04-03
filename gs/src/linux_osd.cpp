#include "linux_osd.h"
#include "imgui.h"
#include "gs_linux_runtime.h"
#include "gs_shared_state.h"

namespace
{

float targetAspectRatio()
{
    const ImVec2 display_size = ImGui::GetIO().DisplaySize;
    if (display_size.x <= 0.0f || display_size.y <= 0.0f)
    {
        return 0.0f;
    }

    switch (s_groundstation_config.screenAspectRatio)
    {
    case ScreenAspectRatio::STRETCH:
        return 0.0f;
    case ScreenAspectRatio::ASPECT5X4:
        return 5.0f / 4.0f;
    case ScreenAspectRatio::ASPECT4X3:
        return 4.0f / 3.0f;
    case ScreenAspectRatio::ASPECT16X9:
        return 16.0f / 9.0f;
    case ScreenAspectRatio::ASPECT16X10:
        return 16.0f / 10.0f;
    case ScreenAspectRatio::LETTERBOX:
    default:
        return display_size.x / display_size.y;
    }
}

}

LinuxOSD g_osd;

//======================================================
//======================================================
LinuxOSD::LinuxOSD()
{
}

//======================================================
//======================================================
void LinuxOSD::init()
{
}

//======================================================
//======================================================
void LinuxOSD::loadFont(const char* fontName)
{
    setFontName(fontName);
    ensureFont();
}

//======================================================
//======================================================
void LinuxOSD::draw()
{
    const ImVec2 display_size = ImGui::GetIO().DisplaySize;
    const float video_aspect = s_decoder.isAspect16x9() ? (16.0f / 9.0f) : (4.0f / 3.0f);
    if (s_groundstation_config.vrMode)
    {
        const int half_width = static_cast<int>(display_size.x) / 2;
        OSDBase::drawFittedInRect(0,
                                  0,
                                  half_width,
                                  static_cast<int>(display_size.y),
                                  video_aspect,
                                  targetAspectRatio());
        OSDBase::drawFittedInRect(half_width,
                                  0,
                                  static_cast<int>(display_size.x) - half_width,
                                  static_cast<int>(display_size.y),
                                  video_aspect,
                                  targetAspectRatio());
    }
    else
    {
        OSDBase::drawFittedInRect(0,
                                  0,
                                  static_cast<int>(display_size.x),
                                  static_cast<int>(display_size.y),
                                  video_aspect,
                                  targetAspectRatio());
    }
}

//======================================================
//======================================================
void LinuxOSD::draw(int /* surface_width */,
                    int /* surface_height */,
                    int /* frame_width */,
                    int /* frame_height */,
                    int /* screen_mode */,
                    bool /* vr_mode */)
{
    draw();
}

//======================================================
//======================================================
bool LinuxOSD::isFontError()
{
    return GsAssetFlightOsd::isFontError();
}
