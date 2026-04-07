#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fontwalksnail.h"
#include "packets.h"

//===================================================================================
//===================================================================================
// Implements the shared flight OSD buffer, font management and draw path.
class FlightOSD
{
public:
    FlightOSD();
    ~FlightOSD();

    const std::string& defaultFontName() const;
    std::string selectedFontName() const;
    void setSelectedFontName(const std::string& font_name);
    std::optional<std::string> findFontNameByCrc(const std::vector<std::string>& font_names, uint32_t font_crc32) const;

    void clear();
    void update(const uint8_t* data, uint16_t size);
    void setLowChars(const uint8_t* low_chars);

    void setFontName(const std::string& font_name);
    bool loadFont(const char* font_name);
    const std::string& currentFontName() const;
    bool isFontError() const;
    void invalidateFontAtlas();

    void draw(int surface_width,
              int surface_height,
              int frame_width,
              int frame_height,
              int screen_mode,
              bool vr_mode);

private:
    bool ensureFont();
    void drawChar(uint16_t code, int x, int y, int width, int height);
    void drawInRect(const OSDBuffer& buffer, int x1, int y1, int x2, int y2);

    mutable std::mutex m_mutex;
    OSDBuffer m_buffer = {};
    std::string m_font_name;
    std::vector<unsigned char> m_font_png;
    FontWalksnail* m_font = nullptr;
    bool m_font_dirty = true;
    bool m_font_failed = false;
};

extern FlightOSD s_flightOSD;
