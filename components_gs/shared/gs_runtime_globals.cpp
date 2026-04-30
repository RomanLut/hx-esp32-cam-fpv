#include "gs_shared_runtime.h"
#include "gs_runtime_core.h"

TGroundstationConfig s_groundstation_config = {};
LensCorrectionState s_lensCorrectionState = {};
ImageStabilizationState s_imageStabilizationState = {};
SettingsStorage s_settingsStorage("gs.ini");
Ground2Air_Config_Packet s_ground2air_config_packet = {};
GsRuntimeCore s_runtimeCore(1, s_groundstation_config, s_ground2air_config_packet);
Clock::time_point& s_last_packet_tp = s_runtimeCore.last_packet_tp;
