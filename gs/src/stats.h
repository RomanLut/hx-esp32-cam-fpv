#pragma once
#include <stdint.h>
#include <string.h>

#define STATS_SIZE 60

class Stats
{
    public:
        uint8_t data[STATS_SIZE];
        int head = 0;
        uint32_t sum = 0;

    Stats()
    {
        memset( this->data, 0, STATS_SIZE );
    }

    void add( uint8_t value )
    {
        this->sum -= this->data[head];
        this->sum += value;

        this->data[head++] = value;
        if ( head == STATS_SIZE ) head = 0;
    }

    void addMultiple( uint8_t value, int count )
    {
        if ( count >= STATS_SIZE) count = STATS_SIZE;
        for ( int i = 0; i < count; i++ ) this->add(value);
    }

    int count()
    {
        return STATS_SIZE;
    }

    float average()
    {
        return this->sum / (float)STATS_SIZE;
    }

    uint8_t max()
    {
        uint8_t res = 0;
        for ( int i = 0; i < STATS_SIZE; i++ ) if ( this->data[i] > res) res = this->data[i];
        return res;
    }

    static float getter(void* data, int idx)
    {
        const Stats* inst = (const Stats*)data;
        idx += inst->head;
        idx = idx % STATS_SIZE;
        return inst->data[idx];
    }

};