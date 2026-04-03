#include "gs_runtime_event_pipeline.h"

#include "gs_runtime_event_flow.h"

RuntimeEventClass processAndDispatchSessionEvent(const ProcessedSessionPacket& processed_packet,
                                                 gs::core::VideoFrameAssembler& assembler,
                                                 gs::core::GsSessionCore& session,
                                                 bool restored_by_fec,
                                                 const SessionEventPipelineDispatch& dispatch)
{
    const gs::core::SessionEvent& session_event = processed_packet.event;
    const RuntimeEventClass event_class = classifyRuntimeEvent(session_event.kind);
    switch (event_class)
    {
    case RuntimeEventClass::ConnectAccepted:
        if (dispatch.on_connect_accepted)
        {
            dispatch.on_connect_accepted(session_event);
        }
        break;
    case RuntimeEventClass::Invalid:
        if (dispatch.on_invalid)
        {
            dispatch.on_invalid(session_event);
        }
        break;
    case RuntimeEventClass::Config:
        if (dispatch.on_config)
        {
            dispatch.on_config(session_event);
        }
        break;
    case RuntimeEventClass::RuntimeData:
    {
        const ProcessedRuntimeEvent processed_event =
            processRuntimeSessionEvent(session_event,
                                       assembler,
                                       session,
                                       restored_by_fec,
                                       processed_packet.processed_tp);
        dispatchProcessedRuntimeEvent(processed_event, dispatch.runtime);
        break;
    }
    case RuntimeEventClass::Ignore:
    default:
        break;
    }

    return event_class;
}
