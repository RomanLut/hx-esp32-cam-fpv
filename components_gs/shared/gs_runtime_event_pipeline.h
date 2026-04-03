#pragma once

#include <functional>

#include "gs_runtime_event_classify.h"
#include "gs_runtime_event_dispatch.h"
#include "gs_runtime_packet_flow.h"

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
