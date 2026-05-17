#pragma once

#include "gs_runtime_osd_font_storage.h"

//===================================================================================
//===================================================================================
// Implements Android-side OSD font discovery and PNG loading from APK assets.
class AndroidOSDFontStorage final : public IOSDFontStorage
{
public:
    const std::vector<std::string>& osdFontsList() const override;
    bool loadOSDFontBytes(const std::string& font_name, std::vector<unsigned char>& font_png) const override;
};

//===================================================================================
//===================================================================================
// Returns the shared Android OSD font storage instance for explicit runtime binding.
IOSDFontStorage& getAndroidOsdFontStorage();
