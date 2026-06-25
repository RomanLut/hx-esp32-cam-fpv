#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "../../components/common/Clock.h"
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

bool isAirStatsFresh(Clock::time_point now);

//===================================================================================
//===================================================================================
// Describes the current high-level link progress shown in the shared top overlay.
enum class LinkState : uint8_t
{
    None = 0,
    LookingForWifiNetwork = 1,
    ConnectingToWifiNetwork = 2,
    ConnectingToStream = 3
};

extern std::atomic<LinkState> s_link_state;

//===================================================================================
//===================================================================================
// Describes one APFPV camera discovered via Wi-Fi scanning.
struct ApfpvCameraDescriptor
{
    uint16_t device_id = 0;
    std::string ssid;
};

//===================================================================================
//===================================================================================
// Captures the shared APFPV camera selection state used by both menu and transports.
struct ApfpvCameraStateSnapshot
{
    uint16_t preferred_camera_id = 0;
    uint16_t active_camera_id = 0;
    std::vector<ApfpvCameraDescriptor> discovered_cameras;
    bool wifi_scan_permission_error = false;
};

void syncAirStatusGlobals();
bool isHQDVRMode();
void setLinkState(LinkState state);
LinkState getLinkState();
void setLinkStateDetailText(const std::string& text);
std::string getLinkStateDetailText();
uint16_t parseApfpvCameraIdFromSsid(const std::string& ssid);
std::string formatApfpvCameraId(uint16_t device_id);
void updateApfpvDiscoveredCameras(const std::vector<ApfpvCameraDescriptor>& cameras);
void setApfpvPreferredCameraId(uint16_t device_id);
uint16_t getApfpvPreferredCameraId();
void setApfpvActiveCamera(const std::string& ssid);
void clearApfpvActiveCamera();
void clearApfpvCameraRuntimeState();
void setApfpvWifiScanPermissionError(bool enabled);
bool getApfpvWifiScanPermissionError();
void requestApfpvWifiScanPermissionPrompt();
bool consumeApfpvWifiScanPermissionPromptRequest();
ApfpvCameraStateSnapshot copyApfpvCameraState();
