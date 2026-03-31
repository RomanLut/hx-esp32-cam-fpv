#include "core/osd_base.h"

#include <cstring>

namespace gs::core
{

OSDBase::OSDBase()
{
    clear();
}

void OSDBase::clear()
{
    memset(&m_buffer, 0, OSD_BUFFER_SIZE);
}

void OSDBase::update(const uint8_t* data, uint16_t size)
{
    bool highBank = false;
    int count;

    int osdCol = 0;
    int osdRow = 0;

    int byteCount = 0;
    while (byteCount < size)
    {
        uint8_t c = data[byteCount++];
        if (c == 0)
        {
            c = data[byteCount++];
            count = (c & 0x7f);
            if (count == 0)
            {
                break;
            }
            highBank ^= (c & 128) != 0;
            c = data[byteCount++];
        }
        else if (c == 255)
        {
            highBank = !highBank;
            c = data[byteCount++];
            count = 1;
        }
        else
        {
            count = 1;
        }

        while (count > 0)
        {
            m_buffer.screenLow[osdRow][osdCol] = c;
            int col8 = osdCol >> 3;
            int sh = osdCol & 0x7;
            uint8_t mask = 1 << sh;
            m_buffer.screenHigh[osdRow][col8] =
                (m_buffer.screenHigh[osdRow][col8] & ~mask) | (highBank ? mask : 0);

            osdCol++;
            if (osdCol == OSD_COLS)
            {
                osdCol = 0;
                osdRow++;
                if (osdRow == OSD_ROWS)
                {
                    osdRow = 0;
                }
            }
            count--;
        }
    }
}

void OSDBase::setLowChar(int row, int col, uint8_t c)
{
    m_buffer.screenLow[row][col] = c;
}

void OSDBase::draw(int x1, int y1, int x2, int y2)
{
    const float screenWidth = static_cast<float>(x2 - x1 + 1);
    const float screenHeight = static_cast<float>(y2 - y1 + 1);

    float fxs = screenWidth / OSD_COLS;
    float fys = screenHeight / OSD_ROWS;

    int ixs = (int)fxs;
    int iys = (int)fys;

    int mx = ((int)screenWidth - OSD_COLS * ixs) / 2 + x1;
    int my = ((int)screenHeight - OSD_ROWS * iys) / 2 + y1;

    int y = my;
    for (int row = 0; row < OSD_ROWS; row++)
    {
        int x = mx;
        int mask = 1;
        int col8 = 0;
        for (int col = 0; col < OSD_COLS; col++)
        {
            uint16_t c = m_buffer.screenLow[row][col];
            if ((m_buffer.screenHigh[row][col8] & mask) != 0)
            {
                c += 0x100;
            }
            if (c != 0)
            {
                drawChar(c, x, y, ixs, iys);
            }
            x += ixs;
            mask <<= 1;
            if (mask == 0x100)
            {
                mask = 1;
                col8++;
            }
        }
        y += iys;
    }
}

void OSDBase::drawFitted(int viewport_width, int viewport_height, float content_aspect, float target_aspect)
{
    if (viewport_width <= 0 || viewport_height <= 0)
    {
        return;
    }

    int x1 = 0;
    int y1 = 0;
    int x2 = viewport_width;
    int y2 = viewport_height;

    if (content_aspect > 0.0f && target_aspect > 0.0f)
    {
        const int content_scaled = static_cast<int>(content_aspect * 100.0f + 0.5f);
        const int target_scaled = static_cast<int>(target_aspect * 100.0f + 0.5f);
        if (content_scaled != target_scaled)
        {
            if (content_aspect > target_aspect)
            {
                const int fitted_height = static_cast<int>(static_cast<float>(viewport_width) / content_aspect);
                const int margin_y = (viewport_height - fitted_height) / 2;
                y1 = margin_y;
                y2 = margin_y + fitted_height;
            }
            else
            {
                const int fitted_width = static_cast<int>(static_cast<float>(viewport_height) * content_aspect);
                const int margin_x = (viewport_width - fitted_width) / 2;
                x1 = margin_x;
                x2 = margin_x + fitted_width;
            }
        }
    }

    draw(x1, y1, x2, y2);
}

const OSDBuffer& OSDBase::buffer() const
{
    return m_buffer;
}

}
