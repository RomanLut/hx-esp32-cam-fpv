#pragma once

#include "gs_shared_runtime.h"

Ground2Air_Config_Packet beginRenderConfigFrame();
bool finishRenderConfigFrame(Ground2Air_Config_Packet& config);
void handleRenderHotkeys(Ground2Air_Config_Packet& config, bool ignore_keys);
void processPendingRestart(char* argv[]);
void processPendingWifiChannelChange();
void processPendingOsdFontReload(const Ground2Air_Config_Packet& config);
