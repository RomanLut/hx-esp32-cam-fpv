#include "vcd_profiler.h"

#ifdef ENABLE_PROFILER

#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "unistd.h"

#ifndef PF0_NAME
#define PF0_NAME ""
#endif

#ifndef PF1_NAME
#define PF1_NAME ""
#endif

#ifndef PF2_NAME
#define PF2_NAME ""
#endif

#ifndef PF3_NAME
#define PF3_NAME ""
#endif

#ifndef PF4_NAME
#define PF4_NAME ""
#endif

#ifndef PF5_NAME
#define PF5_NAME ""
#endif

#ifndef PF6_NAME
#define PF6_NAME ""
#endif

#ifndef PF7_NAME
#define PF7_NAME ""
#endif

#ifndef PF8_NAME
#define PF8_NAME ""
#endif

#ifndef PF9_NAME
#define PF9_NAME ""
#endif

#ifndef PF10_NAME
#define PF10_NAME ""
#endif

#ifndef PF11_NAME
#define PF11_NAME ""
#endif

#ifndef PF12_NAME
#define PF12_NAME ""
#endif

#ifndef PF13_NAME
#define PF13_NAME ""
#endif

#ifndef PF14_NAME
#define PF14_NAME ""
#endif

#ifndef PF15_NAME
#define PF15_NAME ""
#endif


VCDProfiler s_profiler;

//===========================================================
//===========================================================
VCDProfiler::VCDProfiler()
{
    this->profiler_mux = xSemaphoreCreateBinary();
    xSemaphoreGive(this->profiler_mux);
    this->count = 0;
    this->active = false;
    this->regFlags = 0;
}

//===========================================================
//===========================================================
bool VCDProfiler::init()
{
    this->buffer = (Descriptor*)heap_caps_malloc(VCDProfiler::BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    return this->buffer != nullptr;
}

//===========================================================
//===========================================================
IRAM_ATTR void VCDProfiler::set(int var, int val)
{
    if ( this->active )
    {
        xSemaphoreTake(this->profiler_mux,portMAX_DELAY);

        if ( this->count < VCDProfiler::MAX_SAMPLES )
        {
            int64_t t = esp_timer_get_time();
            t -= this->startTime;
            t >>= 4;
            if ( t <= this->duration )
            {
                Descriptor d;
                d.timestamp = t;
                d.var = var;
                //d.core = esp_cpu_get_core_id();
                d.value = val;
                this->buffer[this->count++] = d;
            }

            uint16_t mask = 1 << var;
            if ( val > 1)
            {
                this->regFlags |= mask;
            }

            if ( val > 0 )
            {
                this->lastVal |=  mask;
            }
            else
            {
                this->lastVal &=  ~mask;
            }
        }

        xSemaphoreGive(this->profiler_mux);
    }
}

//===========================================================
//===========================================================
IRAM_ATTR void VCDProfiler::toggle(int var)
{
    if ( this->active )
    {
        xSemaphoreTake(this->profiler_mux,portMAX_DELAY);

        if ( this->count < VCDProfiler::MAX_SAMPLES )
        {
            int64_t t = esp_timer_get_time();
            t -= this->startTime;
            t >>= 4;
            int16_t mask = 1 << var;
            if ( t <= this->duration )
            {
                Descriptor d;
                d.timestamp = t;
                d.var = var;
                //d.core = esp_cpu_get_core_id();
                d.value = (this->lastVal & mask) == 0 ? 1 : 0;
                this->buffer[this->count++] = d;
            }

            this->lastVal ^= mask;
        }

        xSemaphoreGive(this->profiler_mux);
    }
}

//===========================================================
//===========================================================
void VCDProfiler::start(int32_t duration_ms)
{
    //this->stop();
    this->clear();
    xSemaphoreTake(this->profiler_mux,portMAX_DELAY);
    this->startTime = esp_timer_get_time();
    this->active = true;
    this->duration = (duration_ms * 1000) >> 4;
    xSemaphoreGive(this->profiler_mux);
}

//===========================================================
//===========================================================
void VCDProfiler::stop()
{
    xSemaphoreTake(this->profiler_mux,portMAX_DELAY);
    this->active = false;
    xSemaphoreGive(this->profiler_mux);
}

#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c"
#define BYTE_TO_BINARY(byte)  \
  ((byte) & 0x80 ? '1' : '0'), \
  ((byte) & 0x40 ? '1' : '0'), \
  ((byte) & 0x20 ? '1' : '0'), \
  ((byte) & 0x10 ? '1' : '0'), \
  ((byte) & 0x08 ? '1' : '0'), \
  ((byte) & 0x04 ? '1' : '0'), \
  ((byte) & 0x02 ? '1' : '0'), \
  ((byte) & 0x01 ? '1' : '0') 

//===========================================================
//===========================================================
void VCDProfiler::save()
{
    this->stop();

    xSemaphoreTake(this->profiler_mux,portMAX_DELAY);

    for (int ii = 0; ii < 10000; ii++)
    {
        char buffer[64];
        sprintf(buffer, "/sdcard/pf%03lu.vcd", (long unsigned int)ii);
        FILE* f = fopen(buffer, "rb");
        if (f)
        {
            fclose(f);
            continue;
        }
        
        f = fopen(buffer, "wb");
        if (f == nullptr)
        {
            ESP_LOGI("PF","ERROR: Unable to open file %s!\n",buffer);
            break;
        }

        const char* line = "$date Mon Jan 22 22:32:32 2024 $end\n$timescale 1us $end\n$scope module pf $end\n";
        fwrite(line, strlen(line), 1, f);

        const char* refChars = "!#$%&*()@~^=|{}?";
        const char* names[] = { PF0_NAME, PF1_NAME, PF2_NAME, PF3_NAME, PF4_NAME, PF5_NAME, PF6_NAME, PF7_NAME,
                                PF8_NAME, PF9_NAME, PF10_NAME, PF11_NAME, PF12_NAME, PF13_NAME, PF14_NAME, PF15_NAME,};

        for ( int i = 0; i < VCDProfiler::CHANNELS_COUNT; i++)
        {
            if ( *names[i] == 0 ) continue;
            if (this->regFlags & (1<<i))
            {
                sprintf(buffer, "$var reg 8 %c %s $end\n", refChars[i], names[i]);
            }
            else
            {
                sprintf(buffer, "$var wire 1 %c %s $end\n", refChars[i], names[i]);
            }
            fwrite(buffer, strlen(buffer), 1, f);
        }

        line = "$upscope $end\n$enddefinitions $end\n";
        fwrite(line, strlen(line), 1, f);

        Descriptor d;
        //events
        int64_t lastTime = -1;
        for ( int i=-VCDProfiler::CHANNELS_COUNT-1; i < this->count; i++)
        {
            if ( i < 0 )
            {
                d.var = -i-1;
                d.value = 0;
                d.timestamp = 0;
                //d.core = 0;
            }
            else
            {
                d = this->buffer[i];
            }


            int64_t t = ((int64_t)d.timestamp) << 4;
            if ( lastTime != t )
            {
                sprintf(buffer, "#%llu\n", t);
                fwrite(buffer, strlen(buffer), 1, f);
                lastTime = t;
            }
            if (this->regFlags & (1<<d.var))
            {
                sprintf(buffer, "b" BYTE_TO_BINARY_PATTERN " %c\n", BYTE_TO_BINARY(d.value), refChars[d.var]);
            }
            else
            {
                sprintf(buffer, "%c%c\n", d.value == 0 ? '0' : '1', refChars[d.var]);
            }
            fwrite(buffer, strlen(buffer), 1, f);
        }

        fflush(f);
        fsync(fileno(f));
        fclose(f);

        break;
    }

    xSemaphoreGive(this->profiler_mux);
}

//===========================================================
//===========================================================
void VCDProfiler::clear()
{
    this->stop();
    xSemaphoreTake(this->profiler_mux,portMAX_DELAY);
    this->count = 0;
    this->regFlags = 0;
    xSemaphoreGive(this->profiler_mux);
}

//===========================================================
//===========================================================
bool VCDProfiler::full()
{
    bool res;
    xSemaphoreTake(this->profiler_mux,portMAX_DELAY);
    res = this->count == VCDProfiler::MAX_SAMPLES;
    xSemaphoreGive(this->profiler_mux);
    return res;
}


//===========================================================
//===========================================================
bool VCDProfiler::timedOut()
{
    bool res = false;
    xSemaphoreTake(this->profiler_mux,portMAX_DELAY);
    if ( this->active )
    {
        int64_t t = esp_timer_get_time();
        t -= this->startTime;
        t >>= 4;
        res = t >= this->duration;
    }
    xSemaphoreGive(this->profiler_mux);
    return res;
}

//===========================================================
//===========================================================
IRAM_ATTR bool VCDProfiler::isActive()
{
    bool res;
    xSemaphoreTake(this->profiler_mux,portMAX_DELAY);
    res = this->active;
    xSemaphoreGive(this->profiler_mux);
    return res;
}

#endif