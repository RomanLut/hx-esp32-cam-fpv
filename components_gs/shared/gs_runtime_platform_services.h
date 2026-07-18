#pragma once

#include <string>

#include "packets.h"

//===================================================================================
//===================================================================================
// Exposes platform-specific runtime services needed by shared UI and menu code.
class IRuntimePlatformServices
{
public:
    virtual ~IRuntimePlatformServices() = default;

    virtual float getCpuTemperatureCelsius() const = 0;
    virtual std::string getSystemIPv4() const = 0;
    // Human-readable current output mode, e.g. "1920x1080 60Hz". Empty when the
    // platform has no meaningful fixed display mode to report.
    virtual std::string getDisplayModeSummary() const { return {}; }
    virtual bool supportsCustomScreenAspectModes() const = 0;
    virtual bool supportsPipelineModeSelection() const { return false; }
    virtual void setVsync(bool enabled) = 0;
    virtual void exitApp() = 0;
    virtual bool supportsGPIOKeys() const { return true; }
    virtual void restartGPIOButtons() = 0;
    virtual void invalidateDisplayedVideoFrame() = 0;
    virtual void applyGroundStationWifiChannel(Ground2Air_Config_Packet& config) = 0;
    virtual void applyGroundStationTxPower(Ground2Air_Config_Packet& config) = 0;
};

extern IRuntimePlatformServices* s_RuntimePlatformServices;
