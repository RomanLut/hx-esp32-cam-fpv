#include "gs_runtime_config.h"

#include "Clock.h"
#include "core/osd_menu_platform.h"
#include "gs_runtime_core.h"
#include "gs_runtime_state.h"
#include "gs_shared_runtime.h"
#include "settings_storage.h"
#include "wifi_channels.h"

void initializeGroundStationConfigDefaults(uint16_t gs_device_id)
{
    s_groundstation_config.socket_fd = 0;
    s_groundstation_config.wifi_channel = DEFAULT_WIFI_CHANNEL_2_4GHZ;
    s_groundstation_config.wifiBand = DEFAULT_GS_WIFI_BAND;
    s_groundstation_config.screenAspectRatio = ScreenAspectRatio::LETTERBOX;
    s_groundstation_config.txPower = gs::menu::kDefaultTxPower;
    s_groundstation_config.vsync = true;
    s_groundstation_config.txInterface = "auto";
    s_groundstation_config.GPIOKeysLayout = 0;
    s_groundstation_config.stats = false;
    s_groundstation_config.vrMode = false;
    s_groundstation_config.deviceId = gs_device_id;
}

void loadSharedSettings(uint16_t gs_device_id)
{
    initializeGroundStationConfigDefaults(gs_device_id);
    s_settingsStorage.read();
    s_settingsStorage.loadGroundStationConfig();
    s_settingsStorage.loadGround2AirConfig();
}

void commitGround2AirConfig(const Ground2Air_Config_Packet& config)
{
    s_runtimeCore.session.setConfigPacket(config);
    s_settingsStorage.saveGround2AirConfig();
}

void applyWifiChannelToSession(Ground2Air_Config_Packet& config)
{
    config.dataChannel.wifi_channel = s_groundstation_config.wifi_channel;
    s_runtimeCore.session.setConfigPacket(config);
}

void applyWifiChannelInstantToSession(Ground2Air_Config_Packet& config, gs::core::ITransport& transport)
{
    applyWifiChannelToSession(config);
    transport.setChannel(s_groundstation_config.wifi_channel);
}

void applyGSTxPowerToTransport(gs::core::ITransport& transport)
{
    transport.setTxPower(s_groundstation_config.txPower);
}

void performAirUnpair(uint16_t gs_device_id, gs::core::ITransport& transport)
{
    s_last_packet_tp = Clock::now();
    s_last_stats_packet_tp = Clock::now();
    resetAirPairing(gs_device_id, transport);
}

void resetAirPairing(uint16_t gs_device_id, gs::core::ITransport& transport)
{
    s_runtimeCore.gs_device_id = gs_device_id;
    s_runtimeCore.resetPairing(transport, Clock::now());
}
