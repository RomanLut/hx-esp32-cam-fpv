#pragma once

#include "../../../../../components/common/Clock.h"
#include "core/transport.h"
#include "gs_runtime_platform_services.h"

#include <atomic>
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
    bool supportsPipelineModeSelection() const override;
    bool supportsGPIOKeys() const override { return false; }
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

//===================================================================================
//===================================================================================
// Consumes the deferred Android renderer invalidation request flag.
bool consumeAndroidRendererInvalidateRequest();

//===================================================================================
//===================================================================================
// Stores the latest Android thermal status so shared runtime code can consume it.
void setAndroidThermalStatus(int thermal_status);

//===================================================================================
//===================================================================================
// Retunes the RTL8812AU adapter once the air unit has had time to switch channels.
// Must be called each background loop tick when raw-broadcast is active.
void processPendingRawBroadcastChannelChange(gs::core::ITransport& transport);

//===================================================================================
//===================================================================================
// Reports whether raw-broadcast should send control/config packets faster until
// the deferred silence-gated channel change is applied locally.
bool rawBroadcastControlBurstActive(Clock::time_point now);
