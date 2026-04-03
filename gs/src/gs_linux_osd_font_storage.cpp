#include "gs_linux_osd_font_storage.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace
{

//===================================================================================
//===================================================================================
// Owns the Linux OSD font storage implementation for explicit runtime binding.
LinuxOSDFontStorage s_linux_osd_font_storage;

//===================================================================================
//===================================================================================
// Returns the Linux-side directory that stores shared OSD font PNG files.
fs::path osdFontsDirectory()
{
    return "../assets_gs/osd_fonts";
}

} // namespace

//===================================================================================
//===================================================================================
// Returns the shared Linux OSD font storage instance for explicit runtime binding.
IOSDFontStorage& getLinuxOsdFontStorage()
{
    return s_linux_osd_font_storage;
}

//===================================================================================
//===================================================================================
// Rebuilds the Linux OSD font list from the shared assets_gs directory.
void LinuxOSDFontStorage::refreshFontsList() const
{
    m_font_names.clear();

    const fs::path directory_path = osdFontsDirectory();
    if (!fs::exists(directory_path) || !fs::is_directory(directory_path))
    {
        return;
    }

    for (const auto& entry : fs::directory_iterator(directory_path))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".png")
        {
            m_font_names.push_back(entry.path().filename().string());
        }
    }

    std::sort(m_font_names.begin(), m_font_names.end());
}

//===================================================================================
//===================================================================================
// Returns the current Linux OSD font list available from shared assets.
const std::vector<std::string>& LinuxOSDFontStorage::osdFontsList() const
{
    refreshFontsList();
    return m_font_names;
}

//===================================================================================
//===================================================================================
// Loads raw PNG bytes for a Linux OSD font from the shared assets directory.
bool LinuxOSDFontStorage::loadOSDFontBytes(const std::string& font_name, std::vector<unsigned char>& font_png) const
{
    const fs::path font_path = osdFontsDirectory() / font_name;
    std::ifstream file(font_path, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    font_png.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !font_png.empty();
}
