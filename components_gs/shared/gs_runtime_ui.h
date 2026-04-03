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
    std::function<void()> drawFlightOsd;
    std::function<void(Ground2Air_Config_Packet&)> applyWifiChannel;
    std::function<void()> toggleGsRecording;
    std::function<void()> requestRestart;
};

void drawRuntimeDebugSettingsWindow(Ground2Air_Config_Packet& config, const RuntimeUiContext& context);
RuntimeFrameUiState buildLinuxRuntimeFrameUiState(const Ground2Air_Config_Packet& config, const RuntimeUiContext& context);
void drawRuntimeFrameUi(const RuntimeFrameUiState& state, const RuntimeUiContext& context);
