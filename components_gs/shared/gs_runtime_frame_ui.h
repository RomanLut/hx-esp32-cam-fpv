#pragma once

#include <string>

#include "gs_shared_state.h"

//===================================================================================
//===================================================================================
// Carries shared frame UI state excluding the top overlay payload.
struct RuntimeFrameUiState
{
    bool overlay_stats_visible = false;
    std::string menu_footer;
    ScreenAspectRatio screen_mode = ScreenAspectRatio::LETTERBOX;
    bool vsync = true;
    bool vr_mode = false;
    bool flight_osd_is_16x9 = false;
};
