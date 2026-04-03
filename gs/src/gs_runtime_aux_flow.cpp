#include "gs_runtime_aux_flow.h"

#include "packets.h"

bool hasTelemetryPayload(const ProcessedTelemetryEvent& event)
{
    return event.has_payload && event.payload != nullptr && event.payload_size > 0;
}

bool shouldApplyOsdUpdate(const ProcessedOsdEvent& event, bool updates_suppressed)
{
    return !updates_suppressed && event.has_update && event.payload != nullptr && event.payload_size > 0;
}

TelemetryDispatchDecision buildTelemetryDispatchDecision(const ProcessedTelemetryEvent& event)
{
    TelemetryDispatchDecision decision;
    decision.has_payload = hasTelemetryPayload(event);
    decision.payload = event.payload;
    decision.payload_size = event.payload_size;
    decision.inbound_bytes = decision.has_payload ? event.payload_size : 0;
    return decision;
}

OsdDispatchDecision buildOsdDispatchDecision(const ProcessedOsdEvent& event, bool updates_suppressed)
{
    OsdDispatchDecision decision;
    decision.should_apply = shouldApplyOsdUpdate(event, updates_suppressed);
    decision.payload = event.payload;
    decision.payload_size = event.payload_size;
    return decision;
}

ProcessedTelemetryEvent processTelemetrySessionEvent(const gs::core::SessionEvent& event)
{
    ProcessedTelemetryEvent result;
    if (event.kind != gs::core::SessionEventKind::TelemetryPayload || event.telemetry.packet == nullptr)
    {
        return result;
    }

    result.payload =
        reinterpret_cast<const uint8_t*>(event.telemetry.packet) + sizeof(Air2Ground_Data_Packet);
    result.payload_size = event.telemetry.payload_size;
    result.has_payload = true;
    return result;
}

ProcessedOsdEvent processOsdSessionEvent(const gs::core::SessionEvent& event)
{
    ProcessedOsdEvent result;
    if (event.kind != gs::core::SessionEventKind::OsdUpdate || event.osd.packet == nullptr)
    {
        return result;
    }

    result.payload = &event.osd.packet->osd_enc_start;
    result.payload_size = event.osd.osd_data_size;
    result.has_update = true;
    return result;
}
