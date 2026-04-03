#include "gs_runtime_event_flow.h"

ProcessedRuntimeEvent processRuntimeSessionEvent(const gs::core::SessionEvent& event,
                                                 gs::core::VideoFrameAssembler& assembler,
                                                 gs::core::GsSessionCore& session,
                                                 bool restored_by_fec,
                                                 Clock::time_point now)
{
    ProcessedRuntimeEvent result;
    result.kind = event.kind;

    switch (event.kind)
    {
    case gs::core::SessionEventKind::VideoPacket:
        result.video = processVideoSessionEvent(event,
                                                assembler,
                                                session,
                                                restored_by_fec,
                                                now);
        result.video_decision = buildVideoDispatchDecision(result.video, restored_by_fec);
        break;
    case gs::core::SessionEventKind::TelemetryPayload:
        result.telemetry = processTelemetrySessionEvent(event);
        result.telemetry_decision = buildTelemetryDispatchDecision(result.telemetry);
        break;
    case gs::core::SessionEventKind::OsdUpdate:
        result.osd = processOsdSessionEvent(event);
        result.osd_decision = buildOsdDispatchDecision(result.osd, false);
        break;
    default:
        break;
    }

    return result;
}
