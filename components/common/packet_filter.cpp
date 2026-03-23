#include "packet_filter.h"

 //=============================================================================================
//=============================================================================================
void PacketFilter::set_packet_header_data( uint16_t from_device_id, uint16_t to_device_id )
{
    m_from_device_id.store(from_device_id, std::memory_order_release);
    m_to_device_id.store(to_device_id, std::memory_order_release);
}

 //=============================================================================================
//=============================================================================================
/*IRAM_ATTR*/ void PacketFilter::apply_packet_header_data( Packet_Header* packet )
{
    packet->packet_version = PACKET_VERSION;
    packet->packet_signature = PACKET_SIGNATURE;
    packet->fromDeviceId = this->m_from_device_id.load(std::memory_order_acquire);
    packet->toDeviceId = this->m_to_device_id.load(std::memory_order_acquire);
}

//=============================================================================================
//=============================================================================================
void PacketFilter::set_packet_filtering( uint16_t filter_from_device_id, uint16_t filter_to_device_id )
{
    m_filter_from_device_id.store(filter_from_device_id, std::memory_order_release);
    m_filter_to_device_id.store(filter_to_device_id, std::memory_order_release);
}

//=============================================================================================
//=============================================================================================
//data* points to Packet_header + mtu
//size is sizeof(Packet_Header) + sizeof(mtu)
/*IRAM_ATTR*/ PacketFilter::PacketFilterResult PacketFilter::filter_packet( const void* data, size_t size, size_t mtu )
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

    const uint16_t filter_from_device_id = this->m_filter_from_device_id.load(std::memory_order_acquire);
    const uint16_t filter_to_device_id = this->m_filter_to_device_id.load(std::memory_order_acquire);

    if ( ( filter_from_device_id !=0 ) && ( header->fromDeviceId != filter_from_device_id ) )
    {
        return PacketFilterResult::Drop;
    }

    if ( ( filter_to_device_id != 0 ) && ( header->toDeviceId != filter_to_device_id ) )
    {
        return PacketFilterResult::Drop;
    }

    if ( size < ( header->size + sizeof(Packet_Header) ) )
    {
        return PacketFilterResult::WrongStructure;
    }

    if ( header->size > mtu )
    {
        return PacketFilterResult::WrongStructure;
    }

    return PacketFilterResult::Pass;
}

