#include "jpeg_parser.h"

//=============================================================================================
//=============================================================================================
bool getJPEGDimensions(uint8_t* buffer, int& width, int& height, size_t maxSearchLength) 
{
    size_t i = 2; //skip ff d8

    while (true) 
    {
        // Check for the 0xFF marker
        if (buffer[i] == 0xFF) 
        {
            uint8_t marker = buffer[i + 1];

            // Check if it's a start of frame marker (0xC0 - 0xC3)
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2 || marker == 0xC3) 
            {
                height = (buffer[i + 5] << 8) + buffer[i + 6];
                width = (buffer[i + 7] << 8) + buffer[i + 8];
                return true;
            }
            
            // Skip this marker segment
            uint16_t segmentLength = (buffer[i + 2] << 8) + buffer[i + 3];
            i += 2 + segmentLength;
        } 
        else 
        {
            i++;
        }
        if ( i > maxSearchLength ) return false;
    }
    
    return false;
}