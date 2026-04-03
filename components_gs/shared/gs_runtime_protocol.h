#pragma once

#include <cstdint>
#include <vector>

bool tryBuildControlPacketPayload(uint16_t gs_device_id, std::vector<uint8_t>& payload);
