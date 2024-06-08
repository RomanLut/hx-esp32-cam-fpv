#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "circular_buffer.h"

#define MAX_JPEG_LOOKAHEAD (300 * 1024)  // 300 Kb

typedef struct {
    uint32_t offset;      // Offset of JPEG start in the buffer
    uint32_t size;        // Size of the JPEG file
    uint32_t width;       // Width of the JPEG image
    uint32_t height;      // Height of the JPEG image
} JpegInfo;

int find_jpeg_in_buffer(Circular_Buffer& buffer, size_t buffer_size, JpegInfo *jpeg_info);

bool getJPEGDimensions(uint8_t* buffer, int& width, int& height, int maxSearchLength);
