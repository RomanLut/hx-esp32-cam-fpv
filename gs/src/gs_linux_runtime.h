#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "../../components/common/Clock.h"
#include "IHAL.h"
#include "gs_linux_video_decoder.h"
#include "gs_video_layout_shared.h"
#include "gs_runtime_state.h"
#include "ISerialTelemetry.h"

extern bool bRestartRequired;
extern bool bRestart;
extern Clock::time_point s_change_channel;
extern Clock::time_point restart_tp;

extern std::unique_ptr<IHAL> s_hal;
extern gs_linux_video_decoder s_decoder;
extern std::string serialPortName;
