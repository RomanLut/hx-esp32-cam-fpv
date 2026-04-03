#include "gs_asset_flight_osd.h"

#include <cstdio>

GsAssetFlightOsd::GsAssetFlightOsd()
    : m_font_name("INAV_default_24.png")
{
}

GsAssetFlightOsd::~GsAssetFlightOsd()
{
    delete m_font;
}

void GsAssetFlightOsd::setFontName(const std::string& font_name)
{
    if (font_name.empty() || font_name == m_font_name)
    {
        return;
    }

    m_font_name = font_name;
    m_font_dirty = true;
    m_font_failed = false;
}

bool GsAssetFlightOsd::ensureFont()
{
    if (!m_font_dirty)
    {
        return m_font != nullptr && m_font->loaded;
    }

    delete m_font;
    m_font = nullptr;
    m_font_png.clear();
    m_font_dirty = false;

    if (!s_OSDFontStorage->loadOSDFontBytes(m_font_name, m_font_png))
    {
        std::fprintf(stderr, "Failed to load OSD font bytes: %s\n", m_font_name.c_str());
        m_font_failed = true;
        return false;
    }

    m_font = new FontWalksnail(m_font_png.data(), m_font_png.size());
    if (m_font == nullptr || !m_font->loaded)
    {
        std::fprintf(stderr, "Failed to create OSD font atlas: %s\n", m_font_name.c_str());
        delete m_font;
        m_font = nullptr;
        m_font_failed = true;
        return false;
    }

    m_font_failed = false;
    return true;
}

void GsAssetFlightOsd::drawChar(uint16_t code, int x, int y, int width, int height)
{
    if (m_font == nullptr)
    {
        return;
    }

    m_font->drawChar(code, x, y, width, height);
}
