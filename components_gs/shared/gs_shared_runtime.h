#pragma once

#include <cstdint>

#include "gs_runtime_config.h"
#include "gs_runtime_event_flow.h"
#include "gs_runtime_event_pipeline.h"
#include "gs_runtime_protocol.h"
#include "gs_runtime_video_flow.h"
#include "gs_shared_state.h"
#include "settings_storage.h"

constexpr uint64_t kGSMinFreeSpaceBytes = 20ull * 1024 * 1024;

extern SettingsStorage s_settingsStorage;
extern Ground2Air_Config_Packet s_ground2air_config_packet;
