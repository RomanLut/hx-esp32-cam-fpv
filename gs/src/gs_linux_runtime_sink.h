#pragma once

#include <cstdint>

#include "gs_shared_runtime.h"

void logInvalidLinuxSessionEvent(const gs::core::SessionEvent& event, uint32_t current_frame_index);
void handleLinuxVideoDispatch(const VideoDispatchDecision& video_decision);
