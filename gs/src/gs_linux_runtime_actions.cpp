#include "gs_shared_runtime.h"

#include <cstdlib>

#include "Comms.h"
#include "gs_linux_recording.h"
#include "gs_linux_runtime.h"
#include "gs_runtime_core.h"

void exitApp()
{
    if (s_groundstation_config.record)
    {
        toggleGSRecording(0, 0, "exit_app");
    }
    abort();
}

void applyWifiChannel()
{
    Ground2Air_Config_Packet config = s_runtimeCore.session.copyConfigPacket();
    applyWifiChannelToSession(config);
    s_change_channel = Clock::now() + std::chrono::milliseconds(3000);
}

void applyWifiChannel(Ground2Air_Config_Packet& config)
{
    applyWifiChannelToSession(config);
    s_change_channel = Clock::now() + std::chrono::milliseconds(3000);
}

void applyWifiChannelInstant()
{
    Ground2Air_Config_Packet config = s_runtimeCore.session.copyConfigPacket();
    applyWifiChannelInstantToSession(config, s_transport);
}

void applyWifiChannelInstant(Ground2Air_Config_Packet& config)
{
    applyWifiChannelInstantToSession(config, s_transport);
}

void applyGSTxPower()
{
    applyGSTxPowerToTransport(s_transport);
}

void applyGSTxPower(Ground2Air_Config_Packet&)
{
    applyGSTxPower();
}

void airUnpair()
{
    s_last_packet_tp = Clock::now();
    s_last_stats_packet_tp = Clock::now();
    resetAirPairing(s_groundstation_config.deviceId, s_transport);
}
