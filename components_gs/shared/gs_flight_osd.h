#pragma once

#include <cstdint>
#include <string>

class GsFlightOsd
{
public:
    virtual ~GsFlightOsd() = default;

    virtual void update(const uint8_t* data, uint16_t size) = 0;
    virtual void clear() = 0;
    virtual void setLowChar(int row, int col, uint8_t value) = 0;
    virtual void setFontName(const std::string& font_name) = 0;
    virtual void draw(int surface_width,
                      int surface_height,
                      int frame_width,
                      int frame_height,
                      int screen_mode,
                      bool vr_mode) = 0;
};
