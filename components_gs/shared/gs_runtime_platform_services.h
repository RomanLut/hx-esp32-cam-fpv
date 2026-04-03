#pragma once

#include <cstdint>
#include <string>

//===================================================================================
//===================================================================================
// Describes platform-provided ground storage capacity and free space.
struct GroundStorageStatus
{
    uint64_t free_space_bytes = 0;
    uint64_t total_space_bytes = 0;
};

//===================================================================================
//===================================================================================
// Exposes platform-specific runtime services needed by shared UI and menu code.
class IRuntimePlatformServices
{
public:
    virtual ~IRuntimePlatformServices() = default;

    virtual float getCpuTemperatureCelsius() const = 0;
    virtual GroundStorageStatus getGroundStorageStatus() const = 0;
    virtual std::string getSystemIPv4() const = 0;
    virtual void setVsync(bool enabled) = 0;
    virtual void exitApp() = 0;
    virtual void restartGPIOButtons() = 0;
};

extern IRuntimePlatformServices* s_RuntimePlatformServices;
