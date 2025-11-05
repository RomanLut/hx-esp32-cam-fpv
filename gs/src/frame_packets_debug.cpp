#include <string.h>

#include "frame_packets_debug.h"

#include "osd.h"

FramePacketsDebug g_framePacketsDebug;

#define FPD_CHAR_EMPTY          '-'
#define FPD_CHAR_FRAME_START    'F'
#define FPD_CHAR_FRAME_PART     'P'
#define FPD_CHAR_FRAME_END      'E'
#define FPD_CHAR_FRAME_SINGLE   'B'
#define FPD_CHAR_TELEMETRY      'T'
#define FPD_CHAR_CONFIG         'C'
#define FPD_CHAR_OSD            'O'
#define FPD_CHAR_UNKNOWN        '?'
#define FPD_CHAR_FEC            '*'

#define FPD_CHAR_OLD_REJECTED   'J'

#define FPD_CHAR_BLOCK_LOST     '!'

#define FPD_STATE_OFF                       0
#define FPD_STATE_SYNC_BLOCK_START          1
#define FPD_STATE_CAPTURE                   2
#define FPD_STATE_SHOWING                   3

#define FPD_FEK_K   6

//======================================================
//======================================================
FramePacketsDebug::FramePacketsDebug()
{
    this->clear();
    this->state = FPD_STATE_OFF;
}

//======================================================
//======================================================
void FramePacketsDebug::clear()
{
    memset( &this->buffer, FPD_CHAR_EMPTY, FPD_BUFFER_SIZE );
}

//======================================================
//======================================================
void FramePacketsDebug::onPacketReceived(uint32_t block_index, uint32_t packet_index, const uint8_t* data, bool old)
{
    this->onPacketReceivedEx( block_index, packet_index, data, old, false );
}

//======================================================
//======================================================
void FramePacketsDebug::onPacketRestored(uint32_t block_index, uint32_t packet_index, const uint8_t* data)
{
//    this->onPacketReceivedEx( block_index, packet_index, data, false, true );
}

//======================================================
//======================================================
void FramePacketsDebug::onPacketReceivedEx(uint32_t block_index, uint32_t packet_index, const uint8_t* data, bool old, bool recovered)
{
    //choose block id to accumulate
    if ( this->state == FPD_STATE_SYNC_BLOCK_START ) 
    {
        if ( old ) return;
        this->first_block = block_index + 1;
        this->state = FPD_STATE_CAPTURE;
    }
    
    if ( this->state == FPD_STATE_CAPTURE ) 
    {
        if (  block_index < this->first_block ) return; 

        uint32_t row = (block_index - this->first_block) / FPD_BLOCKS_PER_ROW;
        uint32_t col = (block_index - this->first_block) % FPD_BLOCKS_PER_ROW;
        if ( row >= FPD_ROWS )
        {
            bool r = this->copyToOSD();
            this->state = FPD_STATE_SHOWING;
            if (!r && this->needBroken && (this->retryCount < 100 ))
            {
                int i = this->retryCount++;
                this->captureFrame(true);
                this->retryCount = i;
            }
            return;
        }

        uint8_t c = this->getPacketTypeChar(block_index, packet_index, data);

        if ( !old )
        {
            if (this->buffer[row][col][packet_index] == FPD_CHAR_EMPTY)
            {
                this->buffer[row][col][packet_index] = c;
            }
        }
        else
        {
            if (this->buffer[row][col][packet_index] == FPD_CHAR_EMPTY)
            {
                this->buffer[row][col][packet_index] = FPD_CHAR_OLD_REJECTED;
            } 
        }
    }
}

//======================================================
//======================================================
bool FramePacketsDebug::copyToOSD()
{
    g_osd.clear();

    const uint8_t* ptr = (const uint8_t*)this->buffer;
    bool hasLostBock = false;

    for ( int row = 0; row < FPD_ROWS; row++ )
    {
        int col = 0;
        for ( int block = 0; block < FPD_BLOCKS_PER_ROW; block++ )
        {
            int bc = 0;
            for ( int packet = 0; packet < FPD_PACKETS_PER_BLOCK; packet++ )
            {
                g_osd.setLowChar( row, col, *ptr );
                if ( (*ptr != FPD_CHAR_EMPTY) && (*ptr != FPD_CHAR_OLD_REJECTED) && (*ptr != FPD_CHAR_UNKNOWN)) bc++;
                ptr++;
                col++;
            }

            if ( bc < FPD_FEK_K )
            {
                hasLostBock = true;
                g_osd.setLowChar( row, col, FPD_CHAR_BLOCK_LOST );
            }
            col++;
        }
    }

    return hasLostBock;
}


//======================================================
//======================================================
bool FramePacketsDebug::isOn()
{
    return this->state != FPD_STATE_OFF;
}

//======================================================
//======================================================
void FramePacketsDebug::off()
{
    this->state = FPD_STATE_OFF;
}

//======================================================
//======================================================
void FramePacketsDebug::captureFrame(bool broken)
{
    this->clear();
    this->state = FPD_STATE_SYNC_BLOCK_START;
    this->needBroken = broken;
    this->retryCount = 0;
}

//======================================================
//======================================================
uint8_t FramePacketsDebug::getPacketTypeChar(uint32_t block_index, uint32_t packet_index, const uint8_t* data)
{
    if ( packet_index >= FPD_FEK_K )
    {
        return FPD_CHAR_FEC;
    }

    const Air2Ground_Header* hdr2 = (const Air2Ground_Header*)data;
    if ( hdr2->type == Air2Ground_Header::Type::Video)
    {
        const Air2Ground_Video_Packet* hdr3 = (Air2Ground_Video_Packet*)((uint8_t*)hdr2);
        if ( hdr3->part_index == 0 )
        {
            return hdr3->last_part == 1 ? FPD_CHAR_FRAME_SINGLE : FPD_CHAR_FRAME_START;
        }
        else 
        {
            return hdr3->last_part == 1 ? FPD_CHAR_FRAME_END : FPD_CHAR_FRAME_PART;
        }
    }
    else if ( hdr2->type == Air2Ground_Header::Type::Telemetry)
    {
        return FPD_CHAR_TELEMETRY;
    }
    else if ( hdr2->type == Air2Ground_Header::Type::OSD)
    {
        return FPD_CHAR_OSD;
    }
    else if ( hdr2->type == Air2Ground_Header::Type::Config)
    {
        return FPD_CHAR_CONFIG;
    }
    else
    {
        return FPD_CHAR_UNKNOWN;
    }
}
