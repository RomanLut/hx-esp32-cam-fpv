#include "gs_runtime_state.h"

#include "gs_runtime_core.h"
#include "gs_shared_state.h"

bool s_isOV5640 = false;
bool s_isDual = false;
uint16_t s_SDTotalSpaceGB16 = 0;
uint16_t s_SDFreeSpaceGB16 = 0;
bool s_air_record = false;
bool s_SDDetected = false;
bool s_SDSlow = false;
bool s_SDError = false;
std::mutex s_gs_stats_mutex;
Clock::time_point s_last_stats_packet_tp = Clock::now();
Clock::time_point s_incompatibleFirmwareTime = Clock::now() - std::chrono::milliseconds(10000);
GSStats& s_gs_stats = s_runtimeCore.gs_stats;
GSStats& s_last_gs_stats = s_runtimeCore.last_gs_stats;
AirStats& s_last_airStats = s_runtimeCore.session.lastAirStats();
Stats& s_dataSize_stats = s_runtimeCore.data_size_stats;
float video_fps = 0.0f;
bool had_loss = false;
int s_total_data = 0;
int s_lost_frame_count = 0;
WIFI_Rate s_curr_wifi_rate = WIFI_Rate::RATE_B_2M_CCK;
int s_wifi_queue_min = 0;
int s_wifi_queue_max = 0;
uint8_t s_curr_quality = 0;
bool s_wifi_ovf = false;
bool s_noPing = false;

void syncAirStatusGlobals()
{
    const gs::core::AirStatusState& air_status = s_runtimeCore.session.airStatus();
    s_curr_wifi_rate = air_status.curr_wifi_rate;
    s_wifi_queue_min = air_status.wifi_queue_min;
    s_wifi_queue_max = air_status.wifi_queue_max;
    s_curr_quality = air_status.curr_quality;
    s_SDTotalSpaceGB16 = air_status.sd_total_space_gb16;
    s_SDFreeSpaceGB16 = air_status.sd_free_space_gb16;
    s_air_record = air_status.air_record;
    s_wifi_ovf = air_status.wifi_ovf;
    s_SDDetected = air_status.sd_detected;
    s_SDSlow = air_status.sd_slow;
    s_SDError = air_status.sd_error;
    s_isOV5640 = air_status.is_ov5640;
}

bool isHQDVRMode()
{
    return s_last_airStats.hq_dvr_mode != 0;
}
