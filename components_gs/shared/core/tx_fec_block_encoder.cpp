#include "tx_fec_block_encoder.h"

#include <cassert>

#include "Log.h"
#include "structures.h"

namespace gs::core
{

//===================================================================================
//===================================================================================
// Writes the transport Packet_Header of one outgoing packet in place.
void sealTransportPacket(PacketFilter& packet_filter,
                         uint8_t* packet_data,
                         size_t packet_size,
                         size_t packet_header_offset,
                         uint32_t block_index,
                         uint8_t packet_index)
{
    assert(packet_data != nullptr);
    assert(packet_size >= packet_header_offset + sizeof(Packet_Header));

    Packet_Header& header =
        *reinterpret_cast<Packet_Header*>(packet_data + packet_header_offset);

    packet_filter.apply_packet_header_data(&header);

    // Size of user data only, excluding the Packet_Header itself.
    header.size =
        static_cast<uint16_t>(packet_size - packet_header_offset - sizeof(Packet_Header));
    header.block_index = block_index;
    header.packet_index = packet_index;
}

//===================================================================================
//===================================================================================
// Releases the FEC encoder owned by this block encoder.
TxFecBlockEncoder::~TxFecBlockEncoder()
{
    release();
}

//===================================================================================
//===================================================================================
// Creates the FEC encoder for one (k, n) coding pair, replacing any previous one.
bool TxFecBlockEncoder::init(uint32_t coding_k, uint32_t coding_n)
{
    if (coding_k == 0 || coding_n < coding_k)
    {
        LOGE("Invalid TX coding params k={} n={}",
             static_cast<unsigned int>(coding_k),
             static_cast<unsigned int>(coding_n));
        return false;
    }

    release();

    m_fec = fec_new(static_cast<unsigned short>(coding_k), static_cast<unsigned short>(coding_n));
    if (m_fec == nullptr)
    {
        LOGE("Failed to create TX FEC encoder k={} n={}",
             static_cast<unsigned int>(coding_k),
             static_cast<unsigned int>(coding_n));
        return false;
    }

    m_coding_k = coding_k;
    m_coding_n = coding_n;
    return true;
}

//===================================================================================
//===================================================================================
// Frees the FEC encoder and clears the cached coding parameters.
void TxFecBlockEncoder::release()
{
    if (m_fec != nullptr)
    {
        fec_free(m_fec);
        m_fec = nullptr;
    }
    m_coding_k = 0;
    m_coding_n = 0;
}

//===================================================================================
//===================================================================================
// Produces the parity payloads for one completed block of codingK() source payloads.
void TxFecBlockEncoder::encodeBlock(const uint8_t* const* src_payloads,
                                    uint8_t* const* dst_payloads,
                                    size_t payload_size) const
{
    assert(isReady());
    assert(src_payloads != nullptr);
    assert(dst_payloads != nullptr || parityCount() == 0);

    if (m_fec == nullptr || parityCount() == 0)
    {
        return;
    }

    // fec_block_nums() + k selects the parity block numbers that follow the k source
    // blocks; this offset is what makes the produced blocks decodable as parity.
    fec_encode(m_fec,
               reinterpret_cast<const gf* const*>(src_payloads),
               reinterpret_cast<gf* const*>(dst_payloads),
               fec_block_nums() + m_coding_k,
               parityCount(),
               payload_size);
}

}  // namespace gs::core
