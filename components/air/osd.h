#pragma once

#include <stdint.h>
#include "packets.h"
#include "main.h"


#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#else
#define IRAM_ATTR
#endif

class OSD
{
private:

    struct OSDBuffer buffer;
    bool changed;
    int lockCounter;  //lock osd buffer after clear() untill the next draw() to prevent sending incomplete screen

public:
    OSD();
    IRAM_ATTR void* getBuffer();
    IRAM_ATTR bool isChanged();
    IRAM_ATTR bool isLocked();  //do not request buffer while locked
    void clear();
    void commit();
    void writeString(unsigned int row, unsigned int col, int isExtChar, uint8_t* str, int len);
};

extern OSD g_osd;
