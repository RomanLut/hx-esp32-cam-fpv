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

#define FPD_CHAR_BLOCK_OK       ' '
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
void FramePacketsDebug::onPacketReceived(const Packet_Header* header, bool old)
{
    //choose block id to accumulate
    if ( this->state == FPD_STATE_SYNC_BLOCK_START ) 
    {
        this->first_block = header->block_index + 1;
        this->state = FPD_STATE_CAPTURE;
    }
    
    if ( this->state == FPD_STATE_CAPTURE ) 
    {
        if (  header->block_index < this->first_block ) return; 

        int row = (header->block_index - this->first_block) / FPD_BLOCKS_PER_ROW;
        int col = (header->block_index - this->first_block) % FPD_BLOCKS_PER_ROW;
        if ( row >= FPD_ROWS )
        {
            copyToOSD();
            this->state = FPD_STATE_SHOWING;
            return;
        }

        uint8_t c = this->getPacketTypeChar(header);
        if ( !old )
        {
            this->buffer[row][col][header->packet_index] = c;
        }
        else
        {
            if (this->buffer[row][col][header->packet_index] == FPD_CHAR_EMPTY)
            {
                this->buffer[row][col][header->packet_index] = FPD_CHAR_OLD_REJECTED;
            } 
        }
    }
}

//======================================================
//======================================================
void FramePacketsDebug::copyToOSD()
{
    const uint8_t* ptr = (const uint8_t*)this->buffer;

    for ( int row = 0; row < FPD_ROWS; row++ )
    {
        int col = 0;
        for ( int block = 0; block < FPD_BLOCKS_PER_ROW; block++ )
        {
            int bc = 0;
            for ( int packet = 0; packet < FPD_PACKETS_PER_BLOCK; packet++ )
            {
                g_osd.setLowChar(row,col,*ptr);
                ptr++;
                col++;
                if ( (*ptr != FPD_CHAR_EMPTY) && (*ptr != FPD_CHAR_OLD_REJECTED) ) bc++;
            }
            g_osd.setLowChar(row, col, bc >= FPD_FEK_K ? FPD_CHAR_BLOCK_OK : FPD_CHAR_BLOCK_LOST );
            col++;
        }
    }
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
    this->state = FPD_STATE_SYNC_BLOCK_START;
}

//======================================================
//======================================================
uint8_t FramePacketsDebug::getPacketTypeChar(const Packet_Header* header)
{
    if ( header->packet_index >= FPD_FEK_K )
    {
        return FPD_CHAR_FEC;
    }

    const Air2Ground_Header* hdr2 = (const Air2Ground_Header*)((uint8_t*)header + sizeof(Packet_Header));
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
