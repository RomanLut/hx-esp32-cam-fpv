#pragma once

#include <cstddef>
#include <cstdint>

#include "crc.h"
#include "packets.h"

namespace gs::protocol
{

struct AirPacketInfo
{
    const Air2Ground_Header* header = nullptr;
    uint32_t packetSize = 0;
};

void prepareConfigPacket(Ground2Air_Config_Packet& packet,
                         uint8_t ping,
                         uint16_t airDeviceId,
                         uint16_t gsDeviceId);

Ground2Air_Connect_Packet makeConnectPacket(uint16_t gsDeviceId);

void prepareTelemetryPacket(Ground2Air_Data_Packet& packet,
                            size_t payloadSize,
                            uint16_t airDeviceId,
                            uint16_t gsDeviceId);

bool tryParseAirPacket(const void* data, size_t size, AirPacketInfo& outInfo);

bool isConnectConfigPacket(const AirPacketInfo& packetInfo,
                           Air2Ground_Header::Type expectedType,
                           uint16_t gsDeviceId);

bool isPacketForSession(const AirPacketInfo& packetInfo,
                        uint16_t gsDeviceId,
                        uint16_t connectedAirDeviceId);

template <typename T>
bool validateFixedHeaderCrc(const T& packet)
{
    T copy = packet;
    uint8_t crc = copy.crc;
    copy.crc = 0;
    return crc == crc8(0, &copy, sizeof(T));
}

}
