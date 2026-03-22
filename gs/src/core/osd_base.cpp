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

const OSDBuffer& OSDBase::buffer() const
{
    return m_buffer;
}

}
