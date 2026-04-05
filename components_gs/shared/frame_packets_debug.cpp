#include "frame_packets_debug.h"

#include <algorithm>

#include "flight_osd.h"

FramePacketsDebug g_framePacketsDebug;

//======================================================
//======================================================
FramePacketsDebug::FramePacketsDebug()
{
    clear();
    off();
}

//======================================================
//======================================================
void FramePacketsDebug::clear()
{
    s_flightOSD.clear();
}

//======================================================
//======================================================
void FramePacketsDebug::clearBuffer()
{
    for (auto& row : m_buffer)
    {
        for (auto& block : row)
        {
            block.fill(kCharEmpty);
        }
    }
    for (auto& row : m_osd_chars)
    {
        row.fill(0);
    }
}

//======================================================
//======================================================
void FramePacketsDebug::off()
{
    m_state = State::Off;
    clearBuffer();
}

//======================================================
//======================================================
void FramePacketsDebug::captureFrame(bool broken)
{
    clearBuffer();
    m_state = State::SyncBlockStart;
    m_need_broken = broken;
    m_retry_count = 0;
}

//======================================================
//======================================================
bool FramePacketsDebug::isOn() const
{
    return m_state != State::Off;
}

//======================================================
//======================================================
bool FramePacketsDebug::isVisible() const
{
    return m_state == State::Showing;
}

//======================================================
//======================================================
void FramePacketsDebug::onPacketReceived(uint32_t block_index, uint32_t packet_index, const uint8_t* data, bool old)
{
    if (m_state == State::Off)
    {
        return;
    }

    if (m_state == State::SyncBlockStart)
    {
        if (old)
        {
            return;
        }
        m_first_block = block_index + 1;
        m_state = State::Capture;
    }

    if (m_state != State::Capture || block_index < m_first_block || packet_index >= kPacketsPerBlock)
    {
        return;
    }

    const uint32_t relative_block = block_index - m_first_block;
    const uint32_t row = relative_block / kBlocksPerRow;
    const uint32_t col = relative_block % kBlocksPerRow;
    if (row >= kRows)
    {
        const bool has_lost_block = buildOsdChars();
        m_state = State::Showing;
        if (!has_lost_block && m_need_broken && m_retry_count < 100)
        {
            const int retry_count = m_retry_count + 1;
            captureFrame(true);
            m_retry_count = retry_count;
        }
        copyToOSD();
        return;
    }

    uint8_t& cell = m_buffer[row][col][packet_index];
    if (cell == kCharEmpty)
    {
        cell = old ? kCharOldRejected : getPacketTypeChar(packet_index, data);
    }
}

//======================================================
//======================================================
void FramePacketsDebug::onPacketRestored(uint32_t block_index, uint32_t packet_index, const uint8_t* data)
{
    if (m_state != State::Capture || block_index < m_first_block || packet_index >= kPacketsPerBlock)
    {
        return;
    }

    const uint32_t relative_block = block_index - m_first_block;
    const uint32_t row = relative_block / kBlocksPerRow;
    const uint32_t col = relative_block % kBlocksPerRow;
    if (row >= kRows)
    {
        return;
    }

    m_buffer[row][col][packet_index] = getPacketTypeChar(packet_index, data);
}

//======================================================
//======================================================
void FramePacketsDebug::copyToOSD()
{
    s_flightOSD.setLowChars(m_osd_chars[0].data());
}

//======================================================
//======================================================
bool FramePacketsDebug::buildOsdChars()
{
    for (auto& row : m_osd_chars)
    {
        row.fill(0);
    }

    bool has_lost_block = false;
    const int packet_columns = std::min<int>(FPD_FEK_N > 0 ? FPD_FEK_N : kPacketsPerBlock, kPacketsPerBlock);

    for (int row = 0; row < kRows; ++row)
    {
        int osd_col = 0;
        for (int block = 0; block < kBlocksPerRow; ++block)
        {
            int block_count = 0;
            for (int packet = 0; packet < packet_columns; ++packet)
            {
                const uint8_t c = m_buffer[row][block][packet];
                if (c != kCharEmpty && c != kCharOldRejected && c != kCharUnknown)
                {
                    block_count++;
                }
                if (osd_col < OSD_COLS)
                {
                    m_osd_chars[row][osd_col] = c;
                }
                ++osd_col;
            }

            if (osd_col < OSD_COLS)
            {
                if (block_count < FPD_FEK_K)
                {
                    has_lost_block = true;
                    m_osd_chars[row][osd_col] = kCharBlockLost;
                }
                ++osd_col;
            }

            if (osd_col < OSD_COLS)
            {
                ++osd_col;
            }
        }
    }

    return has_lost_block;
}

//======================================================
//======================================================
uint8_t FramePacketsDebug::getPacketTypeChar(uint32_t packet_index, const uint8_t* data) const
{
    if (packet_index >= FPD_FEK_K)
    {
        return kCharFec;
    }

    if (data == nullptr)
    {
        return kCharUnknown;
    }

    const auto* header = reinterpret_cast<const Air2Ground_Header*>(data);
    if (header->type == Air2Ground_Header::Type::Video)
    {
        const auto* video = reinterpret_cast<const Air2Ground_Video_Packet*>(data);
        if (video->part_index == 0)
        {
            return video->last_part == 1 ? kCharFrameSingle : kCharFrameStart;
        }
        return video->last_part == 1 ? kCharFrameEnd : kCharFramePart;
    }
    if (header->type == Air2Ground_Header::Type::Telemetry)
    {
        return kCharTelemetry;
    }
    if (header->type == Air2Ground_Header::Type::OSD)
    {
        return kCharOsd;
    }
    if (header->type == Air2Ground_Header::Type::Config)
    {
        return kCharConfig;
    }
    return kCharUnknown;
}
