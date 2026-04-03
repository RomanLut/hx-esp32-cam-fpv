#include "gs_runtime_packet_flow.h"

ProcessedSessionPacket processIncomingSessionPacket(gs::core::GsSessionCore& session,
                                                    const uint8_t* packet_data,
                                                    size_t packet_size,
                                                    uint16_t gs_device_id,
                                                    gs::core::ITransport& transport,
                                                    Clock::time_point now)
{
    ProcessedSessionPacket result;
    result.processed_tp = now;
    result.event = session.processReceivedPacket(packet_data, packet_size, gs_device_id, now, transport);
    return result;
}
