#pragma once

#include <cstdint>

#include "Clock.h"
#include "core/gs_session_core.h"
#include "core/video_frame_assembler.h"
#include "gs_runtime_aux_flow.h"
#include "gs_runtime_video_flow.h"

struct ProcessedRuntimeEvent
{
    gs::core::SessionEventKind kind = gs::core::SessionEventKind::Ignore;
    ProcessedVideoEvent video = {};
    ProcessedOsdEvent osd = {};
    VideoDispatchDecision video_decision = {};
    OsdDispatchDecision osd_decision = {};
};

ProcessedRuntimeEvent processRuntimeSessionEvent(const gs::core::SessionEvent& event,
                                                 gs::core::VideoFrameAssembler& assembler,
                                                 gs::core::GsSessionCore& session,
                                                 bool restored_by_fec,
                                                 Clock::time_point now);
