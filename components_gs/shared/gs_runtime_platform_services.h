#pragma once

#include <string>

//===================================================================================
//===================================================================================
// Exposes platform-specific runtime services needed by shared UI and menu code.
class IRuntimePlatformServices
{
public:
    virtual ~IRuntimePlatformServices() = default;

    virtual float getCpuTemperatureCelsius() const = 0;
    virtual std::string getSystemIPv4() const = 0;
    virtual void setVsync(bool enabled) = 0;
    virtual void exitApp() = 0;
    virtual void restartGPIOButtons() = 0;
};

extern IRuntimePlatformServices* s_RuntimePlatformServices;
