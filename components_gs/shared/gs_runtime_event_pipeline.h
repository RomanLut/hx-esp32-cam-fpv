#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "../../components/common/Clock.h"
#include "core/gs_session_core.h"
#include "core/transport.h"
#include "gs_runtime_event_flow.h"

struct ProcessedSessionPacket
{
    gs::core::SessionEvent event = {};
    Clock::time_point processed_tp = Clock::now();
};

struct RuntimeEventDispatch
{
    std::function<void(const ProcessedVideoEvent&, const VideoDispatchDecision&)> on_video;
};

struct SessionEventPipelineDispatch
{
    std::function<void(const gs::core::SessionEvent&)> on_connect_accepted;
    std::function<void(const gs::core::SessionEvent&)> on_invalid;
    std::function<void(const gs::core::SessionEvent&)> on_config;
    RuntimeEventDispatch runtime = {};
};

RuntimeEventClass processAndDispatchSessionEvent(const ProcessedSessionPacket& processed_packet,
                                                 gs::core::VideoFrameAssembler& assembler,
                                                 gs::core::GsSessionCore& session,
                                                 bool restored_by_fec,
                                                 const SessionEventPipelineDispatch& dispatch);
