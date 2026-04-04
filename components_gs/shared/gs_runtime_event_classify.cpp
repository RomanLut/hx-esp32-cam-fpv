#include "gs_runtime_event_classify.h"

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
    case gs::core::SessionEventKind::OsdUpdate:
        return RuntimeEventClass::RuntimeData;
    default:
        return RuntimeEventClass::Ignore;
    }
}

bool isInvalidRuntimeEvent(gs::core::SessionEventKind kind)
{
    return classifyRuntimeEvent(kind) == RuntimeEventClass::Invalid;
}
