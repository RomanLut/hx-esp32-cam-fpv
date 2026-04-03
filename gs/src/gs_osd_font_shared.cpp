#include "gs_osd_font_shared.h"

#include "lodepng.h"
#include "gs_shared_runtime.h"

namespace
{

const std::string kDefaultOsdFontName = "INAV_default_24.png";
const std::vector<std::string> kBuiltinOsdFontNames = {kDefaultOsdFontName};

}

const std::string& getDefaultOsdFontName()
{
    return kDefaultOsdFontName;
}

const std::vector<std::string>& getBuiltinOsdFontNames()
{
    return kBuiltinOsdFontNames;
}

std::string getSelectedOsdFontName()
{
    std::string font_name = s_settingsStorage["gs"]["osd_font"];
    if (font_name.empty())
    {
        return kDefaultOsdFontName;
    }
    return font_name;
}

void setSelectedOsdFontName(const std::string& font_name)
{
    s_settingsStorage["gs"]["osd_font"] = font_name;
}

std::optional<std::string> findOsdFontNameByCrc(const std::vector<std::string>& font_names, uint32_t font_crc32)
{
    for (const std::string& font_name : font_names)
    {
        const uint32_t crc32 =
            lodepng_crc32(reinterpret_cast<const unsigned char*>(font_name.c_str()), font_name.length());
        if (crc32 == font_crc32)
        {
            return font_name;
        }
    }

    return std::nullopt;
}
