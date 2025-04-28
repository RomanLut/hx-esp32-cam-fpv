#include "packet_filter.h"

 //=============================================================================================
//=============================================================================================
void PacketFilter::set_packet_header_data( uint16_t from_device_id, uint16_t to_device_id )
{
    m_from_device_id = from_device_id;
    m_to_device_id = to_device_id;
}

 //=============================================================================================
//=============================================================================================
/*IRAM_ATTR*/ void PacketFilter::apply_packet_header_data( Packet_Header* packet )
{
    packet->packet_version = PACKET_VERSION;
    packet->packet_signature = PACKET_SIGNATURE;
    packet->fromDeviceId = this->m_from_device_id;
    packet->toDeviceId = this->m_to_device_id;
}

//=============================================================================================
//=============================================================================================
void PacketFilter::set_packet_filtering( uint16_t filter_from_device_id, uint16_t filter_to_device_id )
{
    m_filter_from_device_id = filter_from_device_id;
    m_filter_to_device_id = filter_to_device_id;
}

//=============================================================================================
//=============================================================================================
/*IRAM_ATTR*/ PacketFilter::PacketFilterResult PacketFilter::filter_packet( const void* data, size_t size )
{
    if ( size < sizeof(Packet_Header) )
    {
        return PacketFilterResult::WrongStructure;
    }

    const Packet_Header* header = reinterpret_cast<const Packet_Header*>(data);

    if ( header->packet_signature != PACKET_SIGNATURE ) 
    {
        return PacketFilterResult::WrongStructure;
    }

    if ( header->packet_version != PACKET_VERSION )
    {
        return PacketFilterResult::WrongVersion;
    }

    if ( ( this->m_filter_from_device_id !=0 ) && ( header->fromDeviceId != this->m_filter_from_device_id ) )
    {
        return PacketFilterResult::Drop;
    }

    if ( ( this->m_filter_to_device_id != 0 ) && ( header->toDeviceId != this->m_filter_to_device_id ) )
    {
        return PacketFilterResult::Drop;
    }

    return PacketFilterResult::Pass;
}

