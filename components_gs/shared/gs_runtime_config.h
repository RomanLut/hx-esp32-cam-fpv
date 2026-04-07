#pragma once

#include <cstdint>
#include <vector>

#include "core/transport.h"
#include "core/transport_kind.h"
#include "gs_shared_state.h"

void initializeGroundStationConfigDefaults(uint16_t gs_device_id);
void loadSharedSettings(uint16_t gs_device_id);
void commitGround2AirConfig(const Ground2Air_Config_Packet& config);
void applyWifiChannelToSession(Ground2Air_Config_Packet& config);
void applyWifiChannelInstantToSession(Ground2Air_Config_Packet& config, gs::core::ITransport& transport);
void applyGSTxPowerToTransport(gs::core::ITransport& transport);
void performAirUnpair(uint16_t gs_device_id, gs::core::ITransport& transport);
void resetAirPairing(uint16_t gs_device_id, gs::core::ITransport& transport);
gs::core::TransportKind currentTransportKind();
bool switchActiveTransport(gs::core::TransportKind kind);
void beginSelectedTransportSearchOrConnect(Ground2Air_Config_Packet& config, Clock::time_point& search_tp);
void advanceSelectedTransportSearchOrConnect(Ground2Air_Config_Packet& config,
                                             Clock::time_point& search_tp,
                                             bool& search_done);
void cancelSelectedTransportSearchOrConnect();
bool isSelectedTransportConnected();
std::vector<std::string> copyCurrentTransportInterfaces();
void applySelectedTxInterfaceToTransport();
