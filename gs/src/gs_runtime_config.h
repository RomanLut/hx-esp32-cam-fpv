#pragma once

#include <cstdint>

#include "core/transport.h"
#include "gs_shared_state.h"

void initializeGroundStationConfigDefaults(uint16_t gs_device_id);
void loadSharedSettings(uint16_t gs_device_id);
void commitGround2AirConfig(const Ground2Air_Config_Packet& config);
void applyWifiChannelToSession(Ground2Air_Config_Packet& config);
void applyWifiChannelInstantToSession(Ground2Air_Config_Packet& config, gs::core::ITransport& transport);
void applyGSTxPowerToTransport(gs::core::ITransport& transport);
void resetAirPairing(uint16_t gs_device_id, gs::core::ITransport& transport);
