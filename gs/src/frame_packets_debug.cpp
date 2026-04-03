#include <string.h>

#include "frame_packets_debug.h"

#include "linux_osd.h"

FramePacketsDebug g_framePacketsDebug;

#define FPD_FEK_K   6
#define FPD_FEK_N   12

//======================================================
//======================================================
FramePacketsDebug::FramePacketsDebug()
{
    this->clear();
    m_core.off();
}

//======================================================
//======================================================
void FramePacketsDebug::clear()
{
    g_osd.clear();
}

//======================================================
//======================================================
void FramePacketsDebug::onPacketReceived(uint32_t block_index, uint32_t packet_index, const uint8_t* data, bool old)
{
    m_core.onPacketReceived(block_index, packet_index, data, old, FPD_FEK_K, FPD_FEK_N);
    if (m_core.isVisible())
    {
        copyToOSD();
    }
}

//======================================================
//======================================================
void FramePacketsDebug::onPacketRestored(uint32_t block_index, uint32_t packet_index, const uint8_t* data)
{
    m_core.onPacketRestored(block_index, packet_index, data, FPD_FEK_K);
    if (m_core.isVisible())
    {
        copyToOSD();
    }
}

//======================================================
//======================================================
void FramePacketsDebug::copyToOSD()
{
    g_osd.clear();
    const auto& chars = m_core.osdChars();
    for (int row = 0; row < OSD_ROWS; row++)
    {
        for (int col = 0; col < OSD_COLS; col++)
        {
            const uint8_t c = chars[row][col];
            if (c != 0)
            {
                g_osd.setLowChar(row, col, c);
            }
        }
    }
}

//======================================================
//======================================================
bool FramePacketsDebug::isOn()
{
    return m_core.isOn();
}

//======================================================
//======================================================
void FramePacketsDebug::off()
{
    m_core.off();
}

//======================================================
//======================================================
void FramePacketsDebug::captureFrame(bool broken)
{
    m_core.captureFrame(broken);
}
