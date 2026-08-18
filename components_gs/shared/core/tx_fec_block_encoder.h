#pragma once

#include <cstddef>
#include <cstdint>

#include "fec.h"
#include "packet_filter.h"

namespace gs::core
{

//===================================================================================
//===================================================================================
// Writes the transport Packet_Header of one outgoing packet in place.
//
// packet_data must point at a buffer of at least packet_header_offset +
// sizeof(Packet_Header) bytes; the header is written at packet_header_offset (i.e.
// after the radiotap + 802.11 headers) and the recorded payload size is everything
// after it.
void sealTransportPacket(PacketFilter& packet_filter,
                         uint8_t* packet_data,
                         size_t packet_size,
                         size_t packet_header_offset,
                         uint32_t block_index,
                         uint8_t packet_index);

//===================================================================================
//===================================================================================
// Owns the FEC encoder for one (k, n) coding pair and produces the parity payloads
// of a completed block.
//
// Buffer ownership stays with the caller: the Linux transport hands out pointers
// into pooled packets while the Android transport uses plain vectors, and only the
// payload pointers are shared vocabulary between them. This class exists so the
// coding-parameter handling and the fec_encode() call itself are written once.
class TxFecBlockEncoder
{
public:
    TxFecBlockEncoder() = default;
    ~TxFecBlockEncoder();

    TxFecBlockEncoder(const TxFecBlockEncoder&) = delete;
    TxFecBlockEncoder& operator=(const TxFecBlockEncoder&) = delete;

    // Rejects k == 0 and n < k; returns false without changing state in that case.
    bool init(uint32_t coding_k, uint32_t coding_n);
    void release();

    bool isReady() const
    {
        return m_fec != nullptr;
    }

    uint32_t codingK() const
    {
        return m_coding_k;
    }

    uint32_t codingN() const
    {
        return m_coding_n;
    }

    uint32_t parityCount() const
    {
        return m_coding_n - m_coding_k;
    }

    // src_payloads must hold codingK() pointers and dst_payloads parityCount()
    // pointers, each addressing payload_size bytes of packet payload (i.e. past the
    // transport Packet_Header).
    void encodeBlock(const uint8_t* const* src_payloads,
                     uint8_t* const* dst_payloads,
                     size_t payload_size) const;

private:
    fec_t* m_fec = nullptr;
    uint32_t m_coding_k = 0;
    uint32_t m_coding_n = 0;
};

}  // namespace gs::core
