#pragma once

#include "Clock.h"
#include "gs_runtime_platform_services.h"

#include <string>

//===================================================================================
//===================================================================================
// Implements Android-specific runtime platform services used by shared code.
class AndroidRuntimePlatformServices final : public IRuntimePlatformServices
{
public:
    float getCpuTemperatureCelsius() const override;
    std::string getSystemIPv4() const override;
    bool supportsCustomScreenAspectModes() const override;
    void setVsync(bool enabled) override;
    void exitApp() override;
    void restartGPIOButtons() override;
    void invalidateDisplayedVideoFrame() override;
    void applyGroundStationWifiChannel(Ground2Air_Config_Packet& config) override;
    void applyGroundStationTxPower(Ground2Air_Config_Packet& config) override;

private:
    mutable std::string m_cached_ipv4 = "0.0.0.0";
    mutable Clock::time_point m_next_ipv4_refresh_tp = Clock::time_point::min();
};

//===================================================================================
//===================================================================================
// Returns the shared Android runtime platform services instance for explicit binding.
IRuntimePlatformServices& getAndroidRuntimePlatformServices();

class GsVideoRenderer;

//===================================================================================
//===================================================================================
// Binds the Android renderer used by shared runtime platform services.
void bindAndroidRuntimeRenderer(GsVideoRenderer* renderer);
