#pragma once

#include <array>
#include <stdint.h>

#include "packets.h"

#define FPD_FEK_K   6
#define FPD_FEK_N   12

//======================================================
//======================================================
class FramePacketsDebug
{
public:
    FramePacketsDebug();
    void clear();
    void onPacketReceived(uint32_t block_index, uint32_t packet_index, const uint8_t* data, bool old);
    void onPacketRestored(uint32_t block_index, uint32_t packet_index, const uint8_t* data);
    void off();
    bool isOn() const;
    bool isVisible() const;
    void captureFrame(bool broken);

private:
    static constexpr int kPacketsPerBlock = 12;
    static constexpr int kBlocksPerRow = 4;
    static constexpr int kRows = 20;

    static constexpr uint8_t kCharEmpty = '-';
    static constexpr uint8_t kCharFrameStart = 'F';
    static constexpr uint8_t kCharFramePart = 'P';
    static constexpr uint8_t kCharFrameEnd = 'E';
    static constexpr uint8_t kCharFrameSingle = 'B';
    static constexpr uint8_t kCharTelemetry = 'T';
    static constexpr uint8_t kCharConfig = 'C';
    static constexpr uint8_t kCharOsd = 'O';
    static constexpr uint8_t kCharUnknown = '?';
    static constexpr uint8_t kCharFec = '*';
    static constexpr uint8_t kCharOldRejected = 'J';
    static constexpr uint8_t kCharBlockLost = '!';

    enum class State : uint8_t
    {
        Off = 0,
        SyncBlockStart = 1,
        Capture = 2,
        Showing = 3,
    };

    void clearBuffer();
    void copyToOSD();
    bool buildOsdChars();
    uint8_t getPacketTypeChar(uint32_t packet_index, const uint8_t* data) const;

    std::array<std::array<std::array<uint8_t, kPacketsPerBlock>, kBlocksPerRow>, kRows> m_buffer = {};
    std::array<std::array<uint8_t, OSD_COLS>, OSD_ROWS> m_osd_chars = {};
    State m_state = State::Off;
    uint32_t m_first_block = 0;
    bool m_need_broken = false;
    int m_retry_count = 0;
};

extern FramePacketsDebug g_framePacketsDebug;
