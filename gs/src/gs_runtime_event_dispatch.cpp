#include "gs_runtime_event_dispatch.h"

void dispatchProcessedRuntimeEvent(const ProcessedRuntimeEvent& event,
                                  const RuntimeEventDispatch& dispatch)
{
    switch (event.kind)
    {
    case gs::core::SessionEventKind::VideoPacket:
        if (dispatch.on_video)
        {
            dispatch.on_video(event.video, event.video_decision);
        }
        break;
    case gs::core::SessionEventKind::TelemetryPayload:
        if (dispatch.on_telemetry)
        {
            dispatch.on_telemetry(event.telemetry, event.telemetry_decision);
        }
        break;
    case gs::core::SessionEventKind::OsdUpdate:
        if (dispatch.on_osd)
        {
            dispatch.on_osd(event.osd, event.osd_decision);
        }
        break;
    default:
        break;
    }
}
