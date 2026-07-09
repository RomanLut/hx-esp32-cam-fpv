#ifndef FRAME_PARSER_8822C_H
#define FRAME_PARSER_8822C_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "basic_types.h" /* cpu_to_le32 / le16_to_cpu used by the LE macros */
#include "TxDescBits.h" /* SET_BITS_TO_LE_4BYTE / LE_BITS_TO_4BYTE */
#include "RxPacket.h"   /* Packet / rx_pkt_attrib / RX_PACKET_TYPE */

/* Jaguar3 (8822C/8812EU/8822EU) TX/RX descriptor layout.
 *
 * This is the Jaguar3 descriptor — distinct from the Jaguar1 SET_TX_DESC_*_8812
 * layout in src/FrameParser.h. Field offsets/widths transcribed verbatim from
 * Realtek halmac (halmac_tx_desc_nic.h / halmac_rx_desc_nic.h, addressed by the
 * 8822C macro layer halmac_tx_desc_chip.h). The TX descriptor is 48 bytes (vs
 * Jaguar1's 40) and carries a 16-bit descriptor checksum at offset 0x1C that the
 * USB MAC verifies — see cal_txdesc_chksum_8822c().
 *
 * Encoding uses the same SET_BITS_TO_LE_4BYTE / LE_BITS_TO_4BYTE helpers as the
 * Jaguar1 macros. Validated on RTL8812CU hardware. */

namespace jaguar3 {

constexpr size_t TXDESC_SIZE_8822C = 48; /* TX_DESC_SIZE_88XX */
constexpr size_t RXDESC_SIZE_8822C = 24; /* RX_DESC_SIZE_88XX */

} /* namespace jaguar3 */

/* --- TX descriptor fields (halmac SET_TX_DESC_*_8822C -> generic offsets) --- */
#define SET_TX_DESC_TXPKTSIZE_8822C(d, v)  SET_BITS_TO_LE_4BYTE((d) + 0x00, 0, 16, v)
#define SET_TX_DESC_OFFSET_8822C(d, v)     SET_BITS_TO_LE_4BYTE((d) + 0x00, 16, 8, v)
#define SET_TX_DESC_BMC_8822C(d, v)        SET_BITS_TO_LE_4BYTE((d) + 0x00, 24, 1, v)
#define SET_TX_DESC_LS_8822C(d, v)         SET_BITS_TO_LE_4BYTE((d) + 0x00, 26, 1, v)
#define SET_TX_DESC_MACID_8822C(d, v)      SET_BITS_TO_LE_4BYTE((d) + 0x04, 0, 7, v)
#define SET_TX_DESC_QSEL_8822C(d, v)       SET_BITS_TO_LE_4BYTE((d) + 0x04, 8, 5, v)
#define SET_TX_DESC_RATE_ID_8822C(d, v)    SET_BITS_TO_LE_4BYTE((d) + 0x04, 16, 5, v)
#define SET_TX_DESC_PKT_OFFSET_8822C(d, v) SET_BITS_TO_LE_4BYTE((d) + 0x04, 24, 5, v)
#define SET_TX_DESC_USE_RATE_8822C(d, v)   SET_BITS_TO_LE_4BYTE((d) + 0x0C, 8, 1, v)
#define SET_TX_DESC_DISDATAFB_8822C(d, v)  SET_BITS_TO_LE_4BYTE((d) + 0x0C, 10, 1, v)
#define SET_TX_DESC_NAVUSEHDR_8822C(d, v)  SET_BITS_TO_LE_4BYTE((d) + 0x0C, 15, 1, v)
#define SET_TX_DESC_NDPA_8822C(d, v)       SET_BITS_TO_LE_4BYTE((d) + 0x0C, 22, 2, v)
#define SET_TX_DESC_DATARATE_8822C(d, v)   SET_BITS_TO_LE_4BYTE((d) + 0x10, 0, 7, v)
#define SET_TX_DESC_DATA_SC_8822C(d, v)    SET_BITS_TO_LE_4BYTE((d) + 0x14, 0, 4, v)
#define SET_TX_DESC_DATA_SHORT_8822C(d, v) SET_BITS_TO_LE_4BYTE((d) + 0x14, 4, 1, v)
#define SET_TX_DESC_DATA_BW_8822C(d, v)    SET_BITS_TO_LE_4BYTE((d) + 0x14, 5, 2, v)
#define SET_TX_DESC_DATA_LDPC_8822C(d, v)  SET_BITS_TO_LE_4BYTE((d) + 0x14, 7, 1, v)
#define SET_TX_DESC_DATA_STBC_8822C(d, v)  SET_BITS_TO_LE_4BYTE((d) + 0x14, 8, 2, v)
#define SET_TX_DESC_DISQSELSEQ_8822C(d, v) SET_BITS_TO_LE_4BYTE((d) + 0x00, 31, 1, v)
#define SET_TX_DESC_G_ID_8822C(d, v)       SET_BITS_TO_LE_4BYTE((d) + 0x08, 24, 6, v)
#define SET_TX_DESC_RTY_LMT_EN_8822C(d, v) SET_BITS_TO_LE_4BYTE((d) + 0x10, 17, 1, v)
#define SET_TX_DESC_RTS_DATA_RTY_LMT_8822C(d, v) SET_BITS_TO_LE_4BYTE((d) + 0x10, 18, 6, v)
#define SET_TX_DESC_SW_DEFINE_8822C(d, v)  SET_BITS_TO_LE_4BYTE((d) + 0x18, 0, 12, v)
#define SET_TX_DESC_TXDESC_CHECKSUM_8822C(d, v) SET_BITS_TO_LE_4BYTE((d) + 0x1C, 0, 16, v)
#define SET_TX_DESC_EN_HWSEQ_8822C(d, v)   SET_BITS_TO_LE_4BYTE((d) + 0x20, 15, 1, v)
#define GET_TX_DESC_PKT_OFFSET_8822C(d)    LE_BITS_TO_4BYTE((d) + 0x04, 24, 5)

/* --- RX descriptor fields --- */
#define GET_RX_DESC_PKT_LEN_8822C(r)       LE_BITS_TO_4BYTE((r) + 0x00, 0, 14)
#define GET_RX_DESC_CRC32_8822C(r)         LE_BITS_TO_4BYTE((r) + 0x00, 14, 1)
#define GET_RX_DESC_ICV_ERR_8822C(r)       LE_BITS_TO_4BYTE((r) + 0x00, 15, 1)
#define GET_RX_DESC_DRV_INFO_SIZE_8822C(r) LE_BITS_TO_4BYTE((r) + 0x00, 16, 4)
#define GET_RX_DESC_SHIFT_8822C(r)         LE_BITS_TO_4BYTE((r) + 0x00, 24, 2)
#define GET_RX_DESC_PHYST_8822C(r)         LE_BITS_TO_4BYTE((r) + 0x00, 26, 1)
#define GET_RX_DESC_RX_RATE_8822C(r)       LE_BITS_TO_4BYTE((r) + 0x0C, 0, 7)
/* DWORD 5 = the 32-bit TSF-low latched at receive (same +0x14 offset as the
 * 8812 GET_RX_STATUS_DESC_TSFL_8812). The hardware TSF timing reference. */
#define GET_RX_DESC_TSFL_8822C(r)          LE_BITS_TO_4BYTE((r) + 0x14, 0, 32)

namespace jaguar3 {

/* Port of fill_txdesc_check_sum_8822c (halmac_common_8822c.c): clear the 16-bit
 * checksum field, then XOR all 16-bit little-endian words of the descriptor
 * (PKT_OFFSET + TXDESC_SIZE/8) << 1 word-pairs) and store the result. Read
 * byte-wise so the on-wire result is identical on any host endianness. */
inline void cal_txdesc_chksum_8822c(uint8_t *txdesc) {
  SET_TX_DESC_TXDESC_CHECKSUM_8822C(txdesc, 0);
  auto le16 = [&](size_t word) -> uint16_t {
    return static_cast<uint16_t>(txdesc[2 * word] |
                                 (txdesc[2 * word + 1] << 8));
  };
  uint16_t pkt_offset =
      static_cast<uint16_t>(GET_TX_DESC_PKT_OFFSET_8822C(txdesc));
  uint16_t pairs =
      static_cast<uint16_t>((pkt_offset + (TXDESC_SIZE_8822C >> 3)) << 1);
  uint16_t chksum = 0;
  for (uint16_t i = 0; i < pairs; i++)
    chksum ^= static_cast<uint16_t>(le16(2 * i) ^ le16(2 * i + 1));
  SET_TX_DESC_TXDESC_CHECKSUM_8822C(txdesc, chksum);
}

/* Fill an 8822C data/monitor-inject TX descriptor (48 bytes, zeroed by caller)
 * and finalise its checksum. `bw` is the descriptor BW code (0=20,1=40,2=80),
 * `rate_hw` the DESC_RATE* index (MRateToHwRate output), `rate_id` 8(HT)/9(VHT).
 * Field choices mirror the Jaguar1 monitor-inject path (MACID 1, USE_RATE,
 * DISDATAFB, HW sequence). */
inline void fill_data_tx_desc_8822c(uint8_t *d, uint16_t pkt_size,
                                    uint8_t rate_hw, uint8_t rate_id, uint8_t bw,
                                    bool short_gi, bool ldpc, uint8_t stbc,
                                    bool bmc = false, bool ndpa = false,
                                    uint8_t data_sc = 0) {
  SET_TX_DESC_TXPKTSIZE_8822C(d, pkt_size);
  SET_TX_DESC_OFFSET_8822C(d, static_cast<uint32_t>(TXDESC_SIZE_8822C));
  SET_TX_DESC_LS_8822C(d, 1);
  /* Broadcast/multicast marker: the kernel sets this whenever addr1 is a group
   * address, so mark group-addressed frames the same way for a faithful port. */
  SET_TX_DESC_BMC_8822C(d, bmc ? 1 : 0);
  /* Remaining fields mirror the kernel's 8822e data/inject descriptor so the
   * on-wire TXDESC is byte-identical (verified against a usbmon capture of the
   * kernel's MCS7 inject): hw-managed sequence (DISQSELSEQ), SU/broadcast group
   * (G_ID=63), a bounded per-frame retry limit (RTY_LMT_EN + RTS_DATA_RTY_LMT),
   * RA-group 9, and a non-zero SW_DEFINE. NB: these do not change on-air
   * throughput (the chip already airs a saturating MCS7 flood at ~77% duty with
   * or without them) — they are here for a faithful port, not a fix. */
  SET_TX_DESC_DISQSELSEQ_8822C(d, 1);
  SET_TX_DESC_G_ID_8822C(d, 0x3f);
  SET_TX_DESC_RTY_LMT_EN_8822C(d, 1);
  SET_TX_DESC_RTS_DATA_RTY_LMT_8822C(d, 12);
  SET_TX_DESC_MACID_8822C(d, 0x01);
  SET_TX_DESC_QSEL_8822C(d, 0x12); /* MGMT queue (mirrors Jaguar1 inject) */
  SET_TX_DESC_RATE_ID_8822C(d, 9); /* kernel uses RA-group 9 for the inject path */
  SET_TX_DESC_USE_RATE_8822C(d, 1);
  SET_TX_DESC_DISDATAFB_8822C(d, 0);
  SET_TX_DESC_SW_DEFINE_8822C(d, 1);
  SET_TX_DESC_DATARATE_8822C(d, rate_hw);
  SET_TX_DESC_DATA_BW_8822C(d, bw);
  /* Data sub-channel: which 20/40 MHz slice of a wider configured channel the
   * frame occupies (VHT_DATA_SC_*). 0 = DONT_CARE (frame BW == channel BW). Set
   * for a 40 MHz frame on an 80 MHz channel — the stall-free 40-in-80 path. */
  SET_TX_DESC_DATA_SC_8822C(d, data_sc);
  SET_TX_DESC_DATA_SHORT_8822C(d, short_gi ? 1 : 0);
  SET_TX_DESC_DATA_LDPC_8822C(d, ldpc ? 1 : 0);
  SET_TX_DESC_DATA_STBC_8822C(d, stbc & 0x3);
  SET_TX_DESC_EN_HWSEQ_8822C(d, 1);
  /* Beamforming self-sounding: mark the frame as an NDPA (halmac NDPA field,
   * dword3 [23:22] = 1) so the armed MAC sounding engine follows it with a
   * hardware-generated NDP — the Jaguar-3 mirror of the Jaguar-1
   * SET_TX_DESC_NDPA_8812 recipe: unicast control frame, so no HW sequence
   * stamp, not broadcast, use-header NAV, no rate fallback. */
  if (ndpa) {
    SET_TX_DESC_NDPA_8822C(d, 1);
    SET_TX_DESC_EN_HWSEQ_8822C(d, 0);
    SET_TX_DESC_BMC_8822C(d, 0);
    SET_TX_DESC_NAVUSEHDR_8822C(d, 1);
    SET_TX_DESC_DISDATAFB_8822C(d, 1);
  }
  cal_txdesc_chksum_8822c(d);
}

/* Parsed view of one 8822C RX frame within a (possibly aggregated) bulk-IN
 * buffer. `frame`/`frame_len` point at the 802.11 PSDU after the descriptor +
 * driver-info + shift padding. */
struct Rx8822cFrame {
  const uint8_t *frame;  /* 802.11 frame start (inside the input buffer) */
  uint32_t frame_len;    /* PKT_LEN */
  bool crc_err;
  bool icv_err;
  uint8_t rx_rate;       /* RX_RATE (DESC_RATE* index) */
  uint16_t drvinfo_size; /* bytes (DRV_INFO_SIZE * 8) */
  uint8_t shift;         /* SHIFT_SZ */
  uint32_t tsfl;         /* hardware TSF-low at receive */
  uint32_t next_offset;  /* 8-byte-aligned offset of the next frame in an agg */
};

/* Decode the leading RX descriptor of `buf` and locate its PSDU. Mirrors the
 * Jaguar1 rtl8812_query_rx_desc_status path (same 24-byte descriptor + drvinfo/
 * shift layout) with 8822C field positions. Returns false on a truncated /
 * malformed buffer. */
inline bool parse_rx_8822c(const uint8_t *buf, size_t buflen,
                           Rx8822cFrame &out) {
  if (buf == nullptr || buflen < RXDESC_SIZE_8822C)
    return false;

  out.frame_len = static_cast<uint32_t>(GET_RX_DESC_PKT_LEN_8822C(buf));
  out.drvinfo_size =
      static_cast<uint16_t>(GET_RX_DESC_DRV_INFO_SIZE_8822C(buf) * 8);
  out.shift = static_cast<uint8_t>(GET_RX_DESC_SHIFT_8822C(buf));
  out.crc_err = GET_RX_DESC_CRC32_8822C(buf) != 0;
  out.icv_err = GET_RX_DESC_ICV_ERR_8822C(buf) != 0;
  out.rx_rate = static_cast<uint8_t>(GET_RX_DESC_RX_RATE_8822C(buf));
  out.tsfl = static_cast<uint32_t>(GET_RX_DESC_TSFL_8822C(buf));

  uint32_t frame_off =
      static_cast<uint32_t>(RXDESC_SIZE_8822C) + out.drvinfo_size + out.shift;
  uint64_t end = static_cast<uint64_t>(frame_off) + out.frame_len;
  if (out.frame_len == 0 || end > buflen)
    return false;

  out.frame = buf + frame_off;
  /* next frame in an aggregate is 8-byte aligned */
  out.next_offset = (static_cast<uint32_t>(end) + 7) & ~0x7u;
  return true;
}

/* Parse the Jaguar-3 (phydm "jgr3") PHY-status report that precedes the PSDU
 * when APP_PHYSTS is enabled (monitor_rx_cfg sets RCR BIT28 + RX_DRVINFO_SZ=4,
 * so the 32-byte report is counted in the descriptor drvinfo). `physts` points
 * at the report (immediately after the 24-byte RX descriptor). Fills the
 * per-path signal metrics from the vendor phy_sts_rpt_jgr3_type0 (CCK) /
 * type1 (OFDM/HT/VHT) layout (reference rtl88x2{c,e}u hal/phydm/phydm_phystatus.h).
 *
 * The jgr3 type1 byte offsets are identical to jgr2 type1 (per-path pwdb[4] at
 * bytes 1..4, l_rxsc/ht_rxsc at byte 5, flag byte at 7, s(8,1) rxevm[4] at
 * bytes 16..19, s(8,1) rxsnr[4] at bytes 24..27); the shared OFDM header
 * (phy_sts_rpt_jgr3_ofdm_cmn) carries pwdb/rxsc/flags for every OFDM page.
 * Values are stored as the raw phy-status bytes, matching the Jaguar-1/Jaguar-2
 * convention (rssi = per-path power byte, dBm = value-110; snr/evm as the
 * vendor's s(8,1) fields). The page type is taken from byte0 low nibble
 * (page_num) rather than guessed from the rate: 0 = CCK type0, else an OFDM
 * page; per-stream EVM/SNR are only present on the type1 OFDM page.
 * Requires physts_len >= 28. */
inline void parse_phy_sts_jgr3(const uint8_t *physts, uint16_t physts_len,
                               rx_pkt_attrib &a) {
  if (physts == nullptr || physts_len < 28)
    return;
  const uint8_t page_num = physts[0] & 0x0f;
  if (page_num == 0) {
    /* type0 (CCK): DW0 = page_num(0), pwdb_a(1). Single path-A power. */
    a.rssi[0] = physts[1];
    return;
  }
  /* OFDM header (valid for every jgr3 OFDM page): per-path pwdb[4] at bytes
   * 1..4, DW1 byte5 l_rxsc[3:0]/ht_rxsc[7:4], DW1 byte7 flags. */
  for (int i = 0; i < 4; i++)
    a.rssi[i] = physts[1 + i];
  /* DW1 byte7: [5]=ldpc [6]=stbc [7]=beamformed (phy_sts_rpt_jgr3_ofdm_cmn). */
  const uint8_t f7 = physts[7];
  a.ldpc = (f7 >> 5) & 1;
  a.stbc = (f7 >> 6) & 1;
  /* RX bandwidth from the active rxsc: legacy OFDM uses l_rxsc, HT/VHT uses
   * ht_rxsc; rxsc 1-8 = 20, 9-12 = 40, >=13 = 80 MHz (phydm_rxsc_2_bw). */
  const uint8_t l_rxsc = physts[5] & 0x0f, ht_rxsc = (physts[5] >> 4) & 0x0f;
  const uint8_t rxsc = (a.data_rate >= 4 && a.data_rate <= 11) ? l_rxsc : ht_rxsc;
  a.bw = rxsc >= 13 ? 2 : rxsc >= 9 ? 1 : 0;
  /* Per-stream EVM/SNR + per-path CFO tail only exist on the type1 page
   * (phy_sts_rpt_jgr3_type1: DW4 rxevm[4] at 16..19, DW5 cfo_tail[4] at
   * 20..23, DW6 rxsnr[4] at 24..27); other OFDM pages (2/3/4/5/6) reuse those
   * offsets for other data, so leave them at 0 there. */
  if (page_num == 1) {
    a.cfo_tail = static_cast<int8_t>(physts[20]); /* path-A CFO tail */
    for (int i = 0; i < 4; i++) {
      a.evm[i] = static_cast<int8_t>(physts[16 + i]);
      a.snr[i] = static_cast<int8_t>(physts[24 + i]);
    }
  }
}

} /* namespace jaguar3 */

#endif /* FRAME_PARSER_8822C_H */
