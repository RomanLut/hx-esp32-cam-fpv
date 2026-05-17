#pragma once

#include "gs_shared_runtime.h"

void handleRenderHotkeys(Ground2Air_Config_Packet& config, bool ignore_keys);
void processPendingRestart(char* argv[]);
void processPendingWifiChannelChange();
