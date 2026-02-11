#pragma once

#include "vcd_profiler_setup.h"

/*
===========================================================
Simple profiler
Ouputs VCD file to SD Card or Conslole

Define to enable profiler:
#define ENABLE_PROFILER

Define to output VCD to console:
#define PROFILER_OUTPUT_TO_CONSOLE

Define to output VCD to SD Card:
#define PROFILER_OUTPUT_TO_SD

Define handy signal names, up to 32 channels:
#define PF_CAMERA_DATA 0
#define PF0_NAME "cam_data"

Start profiling:
    s_profiler.start(1000);  //duration in ms

Toggle pin:
    s_profiler.toggle(PF_CAMERA_SD_OVF);

Add data (values in range 0...255):
    s_profiler.set(PF_CAMERA_DATA, 1);
    s_profiler.set(PF_CAMERA_DATA, 0);

    s_profiler.set(PF_CAMERA_WIFI_QUEUE, qs / 1024);
   
    s_profiler.toggle(PF_CAMERA_FEC_OVF);  //0->1, 1->0,, >1->0


Stop profiling and output resuls as VCD:
    s_profiler.stop();
    s_profiler.save();
    s_profiler.clear();
===========================================================
*/

//===========================================================
#ifdef ENABLE_PROFILER

#include <functional>
#include <cassert>
#include <cstring>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

//===========================================================
//===========================================================
class VCDProfiler
{
public:
    VCDProfiler();

    static const size_t BUFFER_SIZE = 500*1024;
    static const size_t MAX_SAMPLES = BUFFER_SIZE / 4;

    //19 * 65536*16 = ~19 seconds profiling max
    static const size_t MAX_TIMESTAMP_VALUE = 0x7ffff;

    static const size_t CHANNELS_COUNT = 32;

    struct Descriptor
    {
        uint32_t timestamp:19;  //16us unit
        //uint32_t core:1;
        uint32_t var:5; //max 32 channels
        uint32_t value:8; //values in range 0...255
    };

    bool init();

    void start(long duration_ms = (MAX_TIMESTAMP_VALUE << 4 / 1000) );
    void set(int var, int val);
    void toggle(int var, int64_t timestamp = -1);

    void stop();
    void save();
    void clear();

    bool full();
    bool timedOut();

    bool isActive();

private:

    bool active;
    int count;
    uint32_t regFlags;
    uint32_t lastVal;
    int64_t startTime;
    int32_t duration;

    SemaphoreHandle_t profiler_mux;
    Descriptor* buffer;

    void saveToSD();
    void outputToConsole();
    void printVCD(std::function<void(const char*)> writeLine);
};


extern VCDProfiler s_profiler;


#endif
