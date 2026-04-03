#pragma once

#include "core/gs_session_core.h"

enum class RuntimeEventClass
{
    Ignore,
    ConnectAccepted,
    Invalid,
    Config,
    RuntimeData,
};

RuntimeEventClass classifyRuntimeEvent(gs::core::SessionEventKind kind);
bool isInvalidRuntimeEvent(gs::core::SessionEventKind kind);
