#pragma once

#include "gs_runtime_platform_services.h"

//===================================================================================
//===================================================================================
// Implements Linux-specific runtime platform services used by shared code.
class LinuxRuntimePlatformServices final : public IRuntimePlatformServices
{
public:
    float getCpuTemperatureCelsius() const override;
    std::string getSystemIPv4() const override;
    std::string getDisplayModeSummary() const override;
    bool supportsCustomScreenAspectModes() const override;
    void setVsync(bool enabled) override;
    void exitApp() override;
    void restartGPIOButtons() override;
    void invalidateDisplayedVideoFrame() override;
    void applyGroundStationWifiChannel(Ground2Air_Config_Packet& config) override;
    void applyGroundStationTxPower(Ground2Air_Config_Packet& config) override;
};

//===================================================================================
//===================================================================================
// Returns the shared Linux runtime platform services instance for explicit binding.
IRuntimePlatformServices& getLinuxRuntimePlatformServices();
