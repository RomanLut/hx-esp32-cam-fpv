#pragma once

#include "gs_runtime_osd_font_storage.h"

#include <vector>
#include <string>

//===================================================================================
//===================================================================================
// Implements Linux-side OSD font discovery and raw PNG loading from assets_gs.
class LinuxOSDFontStorage final : public IOSDFontStorage
{
public:
    const std::vector<std::string>& osdFontsList() const override;
    bool loadOSDFontBytes(const std::string& font_name, std::vector<unsigned char>& font_png) const override;

private:
    void refreshFontsList() const;

    mutable std::vector<std::string> m_font_names;
};

//===================================================================================
//===================================================================================
// Returns the shared Linux OSD font storage instance for explicit runtime binding.
IOSDFontStorage& getLinuxOsdFontStorage();
