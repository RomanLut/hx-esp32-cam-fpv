#include "gs_runtime_event_flow.h"

//===================================================================================
//===================================================================================
// Maps a low-level session event kind to the runtime event category used by the pipeline.
RuntimeEventClass classifyRuntimeEvent(gs::core::SessionEventKind kind)
{
    switch (kind)
    {
    case gs::core::SessionEventKind::Ignore:
        return RuntimeEventClass::Ignore;
    case gs::core::SessionEventKind::ConnectAccepted:
        return RuntimeEventClass::ConnectAccepted;
    case gs::core::SessionEventKind::InvalidVideoPacket:
    case gs::core::SessionEventKind::InvalidTelemetryPacket:
    case gs::core::SessionEventKind::InvalidOsdPacket:
    case gs::core::SessionEventKind::UnsupportedPacket:
        return RuntimeEventClass::Invalid;
    case gs::core::SessionEventKind::ConfigReceived:
        return RuntimeEventClass::Config;
    case gs::core::SessionEventKind::VideoPacket:
        return RuntimeEventClass::RuntimeData;
    default:
        return RuntimeEventClass::Ignore;
    }
}

//===================================================================================
//===================================================================================
// Returns true when the session event kind represents an invalid runtime packet.
bool isInvalidRuntimeEvent(gs::core::SessionEventKind kind)
{
    return classifyRuntimeEvent(kind) == RuntimeEventClass::Invalid;
}

//===================================================================================
//===================================================================================
// Processes a session event into runtime-specific state and dispatch-ready payloads.
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
    default:
        break;
    }

    return result;
}
