#pragma once

#include <functional>

struct RuntimeMenuUiState
{
    bool visible = false;
    bool vr_mode = false;
    bool touch_nav_enabled = false;
    float surface_width = 0.0f;
    float surface_height = 0.0f;
};

void drawRuntimeMenuUi(const RuntimeMenuUiState& state, const std::function<void()>& draw_menu);
