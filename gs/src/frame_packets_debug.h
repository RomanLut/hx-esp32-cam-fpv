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
    bool needBroken;
    int retryCount;

    bool copyToOSD(); //return true if dump contains lost block
    uint8_t getPacketTypeChar(uint32_t block_index, uint32_t packet_index, const uint8_t* data);
    void onPacketReceivedEx(uint32_t block_index, uint32_t packet_index, const uint8_t* data, bool old, bool recovered);

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


