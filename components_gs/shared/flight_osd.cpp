#include "flight_osd.h"

#include <cstring>

#include "Log.h"
#include "lodepng.h"
#include "gs_runtime_osd_font_storage.h"
#include "gs_shared_runtime.h"

namespace
{

constexpr const char* kDefaultOsdFontName = "INAV_default_24.png";

//===================================================================================
//===================================================================================
// Computes the video-aligned destination rectangle used for OSD drawing.
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

//===================================================================================
//===================================================================================
// Returns the shared fallback OSD font name used when no explicit selection exists.
const std::string& FlightOSD::defaultFontName() const
{
    static const std::string s_default_osd_font_name = kDefaultOsdFontName;
    return s_default_osd_font_name;
}

//===================================================================================
//===================================================================================
// Reads the persisted OSD font selection or falls back to the built-in default.
std::string FlightOSD::selectedFontName() const
{
    std::string font_name = s_settingsStorage["gs"]["osd_font"];
    if (font_name.empty())
    {
        return kDefaultOsdFontName;
    }
    return font_name;
}

//===================================================================================
//===================================================================================
// Persists the selected OSD font file name into shared settings storage.
void FlightOSD::setSelectedFontName(const std::string& font_name)
{
    s_settingsStorage["gs"]["osd_font"] = font_name;
}

//===================================================================================
//===================================================================================
// Finds the first font file name whose crc32 matches the stored selection value.
std::optional<std::string> FlightOSD::findFontNameByCrc(const std::vector<std::string>& font_names,
                                                        uint32_t font_crc32) const
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

//===================================================================================
//===================================================================================
// Constructs the shared flight OSD with its default font selection.
FlightOSD::FlightOSD()
    : m_font_name(defaultFontName())
{
    clear();
}

//===================================================================================
//===================================================================================
// Releases the current flight OSD font atlas.
FlightOSD::~FlightOSD()
{
    delete m_font;
}

//===================================================================================
//===================================================================================
// Clears the whole OSD character buffer.
void FlightOSD::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::memset(&m_buffer, 0, OSD_BUFFER_SIZE);
}

//===================================================================================
//===================================================================================
// Updates the OSD character buffer from the packed incoming DisplayPort payload.
void FlightOSD::update(const uint8_t* data, uint16_t size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    bool high_bank = false;
    int count = 0;
    int osd_col = 0;
    int osd_row = 0;
    int byte_count = 0;

    while (byte_count < size)
    {
        uint8_t c = data[byte_count++];
        if (c == 0)
        {
            c = data[byte_count++];
            count = (c & 0x7f);
            if (count == 0)
            {
                break;
            }
            high_bank ^= (c & 128) != 0;
            c = data[byte_count++];
        }
        else if (c == 255)
        {
            high_bank = !high_bank;
            c = data[byte_count++];
            count = 1;
        }
        else
        {
            count = 1;
        }

        while (count > 0)
        {
            m_buffer.screenLow[osd_row][osd_col] = c;
            const int col8 = osd_col >> 3;
            const int sh = osd_col & 0x7;
            const uint8_t mask = 1 << sh;
            m_buffer.screenHigh[osd_row][col8] =
                (m_buffer.screenHigh[osd_row][col8] & ~mask) | (high_bank ? mask : 0);

            osd_col++;
            if (osd_col == OSD_COLS)
            {
                osd_col = 0;
                osd_row++;
                if (osd_row == OSD_ROWS)
                {
                    osd_row = 0;
                }
            }
            count--;
        }
    }
}

//===================================================================================
//===================================================================================
// Copies a flat OSD_ROWS*OSD_COLS array into screenLow and clears screenHigh.
void FlightOSD::setLowChars(const uint8_t* low_chars)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::memcpy(m_buffer.screenLow, low_chars, OSD_ROWS * OSD_COLS);
    std::memset(m_buffer.screenHigh, 0, OSD_ROWS * OSD_COLS_H);
}

//===================================================================================
//===================================================================================
// Changes the selected OSD font name and invalidates the current font atlas.
void FlightOSD::setFontName(const std::string& font_name)
{
    if (font_name.empty() || font_name == m_font_name)
    {
        return;
    }

    m_font_name = font_name;
    m_font_dirty = true;
    m_font_failed = false;
}

//===================================================================================
//===================================================================================
// Selects and loads the named font immediately through the shared storage backend.
bool FlightOSD::loadFont(const char* font_name)
{
    setFontName(font_name != nullptr ? font_name : "");
    return ensureFont();
}

//===================================================================================
//===================================================================================
// Returns the currently selected OSD font file name.
const std::string& FlightOSD::currentFontName() const
{
    return m_font_name;
}

//===================================================================================
//===================================================================================
// Reports whether the currently selected OSD font failed to load.
bool FlightOSD::isFontError() const
{
    return m_font_failed;
}

//===================================================================================
//===================================================================================
// Invalidates the current GL font atlas so it will be rebuilt in the next context.
void FlightOSD::invalidateFontAtlas()
{
    delete m_font;
    m_font = nullptr;
    m_font_dirty = true;
    m_font_failed = false;
}

//===================================================================================
//===================================================================================
// Ensures the current OSD font atlas is loaded from the platform font storage.
bool FlightOSD::ensureFont()
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
        LOGE("Failed to load OSD font bytes: {}", m_font_name);
        m_font_failed = true;
        return false;
    }

    m_font = new FontWalksnail(m_font_png.data(), m_font_png.size());
    if (m_font == nullptr || !m_font->loaded)
    {
        LOGE("Failed to create OSD font atlas: {}", m_font_name);
        delete m_font;
        m_font = nullptr;
        m_font_failed = true;
        return false;
    }

    LOGI("OSD font loaded ok: {}", m_font_name);
    m_font_failed = false;
    return true;
}

//===================================================================================
//===================================================================================
// Draws the OSD aligned to the current frame rectangle.
void FlightOSD::draw(int surface_width,
                     int surface_height,
                     int frame_width,
                     int frame_height,
                     int screen_mode)
{
    if (!ensureFont())
    {
        LOGE("FlightOSD::draw ensureFont failed, skipping draw");
        return;
    }

    OSDBuffer buffer_snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        buffer_snapshot = m_buffer;
    }

    int x1 = 0;
    int y1 = 0;
    int x2 = surface_width;
    int y2 = surface_height;
    computeVideoBounds(surface_width, surface_height, frame_width, frame_height, screen_mode, x1, y1, x2, y2);
    drawInRect(buffer_snapshot, x1, y1, x2, y2);
}

//===================================================================================
//===================================================================================
// Draws the buffered OSD characters into the supplied output rectangle.
void FlightOSD::drawInRect(const OSDBuffer& buffer, int x1, int y1, int x2, int y2)
{
    const float screen_width = static_cast<float>(x2 - x1 + 1);
    const float screen_height = static_cast<float>(y2 - y1 + 1);

    const int cell_width = static_cast<int>(screen_width / OSD_COLS);
    const int cell_height = static_cast<int>(screen_height / OSD_ROWS);

    const int margin_x = (static_cast<int>(screen_width) - OSD_COLS * cell_width) / 2 + x1;
    const int margin_y = (static_cast<int>(screen_height) - OSD_ROWS * cell_height) / 2 + y1;

    int y = margin_y;
    for (int row = 0; row < OSD_ROWS; row++)
    {
        int x = margin_x;
        int mask = 1;
        int col8 = 0;
        for (int col = 0; col < OSD_COLS; col++)
        {
            uint16_t c = buffer.screenLow[row][col];
            if ((buffer.screenHigh[row][col8] & mask) != 0)
            {
                c += 0x100;
            }
            if (c != 0)
            {
                drawChar(c, x, y, cell_width, cell_height);
            }
            x += cell_width;
            mask <<= 1;
            if (mask == 0x100)
            {
                mask = 1;
                col8++;
            }
        }
        y += cell_height;
    }
}

//===================================================================================
//===================================================================================
// Draws a single OSD glyph through the current font atlas.
void FlightOSD::drawChar(uint16_t code, int x, int y, int width, int height)
{
    if (m_font == nullptr)
    {
        return;
    }

    m_font->drawChar(code, x, y, width, height);
}
