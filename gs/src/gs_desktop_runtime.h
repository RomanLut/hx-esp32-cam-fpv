#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "Clock.h"
#include "IHAL.h"
#include "Video_Decoder.h"
#include "gs_video_layout_shared.h"
#include "gs_runtime_state.h"

extern bool bRestartRequired;
extern bool bRestart;
extern bool s_debugWindowVisisble;
extern uint64_t s_GSSDTotalSpaceBytes;
extern uint64_t s_GSSDFreeSpaceBytes;
extern Clock::time_point s_change_channel;
extern Clock::time_point restart_tp;

extern std::unique_ptr<IHAL> s_hal;
extern Video_Decoder s_decoder;
extern int fdUART;
extern std::string serialPortName;
extern uint8_t s_avi_fps;
extern uint16_t s_avi_frameWidth;
extern uint16_t s_avi_frameHeight;
extern uint32_t s_avi_frameCnt;
extern bool s_avi_ov2640HighFPS;
extern bool s_avi_ov5640HighFPS;
