#pragma once

#include <cstdint>

#include "gs_shared_runtime.h"

void toggleGSRecording(int width, int height, const char* reason);
void logInvalidDesktopSessionEvent(const gs::core::SessionEvent& event, uint32_t current_frame_index);
void handleDesktopVideoDispatch(const VideoDispatchDecision& video_decision);
void handleDesktopTelemetryDispatch(const TelemetryDispatchDecision& telemetry_decision);
void handleDesktopOsdDispatch(const OsdDispatchDecision& osd_decision);
