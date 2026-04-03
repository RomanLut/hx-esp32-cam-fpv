#pragma once

#include <cstdint>
#include <functional>

#include "gs_runtime_frame_ui.h"
#include "gs_runtime_overlay_state.h"
#include "gs_shared_runtime.h"

struct RuntimeUiContext
{
    bool wifi_channel_apply_pending = false;
    bool restart_required = false;
    bool osd_font_error = false;
    float gs_temp_celsius = 0.0f;
    uint64_t gs_sd_free_space_bytes = 0;
    uint64_t gs_sd_total_space_bytes = 0;
    std::function<void()> drawFlightOsd;
    std::function<void(bool enabled, bool apply)> setVsync;
    std::function<void()> toggleGsRecording;
    std::function<void()> requestRestart;
    std::function<void()> requestExit;
};

void drawRuntimeDebugSettingsWindow(Ground2Air_Config_Packet& config, const RuntimeUiContext& context);
RuntimeFrameUiState buildLinuxRuntimeFrameUiState(const Ground2Air_Config_Packet& config, const RuntimeUiContext& context);
void drawRuntimeFrameUi(const RuntimeFrameUiState& state, const RuntimeUiContext& context);
