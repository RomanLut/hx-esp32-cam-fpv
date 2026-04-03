#pragma once

#include <cstddef>
#include <cstdint>

#include "core/gs_session_core.h"

struct ProcessedTelemetryEvent
{
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;
    bool has_payload = false;
};

struct ProcessedOsdEvent
{
    const uint8_t* payload = nullptr;
    uint16_t payload_size = 0;
    bool has_update = false;
};

struct TelemetryDispatchDecision
{
    bool has_payload = false;
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;
    size_t inbound_bytes = 0;
};

struct OsdDispatchDecision
{
    bool should_apply = false;
    const uint8_t* payload = nullptr;
    uint16_t payload_size = 0;
};

bool hasTelemetryPayload(const ProcessedTelemetryEvent& event);
bool shouldApplyOsdUpdate(const ProcessedOsdEvent& event, bool updates_suppressed);
TelemetryDispatchDecision buildTelemetryDispatchDecision(const ProcessedTelemetryEvent& event);
OsdDispatchDecision buildOsdDispatchDecision(const ProcessedOsdEvent& event, bool updates_suppressed);

ProcessedTelemetryEvent processTelemetrySessionEvent(const gs::core::SessionEvent& event);
ProcessedOsdEvent processOsdSessionEvent(const gs::core::SessionEvent& event);
