#include "gs_runtime_event_dispatch.h"

void dispatchProcessedRuntimeEvent(const ProcessedRuntimeEvent& event,
                                  const RuntimeEventDispatch& dispatch)
{
    if (event.kind == gs::core::SessionEventKind::VideoPacket && dispatch.on_video)
    {
        dispatch.on_video(event.video, event.video_decision);
    }
}
