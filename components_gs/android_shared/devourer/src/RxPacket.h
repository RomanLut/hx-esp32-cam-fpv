#ifndef DEVOURER_RX_PACKET_H
#define DEVOURER_RX_PACKET_H

#include <cstdint>
#include <span>

/* Parsed RX packet types, shared across chip generations. The per-generation
 * frame parsers (Jaguar1 FrameParser, Jaguar3 FrameParserJaguar3) both produce
 * `Packet`s; the USB adapter and demos consume them. Kept neutral so neither
 * generation depends on the other's parser header. */

enum class RX_PACKET_TYPE
{
    NORMAL_RX,  /* Normal rx packet */
    TX_REPORT1, /* CCX */
    TX_REPORT2, /* TX RPT */
    HIS_REPORT, /* USB HISR RPT */
    C2H_PACKET
};

struct rx_pkt_attrib
{
    uint16_t pkt_len;
    bool physt;
    uint8_t drvinfo_sz;
    uint8_t shift_sz;
    bool qos;
    uint8_t priority;
    bool mdata;
    uint16_t seq_num;
    uint8_t frag_num;
    bool mfrag;
    bool bdecrypted;
    uint8_t encrypt; /* when 0 indicate no encrypt. when non-zero, indicate the
                        encrypt algorith */
    bool crc_err;
    bool icv_err;
    /* TSF low (4 bytes at RX-descriptor offset 20) — chip-side timestamp
     * for the frame. With the seq_num just above it, a downstream layer
     * can drop duplicates by seq and measure one-way latency by diffing
     * the chip's TSF against its own wall clock. Populated by
     * FrameParser; surfaced through examples/rx/main.cpp's rx.frame event. */
    uint32_t tsfl;
    uint8_t data_rate;
    uint8_t bw;
    uint8_t stbc;
    uint8_t ldpc;
    uint8_t sgi;
    /* Descrambler seed the chip recovered from this frame's SERVICE field.
     * Trustworthy only on RTL8814AU (see FrameParser.cpp); surfaced for the
     * DEVOURER_DUMP_SCRAMBLER hook in examples/rx/main.cpp. */
    uint8_t scrambler;
    /* RSSI / SNR per RF path: A, B (Jaguar 8812/8811) plus C, D (8814AU). On
     * non-8814 chips the [2..3] slots are zero — the upstream RX phy-status
     * report reserves those bytes when only 2 paths are active. */
    uint8_t rssi[4];
    int8_t snr[4];
    /* Per-stream RX EVM (A,B on 8812/8811; plus C,D on 8814). Raw RX-status
     * bytes — a link-quality metric, NOT a per-subcarrier or content signal.
     * Surfaced for the DEVOURER_DUMP_BODY Tier-2 health diagnostic. */
    int8_t evm[4];
    /* Path-A CFO tail from the OFDM phy-status (signed HW units; kHz = raw *
     * 2.5, phydm CFO_HW_RPT_2_KHZ). The carrier-frequency offset between this
     * receiver's crystal and the transmitter's — the closed-loop CFO tracker's
     * input (see IRtlDevice::SetXtalCap). 0 when the phy-status carries none. */
    int8_t cfo_tail = 0;
    RX_PACKET_TYPE pkt_rpt_type;
};

struct Packet
{
    rx_pkt_attrib RxAtrib;
    std::span<uint8_t> Data;
};

#endif /* DEVOURER_RX_PACKET_H */
