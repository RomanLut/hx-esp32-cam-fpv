#pragma once

#include <string>
#include <vector>

//===================================================================================
//===================================================================================
// Provides platform-specific access to available OSD fonts and raw font bytes.
class IOSDFontStorage
{
public:
    virtual ~IOSDFontStorage() = default;

    virtual const std::vector<std::string>& osdFontsList() const = 0;
    virtual bool loadOSDFontBytes(const std::string& font_name, std::vector<unsigned char>& font_png) const = 0;
};

extern IOSDFontStorage* s_OSDFontStorage;
