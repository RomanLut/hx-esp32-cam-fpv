#include <cstring>
#include <cstring>
#include "esp_timer.h"

#include "osd.h"

OSD g_osd;

//==============================================================
//==============================================================
OSD::OSD()
{
    this->clear();
    this->changed = true;
    this->lockCounter = 0;
}

//==============================================================
//==============================================================
void OSD::clear()
{
    memset( &this->buffer, 0, OSD_BUFFER_SIZE );
    this->lockCounter = 30; //lock for 30 seconds max (30 fps update)
}

//==============================================================
//==============================================================
void OSD::commit()
{
    this->changed = true;
    this->lockCounter = 0;
}


//==============================================================
//==============================================================
void OSD::writeString(unsigned int row, unsigned int col, int isExtChar, uint8_t* str, int len)
{
    if ( row >= OSD_ROWS ) return;

    uint8_t flag = isExtChar ? 0xff : 0;
    
    uint8_t* pScreenLowRow = &(this->buffer.screenLow[row][0]);
    uint8_t* pScreenHighRow = &(this->buffer.screenHigh[row][0]);
    while ( (len > 0) && (col < OSD_COLS) )
    {
        pScreenLowRow[col] = *str++;

        int col8 = col >> 3;
        int sh = col & 0x7;
        uint8_t m = 1 << sh;
        pScreenHighRow[col8] = (pScreenHighRow[col8] & ~m) | (m & flag);

        len--;
        col++;
    }
}

//==============================================================
//==============================================================
void* OSD::getBuffer()
{
    return &this->buffer;
}

//==============================================================
//==============================================================
bool OSD::isChanged()
{
    /*
    for ( int i = 0; i < 10; i++)
    {
        this->buffer.screenLow[10][10+i]++;
    }
    this->changed = true;
    */

    bool res = this->changed;
    this->changed = false;
    return res;
}

//==============================================================
//==============================================================
bool OSD::isLocked()
{
    if ( this->lockCounter == 0 ) return false;
    this->lockCounter--;
    return true;
}