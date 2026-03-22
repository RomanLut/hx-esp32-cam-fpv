#include "core/gs_protocol.h"

namespace gs::protocol
{

void prepareConfigPacket(Ground2Air_Config_Packet& packet,
                         uint8_t ping,
                         uint16_t airDeviceId,
                         uint16_t gsDeviceId)
{
    packet.ping = ping;
    packet.type = Ground2Air_Header::Type::Config;
    packet.size = sizeof(packet);
    packet.airDeviceId = airDeviceId;
    packet.gsDeviceId = gsDeviceId;
    packet.crc = 0;
    packet.crc = crc8(0, &packet, sizeof(packet));
}

Ground2Air_Connect_Packet makeConnectPacket(uint16_t gsDeviceId)
{
    Ground2Air_Connect_Packet packet;
    packet.type = Ground2Air_Header::Type::Connect;
    packet.size = sizeof(packet);
    packet.airDeviceId = 0;
    packet.gsDeviceId = gsDeviceId;
    packet.crc = 0;
    packet.crc = crc8(0, &packet, sizeof(packet));
    return packet;
}

void prepareTelemetryPacket(Ground2Air_Data_Packet& packet,
                            size_t payloadSize,
                            uint16_t airDeviceId,
                            uint16_t gsDeviceId)
{
    packet.type = Ground2Air_Header::Type::Telemetry;
    packet.size = sizeof(Ground2Air_Header) + payloadSize;
    packet.airDeviceId = airDeviceId;
    packet.gsDeviceId = gsDeviceId;
    packet.crc = 0;
    packet.crc = crc8(0, &packet, packet.size);
}

bool tryParseAirPacket(const void* data, size_t size, AirPacketInfo& outInfo)
{
    if (!data || size < sizeof(Air2Ground_Header))
    {
        return false;
    }

    const auto* header = static_cast<const Air2Ground_Header*>(data);
    if (header->size > size)
    {
        return false;
    }

    outInfo.header = header;
    outInfo.packetSize = header->size;
    return true;
}

bool isConnectConfigPacket(const AirPacketInfo& packetInfo,
                           Air2Ground_Header::Type expectedType,
                           uint16_t gsDeviceId)
{
    return packetInfo.header &&
           packetInfo.header->type == expectedType &&
           packetInfo.header->gsDeviceId == gsDeviceId;
}

bool isPacketForSession(const AirPacketInfo& packetInfo,
                        uint16_t gsDeviceId,
                        uint16_t connectedAirDeviceId)
{
    if (!packetInfo.header || packetInfo.header->gsDeviceId != gsDeviceId)
    {
        return false;
    }

    return connectedAirDeviceId == 0 || packetInfo.header->airDeviceId == connectedAirDeviceId;
}

}
