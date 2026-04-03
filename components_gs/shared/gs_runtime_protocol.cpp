#include "gs_runtime_protocol.h"

#include <cstring>

#include "gs_runtime_core.h"
#include "gs_shared_state.h"

bool tryBuildControlPacketPayload(uint16_t gs_device_id, std::vector<uint8_t>& payload)
{
    const gs::core::ControlPacketView control_packet = s_runtimeCore.session.buildControlPacket(gs_device_id);
    switch (control_packet.type)
    {
    case gs::core::ControlPacketType::Config:
        payload.resize(sizeof(control_packet.config_packet));
        std::memcpy(payload.data(), &control_packet.config_packet, sizeof(control_packet.config_packet));
        return true;
    case gs::core::ControlPacketType::Connect:
        payload.resize(sizeof(control_packet.connect_packet));
        std::memcpy(payload.data(), &control_packet.connect_packet, sizeof(control_packet.connect_packet));
        return true;
    case gs::core::ControlPacketType::None:
    default:
        payload.clear();
        return false;
    }
}
