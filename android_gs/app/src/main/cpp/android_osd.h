#pragma once

#include "core/osd_base.h"
#include "fontwalksnail.h"

#include <string>
#include <vector>

class AndroidOSD final : public gs::core::OSDBase
{
public:
    AndroidOSD();
    ~AndroidOSD() override;

    void setFontName(const std::string& font_name);
    void draw(int surface_width, int surface_height, int frame_width, int frame_height, int screen_mode);

protected:
    void drawChar(uint16_t code, int x, int y, int width, int height) override;

private:
    bool ensureFont();

    std::string m_font_name;
    std::vector<unsigned char> m_font_png;
    FontWalksnail* m_font = nullptr;
    bool m_font_dirty = true;
    bool m_font_failed = false;
};
