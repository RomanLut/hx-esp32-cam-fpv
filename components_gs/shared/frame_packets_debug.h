#pragma once

#include <stdint.h>

#include "core/frame_packets_debug_core.h"

#define FPD_PACKETS_PER_BLOCK       12
#define FPD_BLOCKS_PER_ROW          4
#define FPD_ROWS                    20
#define FPD_BUFFER_SIZE             ( FPD_ROWS * FPD_BLOCKS_PER_ROW * FPD_PACKETS_PER_BLOCK )

//======================================================
//======================================================
class FramePacketsDebug
{
private:
    gs::core::FramePacketsDebugCore m_core;
    void copyToOSD();

public:

    FramePacketsDebug();
    void clear();
    void onPacketReceived(uint32_t block_index, uint32_t packet_index, const uint8_t* data, bool old);  //called for normal and fec packets
    void onPacketRestored(uint32_t block_index, uint32_t packet_index, const uint8_t* data);  //called for restored normal packets
    void off();
    bool isOn();
    void captureFrame(bool broken);
};

extern FramePacketsDebug g_framePacketsDebug;
