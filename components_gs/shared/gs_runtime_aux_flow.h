#pragma once

#include <cstddef>
#include <cstdint>

#include "core/gs_session_core.h"

struct ProcessedOsdEvent
{
    const uint8_t* payload = nullptr;
    uint16_t payload_size = 0;
    bool has_update = false;
};

struct OsdDispatchDecision
{
    bool should_apply = false;
    const uint8_t* payload = nullptr;
    uint16_t payload_size = 0;
};

bool shouldApplyOsdUpdate(const ProcessedOsdEvent& event, bool updates_suppressed);
OsdDispatchDecision buildOsdDispatchDecision(const ProcessedOsdEvent& event, bool updates_suppressed);

ProcessedOsdEvent processOsdSessionEvent(const gs::core::SessionEvent& event);
