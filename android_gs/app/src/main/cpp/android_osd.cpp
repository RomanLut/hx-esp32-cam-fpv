#include "android_osd.h"

#include <GLES2/gl2.h>
#include <android/asset_manager.h>
#include <android/log.h>

#include "android_jni_shared.h"
#include "imgui.h"
#include <algorithm>
#include <vector>

namespace
{

constexpr const char* kLogTag = "AndroidGSOSD";
void computeVideoBounds(int surface_width,
                        int surface_height,
                        int frame_width,
                        int frame_height,
                        int screen_mode,
                        int& x1,
                        int& y1,
                        int& x2,
                        int& y2)
{
    x1 = 0;
    y1 = 0;
    x2 = surface_width;
    y2 = surface_height;

    if (surface_width <= 0 || surface_height <= 0 || frame_width <= 0 || frame_height <= 0)
    {
        return;
    }

    if (screen_mode != 1)
    {
        return;
    }

    const float video_aspect = static_cast<float>(frame_width) / static_cast<float>(frame_height);
    const float screen_aspect = static_cast<float>(surface_width) / static_cast<float>(surface_height);

    if (video_aspect > screen_aspect)
    {
        const int fitted_height = static_cast<int>(static_cast<float>(surface_width) / video_aspect);
        const int margin_y = (surface_height - fitted_height) / 2;
        y1 = margin_y;
        y2 = margin_y + fitted_height;
    }
    else
    {
        const int fitted_width = static_cast<int>(static_cast<float>(surface_height) * video_aspect);
        const int margin_x = (surface_width - fitted_width) / 2;
        x1 = margin_x;
        x2 = margin_x + fitted_width;
    }
}

} // namespace

AndroidOSD::AndroidOSD()
    : m_font_name("INAV_default_24.png")
{
}

AndroidOSD::~AndroidOSD()
{
    delete m_font;
}

void AndroidOSD::setFontName(const std::string& font_name)
{
    if (font_name.empty() || font_name == m_font_name)
    {
        return;
    }

    m_font_name = font_name;
    m_font_dirty = true;
    m_font_failed = false;
}

void AndroidOSD::draw(int surface_width, int surface_height, int frame_width, int frame_height, int screen_mode)
{
    if (!ensureFont())
    {
        return;
    }

    int x1 = 0;
    int y1 = 0;
    int x2 = surface_width;
    int y2 = surface_height;
    computeVideoBounds(surface_width, surface_height, frame_width, frame_height, screen_mode, x1, y1, x2, y2);
    OSDBase::draw(x1, y1, x2, y2);
}

void AndroidOSD::drawChar(uint16_t code, int x, int y, int width, int height)
{
    if (m_font == nullptr)
    {
        return;
    }
    m_font->drawChar(code, x, y, width, height);
}

bool AndroidOSD::ensureFont()
{
    if (!m_font_dirty)
    {
        return m_font != nullptr && m_font->loaded;
    }

    delete m_font;
    m_font = nullptr;
    m_font_png.clear();
    m_font_dirty = false;

    AAssetManager* asset_manager = androidGetAssetManager();
    if (asset_manager == nullptr)
    {
        if (!m_font_failed)
        {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Asset manager is not set");
            m_font_failed = true;
        }
        return false;
    }

    const std::string asset_path = "osd_fonts/" + m_font_name;
    AAsset* asset = AAssetManager_open(asset_manager, asset_path.c_str(), AASSET_MODE_BUFFER);
    if (asset == nullptr)
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to open OSD font asset: %s", asset_path.c_str());
        m_font_failed = true;
        return false;
    }

    const off_t asset_size = AAsset_getLength(asset);
    if (asset_size <= 0)
    {
        AAsset_close(asset);
        m_font_failed = true;
        return false;
    }

    m_font_png.resize(static_cast<size_t>(asset_size));
    const int read_size = AAsset_read(asset, m_font_png.data(), static_cast<size_t>(asset_size));
    AAsset_close(asset);
    if (read_size != asset_size)
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to read OSD font asset: %s", asset_path.c_str());
        m_font_failed = true;
        return false;
    }

    m_font = new FontWalksnail(m_font_png.data(), m_font_png.size());
    if (m_font == nullptr || !m_font->loaded)
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create OSD font atlas: %s", asset_path.c_str());
        delete m_font;
        m_font = nullptr;
        m_font_failed = true;
        return false;
    }
    return true;
}
