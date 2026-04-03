#pragma once

#include "Clock.h"
#include "gs_shared_runtime.h"

struct ImGuiIO;

void initializeDesktopOsd();
void registerDesktopRenderCallback(Ground2Air_Config_Packet& config, char* argv[]);
void processDesktopFrameTick(size_t& video_frame_count,
                             Clock::time_point& last_stats_tp,
                             Clock::time_point& last_tp,
                             ImGuiIO& io);
