#pragma once

#include "gs_flight_osd.h"
#include "core/osd_base.h"
#include "fontwalksnail.h"

#include <string>
#include <vector>

class GsAssetFlightOsd : public GsFlightOsd, public gs::core::OSDBase
{
public:
    GsAssetFlightOsd();
    ~GsAssetFlightOsd() override;

    void update(const uint8_t* data, uint16_t size) override { OSDBase::update(data, size); }
    void clear() override { OSDBase::clear(); }
    void setLowChar(int row, int col, uint8_t value) override { OSDBase::setLowChar(row, col, value); }
    void setFontName(const std::string& font_name) override;

protected:
    bool ensureFont();
    void drawChar(uint16_t code, int x, int y, int width, int height) override;
    virtual bool loadFontBytes(const std::string& font_name, std::vector<unsigned char>& font_png) = 0;
    virtual void onFontLoadError(const std::string& message, const std::string& font_name) = 0;

private:
    std::string m_font_name;
    std::vector<unsigned char> m_font_png;
    FontWalksnail* m_font = nullptr;
    bool m_font_dirty = true;
    bool m_font_failed = false;
};
