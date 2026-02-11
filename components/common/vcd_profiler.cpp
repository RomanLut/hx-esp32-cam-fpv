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

#ifndef PF16_NAME
#define PF16_NAME ""
#endif

#ifndef PF17_NAME
#define PF17_NAME ""
#endif

#ifndef PF18_NAME
#define PF18_NAME ""
#endif

#ifndef PF19_NAME
#define PF19_NAME ""
#endif

#ifndef PF20_NAME
#define PF20_NAME ""
#endif

#ifndef PF21_NAME
#define PF21_NAME ""
#endif

#ifndef PF22_NAME
#define PF22_NAME ""
#endif

#ifndef PF23_NAME
#define PF23_NAME ""
#endif

#ifndef PF24_NAME
#define PF24_NAME ""
#endif

#ifndef PF25_NAME
#define PF25_NAME ""
#endif

#ifndef PF26_NAME
#define PF26_NAME ""
#endif

#ifndef PF27_NAME
#define PF27_NAME ""
#endif

#ifndef PF28_NAME
#define PF28_NAME ""
#endif

#ifndef PF29_NAME
#define PF29_NAME ""
#endif

#ifndef PF30_NAME
#define PF30_NAME ""
#endif

#ifndef PF31_NAME
#define PF31_NAME ""
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

            uint32_t mask = 1 << var;
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
IRAM_ATTR void VCDProfiler::toggle(int var, int64_t timestamp)
{
    if ( this->active )
    {
        if ( timestamp == -1 ) timestamp = esp_timer_get_time();

        xSemaphoreTake(this->profiler_mux,portMAX_DELAY);

        if ( this->count < VCDProfiler::MAX_SAMPLES )
        {
            timestamp -= this->startTime;
            timestamp >>= 4;
            uint32_t mask = 1 << var;
            if ( timestamp <= this->duration )
            {
                Descriptor d;
                d.timestamp = timestamp;
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
//===========================================================
void VCDProfiler::outputToConsole()
{
    this->stop();

    xSemaphoreTake(this->profiler_mux,portMAX_DELAY);

    printf("<<<VCDSTART>>>");
    int lineCount = 0;
    auto writeLine = [&lineCount](const char* str){ printf("%s", str); lineCount++; if (lineCount % 100 == 0) vTaskDelay(1); };
    this->printVCD(writeLine);
    printf("<<<VCDSTOP>>>");
    fflush(stdout);

    xSemaphoreGive(this->profiler_mux);
}

//===========================================================
//===========================================================
void VCDProfiler::start(long duration_ms)
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
void VCDProfiler::saveToSD()
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

        auto writeLine = [f](const char* str){ fwrite(str, strlen(str), 1, f); };
        this->printVCD(writeLine);

        fflush(f);
        fsync(fileno(f));
        fclose(f);

        break;
    }

    xSemaphoreGive(this->profiler_mux);
}

//===========================================================
//===========================================================
void VCDProfiler::printVCD(std::function<void(const char*)> writeLine)
{
    const char* line = "$date Mon Jan 22 22:32:32 2024 $end\n$timescale 1us $end\n$scope module pf $end\n";
    writeLine(line);

    const char* refChars = "!#$%&*()@~^=|{}?abcdefghijklmnop";
    const char* names[] = { PF0_NAME, PF1_NAME, PF2_NAME, PF3_NAME, PF4_NAME, PF5_NAME, PF6_NAME, PF7_NAME,
                            PF8_NAME, PF9_NAME, PF10_NAME, PF11_NAME, PF12_NAME, PF13_NAME, PF14_NAME, PF15_NAME,
                            PF16_NAME, PF17_NAME, PF18_NAME, PF19_NAME, PF20_NAME, PF21_NAME, PF22_NAME, PF23_NAME,
                            PF24_NAME, PF25_NAME, PF26_NAME, PF27_NAME, PF28_NAME, PF29_NAME, PF30_NAME, PF31_NAME,};
    char buffer[64];

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
        writeLine(buffer);
    }

    line = "$upscope $end\n$enddefinitions $end\n";
    writeLine(line);

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
            writeLine(buffer);
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
        writeLine(buffer);
    }
}


//===========================================================
//===========================================================
void VCDProfiler::save()
{
    this->stop();

#ifdef PROFILER_OUTPUT_TO_SD
    this->saveToSD();
#endif

#ifdef PROFILER_OUTPUT_TO_CONSOLE
    this->outputToConsole();
#endif
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
