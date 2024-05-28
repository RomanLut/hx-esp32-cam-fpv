#pragma once

#include <cassert>
#include <cstring>
#include <cstdint>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#else
#define IRAM_ATTR
#endif

class Circular_Buffer
{
public:
    Circular_Buffer(uint8_t* buffer, size_t size)
        : m_data(buffer)
        , m_capacity(size)
    {
    }
    
    IRAM_ATTR size_t size() const;
    IRAM_ATTR bool empty() const;
    IRAM_ATTR size_t get_space_left() const;
    IRAM_ATTR size_t capacity() const;
    IRAM_ATTR void resize(size_t size);
    IRAM_ATTR bool write(const void* data, size_t size);
    IRAM_ATTR bool read(void* dst, size_t size);
    IRAM_ATTR bool skip(size_t size);
    IRAM_ATTR const void* start_reading(size_t& size);
    IRAM_ATTR void end_reading(size_t size); //call with the same size as the one returned by start_reading
    IRAM_ATTR void clear();
    IRAM_ATTR uint8_t peek( size_t offset);

private:
    uint8_t* m_data;
    size_t m_capacity;
    size_t m_start = 0;
    size_t m_size = 0;
};
