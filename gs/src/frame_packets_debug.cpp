#include <string.h>

#include "frame_packets_debug.h"

#include "osd.h"

FramePacketsDebug g_framePacketsDebug;

#define FPD_EMPTY           0
#define FPD_RECEIVED        1
#define FPD_OLD_REGECTED    2
#define FPD_END             3

#define FPD_CHAR_RECEVIED   'O'
#define FPD_CHAR_RESTORED   'o'
#define FPD_CHAR_LOST       '-'
#define FPD_CHAR_END        'F'
#define FPD_CHAR_BLOCK_EDGE 'I'

#define FPD_STATE_OFF                       0
#define FPD_STATE_SYNC_BLOCK_START          1
#define FPD_STATE_SYNC_FRAME_START          2
#define FPD_STATE_CAPTURE_FRAME             3
#define FPD_STATE_SHOWING                   4

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
    memset( &this->buffer, FPD_EMPTY, FPD_BUFFER_SIZE );
}

//======================================================
//======================================================
void FramePacketsDebug::onPacketReceived(const Packet_Header* header, bool old)
{
    //if ( old ) return;

    //uint32_t block_index : 24;
    //uint32_t packet_index : 8;

    //choose block id to accumulate
    if ( this->state == FPD_STATE_SYNC_BLOCK_START ) 
    {
        this->first_block = header->block_index + 1;
        this->state = FPD_STATE_SYNC_FRAME_START;
    }
    
    if ( this->state == FPD_STATE_SYNC_FRAME_START ) 
    {
        if (  header->block_index < this->first_block ) return; 

        //packet is from the next block
        if ( header->block_index > this->first_block )
        {
            //was there a frame start in this block?
            if ( this->frameIndex == 0 )
            {
                //no, continue accumulating starting from the next block;
                this->clear();
                this->first_block = header->block_index;
                this->checkFrameStartAndEnd(header);
                this->buffer[0][0][header->packet_index] = this->gotFrameEnd ? FPD_END : FPD_RECEIVED;
                return;
            }
            
            //start capturing frame
            this->state = FPD_STATE_CAPTURE_FRAME;
        }
        else
        {
            this->checkFrameStartAndEnd(header);
            this->buffer[0][0][header->packet_index] = this->gotFrameEnd ? FPD_END : FPD_RECEIVED;
        }
    }
    
    if ( this->state == FPD_STATE_CAPTURE_FRAME ) 
    {
        if (  header->block_index < this->first_block ) return; 

        int row = (header->block_index - this->first_block) / FPD_BLOCKS_PER_ROW;
        int col = (header->block_index - this->first_block) % FPD_BLOCKS_PER_ROW;
        if ( row > FPD_ROWS ) 
        {
            //finished capturing
            copyToOSD();
            this->state = FPD_STATE_SHOWING;
            return;
        }

        this->checkFrameStartAndEnd(header);
        this->buffer[row][col][header->packet_index] = this->gotFrameEnd ? FPD_END : FPD_RECEIVED;
    }
}

//======================================================
//======================================================
void FramePacketsDebug::copyToOSD()
{
    this->clear();

    const uint8_t* ptr = (const uint8_t*)this->buffer;

    for ( int row = 0; row < FPD_ROWS; row++ )
    {
        int col = 0;
        for ( int block = 0; block < FPD_BLOCKS_PER_ROW; block++ )
        {
            for ( int packet = 0; packet < FPD_PACKETS_PER_BLOCK; packet++ )
            {
                if ( *ptr == FPD_RECEIVED )
                {
                    g_osd.setLowChar(row,col,FPD_CHAR_RECEVIED);
                }
                else if ( *ptr == FPD_END )
                {
                    g_osd.setLowChar(row,col,FPD_CHAR_END);
                }
                else
                {
                    g_osd.setLowChar(row,col,FPD_CHAR_LOST);
                }
                ptr++;
            }
            col++;
        }
        g_osd.setLowChar(row, col, FPD_CHAR_BLOCK_EDGE);
        col++;
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
    this->needBroken = broken;
    this->frameIndex = 0;
    this->gotFrameEnd = false;
}

//======================================================
//======================================================
void FramePacketsDebug::checkFrameStartAndEnd(const Packet_Header* header)
{
    //check for frame start packet
    const Air2Ground_Header* hdr2 = (const Air2Ground_Header*)((uint8_t*)header + sizeof(Packet_Header));
    if ( hdr2->type == Air2Ground_Header::Type::Video)
    {
        const Air2Ground_Video_Packet* hdr3 = (Air2Ground_Video_Packet*)((uint8_t*)hdr2);

        if ( this->frameIndex == 0 )
        {
            if ( hdr3->part_index == 0 )
            {
                //this packet is frame start
                this->frameIndex = hdr3->frame_index;
            }
        }
        else  if ( this->frameIndex != hdr3->frame_index )
        {
            this->gotFrameEnd = true;
        }
    }
}