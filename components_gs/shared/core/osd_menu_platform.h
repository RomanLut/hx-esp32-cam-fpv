#pragma once

#include <string>

#include "gs_shared_state.h"
#include "core/transport.h"

namespace gs::menu
{

constexpr int kMinTxPower = 5;
constexpr int kDefaultTxPower = 45;
constexpr int kMaxTxPower = 63;

class IOSDMenuPlatform
{
public:
    virtual ~IOSDMenuPlatform() = default;

    virtual gs::core::ITransport& transport() = 0;
    virtual const gs::core::ITransport& transport() const = 0;

    virtual const char* currentOSDFontName() const = 0;
    virtual void selectOSDFont(Ground2Air_Config_Packet& config, const std::string& font_name) = 0;

    virtual void applyWifiChannel(Ground2Air_Config_Packet& config) = 0;
    virtual void applyWifiChannelInstant(Ground2Air_Config_Packet& config) = 0;
    virtual void applyGSTxPower(Ground2Air_Config_Packet& config) = 0;

    virtual void airUnpair() = 0;
    virtual bool supportsCustomScreenAspectModes() const { return true; }

    virtual void captureFrameDebug(bool until_loss) = 0;
    virtual void disableFrameDebug() = 0;
};

} // namespace gs::menu
