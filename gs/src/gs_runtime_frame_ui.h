#pragma once

#include <string>

#include "gs_runtime_overlay_state.h"

struct RuntimeFrameUiState
{
    RuntimeOverlayState overlay;
    std::string menu_footer;
    int screen_mode = 1;
    bool vsync = true;
    bool vr_mode = false;
};

void drawRuntimeFrameUiContent(const RuntimeFrameUiState& state);
