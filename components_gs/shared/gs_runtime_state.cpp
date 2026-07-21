#include "gs_runtime_state.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

#include "Log.h"
#include "gs_runtime_core.h"
#include "gs_shared_state.h"

bool s_isOV5640 = false;
bool s_isOV3660 = false;
bool s_isEsp32 = false;
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
std::atomic<LinkState> s_link_state = LinkState::None;

namespace
{

constexpr Clock::duration kAirStatsStaleDuration = std::chrono::seconds(2);

} // namespace

//===================================================================================
//===================================================================================
// Returns whether air-derived status values are recent enough to display as valid.
bool isAirStatsFresh(Clock::time_point now)
{
    return now - s_runtimeCore.last_packet_tp < kAirStatsStaleDuration;
}

//===================================================================================
//===================================================================================
// Returns the lazily initialized mutex guarding the shared link-state detail text.
static std::mutex& linkStateDetailMutex()
{
    static std::mutex mutex;
    return mutex;
}

//===================================================================================
//===================================================================================
// Returns the lazily initialized shared link-state detail text storage.
static std::string& linkStateDetailTextStorage()
{
    static std::string text;
    return text;
}

//===================================================================================
//===================================================================================
// Returns the lazily initialized mutex guarding the shared APFPV camera state cache.
static std::mutex& apfpvCameraStateMutex()
{
    static std::mutex mutex;
    return mutex;
}

//===================================================================================
//===================================================================================
// Returns the lazily initialized preferred APFPV camera identifier storage.
static uint16_t& apfpvPreferredCameraIdStorage()
{
    static uint16_t device_id = 0;
    return device_id;
}

//===================================================================================
//===================================================================================
// Returns the lazily initialized discovered APFPV camera cache keyed by device id.
static std::map<uint16_t, std::string>& apfpvDiscoveredCamerasStorage()
{
    static std::map<uint16_t, std::string> cameras;
    return cameras;
}

//===================================================================================
//===================================================================================
// Returns the lazily initialized active APFPV camera identifier storage.
static uint16_t& apfpvActiveCameraIdStorage()
{
    static uint16_t device_id = 0;
    return device_id;
}

//===================================================================================
//===================================================================================
// Returns the lazily initialized APFPV Wi-Fi scan permission error flag.
static bool& apfpvWifiScanPermissionErrorStorage()
{
    static bool enabled = false;
    return enabled;
}

//===================================================================================
//===================================================================================
// Returns the lazily initialized APFPV permission-prompt request flag.
static std::atomic<bool>& apfpvWifiScanPermissionPromptRequestStorage()
{
    static std::atomic<bool> requested = false;
    return requested;
}

//===================================================================================
//===================================================================================
// Stores the latest shared link-progress state used by the top overlay.
void setLinkState(LinkState state)
{
    s_link_state.store(state, std::memory_order_release);
    if (state == LinkState::None)
    {
        std::lock_guard<std::mutex> lock(linkStateDetailMutex());
        linkStateDetailTextStorage().clear();
    }
}

//===================================================================================
//===================================================================================
// Returns the latest shared link-progress state used by the top overlay.
LinkState getLinkState()
{
    return s_link_state.load(std::memory_order_acquire);
}

//===================================================================================
//===================================================================================
// Stores optional detailed link-progress text for the top overlay during connect transitions.
void setLinkStateDetailText(const std::string& text)
{
    std::lock_guard<std::mutex> lock(linkStateDetailMutex());
    linkStateDetailTextStorage() = text;
}

//===================================================================================
//===================================================================================
// Returns the optional detailed link-progress text for the top overlay.
std::string getLinkStateDetailText()
{
    std::lock_guard<std::mutex> lock(linkStateDetailMutex());
    return linkStateDetailTextStorage();
}

//===================================================================================
//===================================================================================
// Parses the APFPV camera device id suffix from an SSID such as esp32cam-fpv-252a.
uint16_t parseApfpvCameraIdFromSsid(const std::string& ssid)
{
    constexpr const char* prefix = "esp32cam-fpv-";
    constexpr size_t prefix_length = 13;
    if (ssid.size() <= prefix_length || ssid.rfind(prefix, 0) != 0)
    {
        return 0;
    }

    const std::string suffix = ssid.substr(prefix_length);
    if (suffix.empty())
    {
        return 0;
    }

    if (!std::all_of(suffix.begin(), suffix.end(), [](unsigned char ch)
        {
            return std::isxdigit(ch) != 0;
        }))
    {
        return 0;
    }

    try
    {
        return static_cast<uint16_t>(std::stoul(suffix, nullptr, 16));
    }
    catch (...)
    {
        return 0;
    }
}

//===================================================================================
//===================================================================================
// Formats one APFPV camera device id as the uppercase hex label shown in the menu.
std::string formatApfpvCameraId(uint16_t device_id)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%04X", device_id);
    return buffer;
}

//===================================================================================
//===================================================================================
// Replaces the shared list of APFPV cameras discovered by the current platform backend.
void updateApfpvDiscoveredCameras(const std::vector<ApfpvCameraDescriptor>& cameras)
{
    std::lock_guard<std::mutex> lock(apfpvCameraStateMutex());
    std::map<uint16_t, std::string>& discovered_cameras = apfpvDiscoveredCamerasStorage();
    discovered_cameras.clear();

    for (const ApfpvCameraDescriptor& camera : cameras)
    {
        if (camera.device_id == 0 || camera.ssid.empty())
        {
            continue;
        }
        discovered_cameras[camera.device_id] = camera.ssid;
    }
    LOGI("updateApfpvDiscoveredCameras count={}", discovered_cameras.size());
}

//===================================================================================
//===================================================================================
// Stores the preferred APFPV camera id in the shared camera state used across threads.
void setApfpvPreferredCameraId(uint16_t device_id)
{
    std::lock_guard<std::mutex> lock(apfpvCameraStateMutex());
    apfpvPreferredCameraIdStorage() = device_id;
    LOGI("setApfpvPreferredCameraId {}", formatApfpvCameraId(device_id));
}

//===================================================================================
//===================================================================================
// Returns the preferred APFPV camera id from the shared camera state.
uint16_t getApfpvPreferredCameraId()
{
    std::lock_guard<std::mutex> lock(apfpvCameraStateMutex());
    return apfpvPreferredCameraIdStorage();
}

//===================================================================================
//===================================================================================
// Marks the currently active APFPV camera based on the connected Wi-Fi SSID.
void setApfpvActiveCamera(const std::string& ssid)
{
    std::lock_guard<std::mutex> lock(apfpvCameraStateMutex());
    uint16_t& active_camera_id = apfpvActiveCameraIdStorage();
    active_camera_id = parseApfpvCameraIdFromSsid(ssid);
    LOGI("setApfpvActiveCamera ssid={} id={}", ssid, formatApfpvCameraId(active_camera_id));
}

//===================================================================================
//===================================================================================
// Clears the currently active APFPV camera after Wi-Fi disconnect or transport reset.
void clearApfpvActiveCamera()
{
    std::lock_guard<std::mutex> lock(apfpvCameraStateMutex());
    apfpvActiveCameraIdStorage() = 0;
    LOGI("clearApfpvActiveCamera");
}

//===================================================================================
//===================================================================================
// Clears all non-persisted APFPV camera runtime state while preserving preferred settings.
void clearApfpvCameraRuntimeState()
{
    std::lock_guard<std::mutex> lock(apfpvCameraStateMutex());
    uint16_t& preferred_camera_id = apfpvPreferredCameraIdStorage();
    preferred_camera_id = s_groundstation_config.apfpvPreferredCameraId;
    apfpvActiveCameraIdStorage() = 0;
    apfpvDiscoveredCamerasStorage().clear();
    apfpvWifiScanPermissionErrorStorage() = false;
    LOGI("clearApfpvCameraRuntimeState preferred={}", formatApfpvCameraId(preferred_camera_id));
}

//===================================================================================
//===================================================================================
// Stores whether APFPV Wi-Fi scanning is currently blocked by missing Android permissions.
void setApfpvWifiScanPermissionError(bool enabled)
{
    std::lock_guard<std::mutex> lock(apfpvCameraStateMutex());
    apfpvWifiScanPermissionErrorStorage() = enabled;
}

//===================================================================================
//===================================================================================
// Returns whether APFPV Wi-Fi scanning is currently blocked by missing Android permissions.
bool getApfpvWifiScanPermissionError()
{
    std::lock_guard<std::mutex> lock(apfpvCameraStateMutex());
    return apfpvWifiScanPermissionErrorStorage();
}

//===================================================================================
//===================================================================================
// Queues an explicit APFPV Wi-Fi scan permission prompt request from menu action.
void requestApfpvWifiScanPermissionPrompt()
{
    apfpvWifiScanPermissionPromptRequestStorage().store(true, std::memory_order_release);
}

//===================================================================================
//===================================================================================
// Returns and clears one queued APFPV Wi-Fi scan permission prompt request.
bool consumeApfpvWifiScanPermissionPromptRequest()
{
    return apfpvWifiScanPermissionPromptRequestStorage().exchange(false, std::memory_order_acq_rel);
}

//===================================================================================
//===================================================================================
// Returns a thread-safe snapshot of preferred, active, and discovered APFPV cameras.
ApfpvCameraStateSnapshot copyApfpvCameraState()
{
    std::lock_guard<std::mutex> lock(apfpvCameraStateMutex());
    const uint16_t preferred_camera_id = apfpvPreferredCameraIdStorage();
    const uint16_t active_camera_id = apfpvActiveCameraIdStorage();
    const std::map<uint16_t, std::string>& discovered_cameras = apfpvDiscoveredCamerasStorage();

    ApfpvCameraStateSnapshot snapshot = {};
    snapshot.preferred_camera_id = preferred_camera_id;
    snapshot.active_camera_id = active_camera_id;
    snapshot.wifi_scan_permission_error = apfpvWifiScanPermissionErrorStorage();

    for (const auto& [device_id, ssid] : discovered_cameras)
    {
        snapshot.discovered_cameras.push_back({device_id, ssid});
    }

    return snapshot;
}

//===================================================================================
//===================================================================================
// Synchronizes shared air-unit status globals from the latest session snapshot.
void syncAirStatusGlobals()
{
    const AirStats air_stats = s_runtimeCore.session.copyLastAirStats();
    s_curr_wifi_rate = static_cast<WIFI_Rate>(air_stats.curr_wifi_rate);
    s_wifi_queue_min = air_stats.wifi_queue_min;
    s_wifi_queue_max = air_stats.wifi_queue_max;
    s_curr_quality = air_stats.curr_quality;
    s_SDTotalSpaceGB16 = air_stats.SDTotalSpaceGB16;
    s_SDFreeSpaceGB16 = air_stats.SDFreeSpaceGB16;
    s_air_record = air_stats.air_record_state != 0;
    s_wifi_ovf = air_stats.wifi_ovf != 0;
    s_SDDetected = air_stats.SDDetected != 0;
    s_SDSlow = air_stats.SDSlow != 0;
    s_SDError = air_stats.SDError != 0;
    s_isOV5640 = air_stats.isOV5640 != 0;
    s_isOV3660 = air_stats.isOV3660 != 0;
    s_isEsp32 = air_stats.isEsp32 != 0;
}

//===================================================================================
//===================================================================================
// Returns whether the current air link reports HQ DVR mode enabled.
bool isHQDVRMode()
{
    return s_runtimeCore.session.copyLastAirStats().hq_dvr_mode != 0;
}
