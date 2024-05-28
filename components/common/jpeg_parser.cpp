#include <iostream>
#include <vector>
#include <cstring>

#include "jpeg_parser.h"

//=============================================================================================
//=============================================================================================
bool check_jfif(Circular_Buffer &buffer, size_t pos) 
{
    return buffer.peek(pos) == 'J' &&
           buffer.peek(pos + 1) == 'F' &&
           buffer.peek(pos + 2) == 'I' &&
           buffer.peek(pos + 3) == 'F';
}

//=============================================================================================
//=============================================================================================
int find_jpeg_in_buffer1(Circular_Buffer &buffer, size_t buffer_size, JpegInfo *jpeg_info) 
{
    // Searching for JPEG start (SOI) marker
    for (size_t i = 0; i <= buffer_size - 3; i++) 
    {
        if (buffer.peek(i) == 0xFF && buffer.peek(i + 1) == 0xD8 && buffer.peek(i + 2) == 0xFF) 
        {
            size_t pos = i + 2;
            while (pos < buffer_size - 2) 
            {
                if (buffer.peek(pos) == 0xFF) 
                {
                    uint8_t marker = buffer.peek(pos + 1);
                    if (marker == 0xE0) 
                    {  
                        // APP0 marker (could contain JFIF)
                        if (((pos + 4 + 4) <= buffer_size) && check_jfif(buffer, pos + 4)) 
                        {
                            jpeg_info->offset = i;

                            // Parse the JPEG file size and resolution
                            size_t segment_pos = pos;
                            while (segment_pos < buffer_size - 2) 
                            {
                                if (buffer.peek(segment_pos) == 0xFF) 
                                {
                                    uint8_t segment_marker = buffer.peek(segment_pos + 1);
                                    
                                    if (segment_marker >= 0xD0 && segment_marker <= 0xD7) 
                                    {
                                        segment_pos += 2;  // Skip the restart marker
                                        continue;
                                    }                                    
                                    if (segment_marker == 0xC0 || segment_marker == 0xC2) 
                                    { 
                                        // Start of frame markers (baseline or progressive)
                                        if (segment_pos + 8 >= buffer_size) 
                                        {
                                            // Truncated frame segment
                                            return -2;
                                        }
                                        jpeg_info->height = (buffer.peek(segment_pos + 5) << 8) + buffer.peek(segment_pos + 6);
                                        jpeg_info->width = (buffer.peek(segment_pos + 7) << 8) + buffer.peek(segment_pos + 8);
                                    }
                                    if (segment_marker == 0xD9) 
                                    {  
                                        // EOI (End of Image)
                                        return -2;  //should have SOS Segment
                                    } 
                                    else 
                                    {
                                        // Skip the current marker segment
                                        if (segment_pos + 3 >= buffer_size) 
                                        {
                                            // Truncated segment length field
                                            return -2;
                                        }
                                        
                                        uint16_t segment_length = (((int)buffer.peek(segment_pos + 2)) << 8) + buffer.peek(segment_pos + 3);

                //LOG("Segment  0x%02x %d!\n", segment_marker, segment_length);
                                        segment_pos += 2 + segment_length;
                                        if (segment_pos >= buffer_size) 
                                        {
                                            // Truncated segment data
                                            return -2;
                                        }

                                        if (segment_marker == 0xDA) 
                                        {
                                            //afer  skipping SOS, search for FF D9
                                            //all FFs are followed with 0 in compressed

                                            while (segment_pos < buffer_size - 2) 
                                            {
                                                if (buffer.peek(segment_pos) == 0xFF && buffer.peek(segment_pos+1) == 0xD9) 
                                                {  
                                                    // EOI (End of Image)
                                                    jpeg_info->size = segment_pos + 2 - i;
                                                    return 0;  // Successfully found and parsed the JPEG
                                                } 
                                                segment_pos++;
                                            }
                                            return -2; //no EOF found
                                        }
                                    }
                                } 
                                else 
                                {
                                    return -2; //incorrect segment
                                }
                            }
                            break;
                        }
                    } 
                    else 
                    {
                        // Skip the current marker segment
                        if (pos + 3 >= buffer_size) 
                        {
                            // Truncated segment length field
                            return -2;
                        }
                        uint16_t segment_length = (buffer.peek(pos + 2) << 8) + buffer.peek(pos + 3);
                        pos += 2 + segment_length;
                        if (pos >= buffer_size) 
                        {
                            // Truncated segment data
                            return -2;
                        }
                    }
                } 
                else 
                {
                    //incorrect structure, marker expected
                    return -3;
                }
            }
        }
    }
    // If no valid JPEG is found
    return -1;
}
