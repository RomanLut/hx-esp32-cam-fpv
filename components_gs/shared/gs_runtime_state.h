#pragma once

#include <cstdint>
#include <mutex>

#include "Clock.h"
#include "core/gs_session_core.h"
#include "gs_stats.h"
#include "stats.h"

extern bool s_isOV5640;
extern bool s_isDual;
extern uint16_t s_SDTotalSpaceGB16;
extern uint16_t s_SDFreeSpaceGB16;
extern bool s_air_record;
extern bool s_SDDetected;
extern bool s_SDSlow;
extern bool s_SDError;
extern std::mutex s_gs_stats_mutex;
extern Clock::time_point s_last_stats_packet_tp;
extern Clock::time_point s_incompatibleFirmwareTime;
extern GSStats& s_last_gs_stats;
extern AirStats& s_last_airStats;
extern Stats& s_dataSize_stats;
extern float video_fps;
extern bool had_loss;
extern int s_total_data;
extern int s_lost_frame_count;
extern WIFI_Rate s_curr_wifi_rate;
extern int s_wifi_queue_min;
extern int s_wifi_queue_max;
extern uint8_t s_curr_quality;
extern bool s_wifi_ovf;
extern bool s_noPing;

void syncAirStatusGlobals();
bool isHQDVRMode();
