#pragma once

//===========================================================
//Uncomment to enable profiler
//#define PROFILE_CAMERA_DATA

//uncomment to start profiling with button
#define START_PROFILER_WITH_BUTTON

//uncommentto output VCD to console
#define PROFILER_OUTPUT_TO_CONSOLE
//===========================================================

//------------------------
#ifdef PROFILE_CAMERA_DATA

#define ENABLE_PROFILER

#define PF_CAMERA_DATA          0
#define PF_CAMERA_FRAME_QUALITY 1
#define PF_CAMERA_DATA_SIZE     2
#define PF_CAMERA_FEC_POOL      3
#define PF_CAMERA_FEC           4
#define PF_CAMERA_WIFI_TX       5
#define PF_CAMERA_WIFI_QUEUE    6
#define PF_CAMERA_SD_FAST_BUF   7
#define PF_CAMERA_SD_SLOW_BUF   8
#define PF_CAMERA_FEC_SPIN      9
#define PF_CAMERA_WIFI_SPIN     10
#define PF_CAMERA_WIFI_DONE_CB  11
#define PF_CAMERA_FEC_OVF       12
#define PF_CAMERA_WIFI_OVF      13
#define PF_CAMERA_OVF           14
#define PF_CAMERA_SD_OVF        15

#define PF0_NAME "cam_data"
#define PF1_NAME "quality"
#define PF2_NAME "data_size"
#define PF3_NAME "fec_pool"
#define PF4_NAME "fec"
#define PF5_NAME "wifi_tx"
#define PF6_NAME "wifi_queue"
#define PF7_NAME "sd_fast_buf"
#define PF8_NAME "sd_slow_buf"
#define PF9_NAME "fec_spin"
#define PF10_NAME "wifi_spin"
#define PF11_NAME "wifi_done_cb"
#define PF12_NAME "fec_ovf"
#define PF13_NAME "wifi_ovf"
#define PF14_NAME "cam_ovf"
#define PF15_NAME "sd_ovf"


#endif

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

    //16 * 65536*16 = ~16 seconds profiling max
    static const size_t MAX_TIMESTAMP_VALUE = 0xfffff;

    static const size_t CHANNELS_COUNT = 16;

    struct Descriptor
    {
        uint32_t timestamp:20;  //16us unit
        //uint32_t core:1;
        uint32_t var:4;
        uint32_t value:8;
    };

    bool init();

    void start(long duration_ms = (MAX_TIMESTAMP_VALUE << 4 / 1000) );
    void set(int var, int val);
    void toggle(int var);

    void stop();
    void save();
    void clear();

    bool full();
    bool timedOut();

    bool isActive();

private:

    bool active;
    int count;
    uint16_t regFlags;
    uint16_t lastVal;
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
