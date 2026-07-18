#pragma once

constexpr float kRuntimeMenuImageBrightness = 0.60f;

struct RuntimeMenuUiState
{
    bool visible = false;
    bool vr_mode = false;
    bool touch_nav_enabled = false;
    float surface_width = 0.0f;
    float surface_height = 0.0f;
    bool gs_recording = false;
    bool air_recording = false;
};

void drawRuntimeMenuOverlay(const RuntimeMenuUiState& state);
void drawRuntimeMenuTouchNav(const RuntimeMenuUiState& state);
void drawPlaybackProgressOverlay(float overlay_width, float surface_height);
