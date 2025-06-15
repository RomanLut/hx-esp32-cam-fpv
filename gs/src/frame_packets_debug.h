#pragma once

#include <stdint.h>
#include "fec.h"

#define FPD_PACKETS_PER_BLOCK       12
#define FPD_BLOCKS_PER_ROW          4
#define FPD_ROWS                    20
#define FPD_BUFFER_SIZE             ( FPD_ROWS * FPD_BLOCKS_PER_ROW * FPD_PACKETS_PER_BLOCK )

//======================================================
//======================================================
class FramePacketsDebug
{
private:
    uint8_t buffer[FPD_ROWS][FPD_BLOCKS_PER_ROW][FPD_PACKETS_PER_BLOCK];

    uint8_t state;
    uint32_t first_block;

    void copyToOSD();
    uint8_t getPacketTypeChar(const Packet_Header* header);

public:

    FramePacketsDebug();
    void clear();
    void onPacketReceived(const Packet_Header* header, bool old);  //called for normal and fec packets
    //void onPacketRestored(const Packet_Header* header);  //called for restored normal packets
    void off();
    bool isOn();
    void captureFrame(bool broken);
};

extern FramePacketsDebug g_framePacketsDebug;


