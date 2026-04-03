#pragma once

#include <cstddef>
#include <cstdint>

#include "Clock.h"
#include "core/gs_session_core.h"
#include "core/transport.h"

struct ProcessedSessionPacket
{
    gs::core::SessionEvent event = {};
    Clock::time_point processed_tp = Clock::now();
};

ProcessedSessionPacket processIncomingSessionPacket(gs::core::GsSessionCore& session,
                                                    const uint8_t* packet_data,
                                                    size_t packet_size,
                                                    uint16_t gs_device_id,
                                                    gs::core::ITransport& transport,
                                                    Clock::time_point now = Clock::now());
