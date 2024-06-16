#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern bool getJPEGDimensions(uint8_t* buffer, int& width, int& height, size_t maxSearchLength);
