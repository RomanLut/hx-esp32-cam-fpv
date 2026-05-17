#pragma once

#include <cstdint>

#include "../../components/common/Clock.h"
#include "core/gs_session_core.h"
#include "core/video_frame_assembler.h"
#include "gs_runtime_video_flow.h"

//===================================================================================
//===================================================================================
// Classifies session events into high-level runtime handling categories.
enum class RuntimeEventClass
{
    Ignore,
    ConnectAccepted,
    Invalid,
    Config,
    RuntimeData,
};

struct ProcessedRuntimeEvent
{
    gs::core::SessionEventKind kind = gs::core::SessionEventKind::Ignore;
    ProcessedVideoEvent video = {};
    VideoDispatchDecision video_decision = {};
};

//===================================================================================
//===================================================================================
// Maps a low-level session event kind to the runtime event category used by the pipeline.
RuntimeEventClass classifyRuntimeEvent(gs::core::SessionEventKind kind);

//===================================================================================
//===================================================================================
// Returns true when the session event kind represents an invalid runtime packet.
bool isInvalidRuntimeEvent(gs::core::SessionEventKind kind);

//===================================================================================
//===================================================================================
// Processes a session event into runtime-specific state and dispatch-ready payloads.
ProcessedRuntimeEvent processRuntimeSessionEvent(const gs::core::SessionEvent& event,
                                                 gs::core::VideoFrameAssembler& assembler,
                                                 gs::core::GsSessionCore& session,
                                                 bool restored_by_fec,
                                                 Clock::time_point now);
