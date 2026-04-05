#pragma once

#include <functional>

#include "gs_runtime_event_flow.h"

struct RuntimeEventDispatch
{
    std::function<void(const ProcessedVideoEvent&, const VideoDispatchDecision&)> on_video;
};

void dispatchProcessedRuntimeEvent(const ProcessedRuntimeEvent& event,
                                  const RuntimeEventDispatch& dispatch);
