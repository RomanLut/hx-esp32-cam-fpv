#pragma once

#include <cstdint>
#include "fec.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#else
#define IRAM_ATTR
#endif

//===========================================================================================
//===========================================================================================
class PacketFilter
{
public:
    enum class PacketFilterResult
    {
        Pass,
        Drop,
        WrongStructure,
        WrongVersion
    };

    void set_packet_header_data( uint16_t from_device_id, uint16_t to_device_id );
    IRAM_ATTR void apply_packet_header_data( Packet_Header* packet );
    void set_packet_filtering( uint16_t filter_from_device_id, uint16_t filter_to_device_id );
    IRAM_ATTR PacketFilterResult filter_packet( const void* data, size_t size, size_t mtu ); 

private:

    //values are set on outgoing packets
    uint16_t m_from_device_id = 0;
    uint16_t m_to_device_id = 0;

    //values are used to filter incoming packets. 0 - no filtering
    uint16_t m_filter_from_device_id = 0;
    uint16_t m_filter_to_device_id = 0;
};
