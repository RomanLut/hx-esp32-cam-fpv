#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "main.h"
#include "core/transport.h"

namespace gs::menu
{

constexpr int kMinTxPower = 5;
constexpr int kDefaultTxPower = 45;
constexpr int kMaxTxPower = 63;

struct AirStorageStatus
{
    bool detected = false;
    bool error = false;
    bool slow = false;
    uint16_t free_space_gb16 = 0;
    uint16_t total_space_gb16 = 0;
};

struct GroundStorageStatus
{
    uint64_t free_space_bytes = 0;
    uint64_t total_space_bytes = 0;
};

class IOSDMenuPlatform
{
public:
    virtual ~IOSDMenuPlatform() = default;

    virtual TGroundstationConfig& groundstationConfig() = 0;
    virtual const TGroundstationConfig& groundstationConfig() const = 0;

    virtual gs::core::ITransport& transport() = 0;
    virtual const gs::core::ITransport& transport() const = 0;

    virtual bool isOV5640() const = 0;
    virtual bool isDualCamera() const = 0;

    virtual AirStorageStatus airStorageStatus() const = 0;
    virtual GroundStorageStatus groundStorageStatus() const = 0;

    virtual const char* currentOSDFontName() const = 0;
    virtual const std::vector<std::string>& osdFontsList() const = 0;
    virtual void selectOSDFont(Ground2Air_Config_Packet& config, const std::string& font_name) = 0;

    virtual void saveGroundStationConfig() = 0;
    virtual void saveGround2AirConfig(const Ground2Air_Config_Packet& config) = 0;

    virtual void applyWifiChannel(Ground2Air_Config_Packet& config) = 0;
    virtual void applyWifiChannelInstant(Ground2Air_Config_Packet& config) = 0;
    virtual void applyGSTxPower(Ground2Air_Config_Packet& config) = 0;

    virtual void airUnpair() = 0;
    virtual void exitApp() = 0;
    virtual void restartGPIOButtons() = 0;
    virtual void setVsync(bool enabled) = 0;
    virtual bool supportsCustomScreenAspectModes() const { return true; }

    virtual std::string systemIPv4() const = 0;
    virtual Clock::time_point lastPacketTime() const = 0;

    virtual void captureFrameDebug(bool until_loss) = 0;
    virtual void disableFrameDebug() = 0;
};

} // namespace gs::menu
