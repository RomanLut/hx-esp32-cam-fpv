#include "gs_runtime_aux_flow.h"

#include "packets.h"

bool shouldApplyOsdUpdate(const ProcessedOsdEvent& event, bool updates_suppressed)
{
    return !updates_suppressed && event.has_update && event.payload != nullptr && event.payload_size > 0;
}

OsdDispatchDecision buildOsdDispatchDecision(const ProcessedOsdEvent& event, bool updates_suppressed)
{
    OsdDispatchDecision decision;
    decision.should_apply = shouldApplyOsdUpdate(event, updates_suppressed);
    decision.payload = event.payload;
    decision.payload_size = event.payload_size;
    return decision;
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
