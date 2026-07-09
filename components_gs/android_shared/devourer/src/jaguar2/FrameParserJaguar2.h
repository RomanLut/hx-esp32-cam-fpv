#ifndef FRAME_PARSER_8822B_H
#define FRAME_PARSER_8822B_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "basic_types.h"
#include "TxDescBits.h"
#include "RxPacket.h"

/* Jaguar2 (RTL8822B) TX/RX descriptor layout. 8822B is a HalMAC 88xx chip, so
 * the descriptor is the common 88xx layout — the same 48-byte TX / 24-byte RX
 * format as Jaguar3 (TX_DESC_SIZE_88XX / RX_DESC_SIZE_88XX both 48/24, verified
 * identical between the rtl88x2bu and rtl88x2cu halmac trees). Field
 * offsets/widths are the halmac_tx_desc_nic.h / halmac_rx_desc_nic.h positions;
 * the 16-bit descriptor checksum at 0x1C is verified by the USB MAC. Macros are
 * suffixed _8822B (distinct symbols from the Jaguar3 _8822C set). */

namespace jaguar2 {

constexpr size_t TXDESC_SIZE_8822B = 48; /* TX_DESC_SIZE_88XX */
constexpr size_t RXDESC_SIZE_8822B = 24; /* RX_DESC_SIZE_88XX */

} /* namespace jaguar2 */

/* --- TX descriptor fields --- */
#define SET_TX_DESC_TXPKTSIZE_8822B(d, v)  SET_BITS_TO_LE_4BYTE((d) + 0x00, 0, 16, v)
#define SET_TX_DESC_OFFSET_8822B(d, v)     SET_BITS_TO_LE_4BYTE((d) + 0x00, 16, 8, v)
#define SET_TX_DESC_BMC_8822B(d, v)        SET_BITS_TO_LE_4BYTE((d) + 0x00, 24, 1, v)
#define SET_TX_DESC_LS_8822B(d, v)         SET_BITS_TO_LE_4BYTE((d) + 0x00, 26, 1, v)
#define SET_TX_DESC_MACID_8822B(d, v)      SET_BITS_TO_LE_4BYTE((d) + 0x04, 0, 7, v)
#define SET_TX_DESC_QSEL_8822B(d, v)       SET_BITS_TO_LE_4BYTE((d) + 0x04, 8, 5, v)
#define SET_TX_DESC_RATE_ID_8822B(d, v)    SET_BITS_TO_LE_4BYTE((d) + 0x04, 16, 5, v)
#define SET_TX_DESC_PKT_OFFSET_8822B(d, v) SET_BITS_TO_LE_4BYTE((d) + 0x04, 24, 5, v)
#define SET_TX_DESC_USE_RATE_8822B(d, v)   SET_BITS_TO_LE_4BYTE((d) + 0x0C, 8, 1, v)
#define SET_TX_DESC_WHEADER_LEN_8822B(d, v) SET_BITS_TO_LE_4BYTE((d) + 0x0C, 0, 5, v)
#define SET_TX_DESC_CHK_EN_8822B(d, v)     SET_BITS_TO_LE_4BYTE((d) + 0x0C, 14, 1, v)
#define SET_TX_DESC_DISDATAFB_8822B(d, v)  SET_BITS_TO_LE_4BYTE((d) + 0x0C, 10, 1, v)
#define SET_TX_DESC_DATARATE_8822B(d, v)   SET_BITS_TO_LE_4BYTE((d) + 0x10, 0, 7, v)
#define SET_TX_DESC_DATA_SC_8822B(d, v)    SET_BITS_TO_LE_4BYTE((d) + 0x14, 0, 4, v)
#define SET_TX_DESC_DATA_SHORT_8822B(d, v) SET_BITS_TO_LE_4BYTE((d) + 0x14, 4, 1, v)
#define SET_TX_DESC_DATA_BW_8822B(d, v)    SET_BITS_TO_LE_4BYTE((d) + 0x14, 5, 2, v)
#define SET_TX_DESC_DATA_LDPC_8822B(d, v)  SET_BITS_TO_LE_4BYTE((d) + 0x14, 7, 1, v)
#define SET_TX_DESC_DATA_STBC_8822B(d, v)  SET_BITS_TO_LE_4BYTE((d) + 0x14, 8, 2, v)
/* Per-packet TX-power offset (halmac TXPWR_OFSET, txdesc+0x14[30:28]). A 3-bit
 * hardware LUT applied per-frame on top of the rate's TXAGC — the vendor's
 * dynamic-TX-power field (phydm dpt_lv): 0=none, 1=-3dB, 2=-7dB, 3=-11dB,
 * 4=+3dB, 5=+6dB (PHYDM_OFFSET_* in phydm_dynamictxpower.h). Zero-cost
 * per-packet power trim — a bitfield in the descriptor devourer already builds. */
#define SET_TX_DESC_TXPWR_OFSET_8822B(d, v) SET_BITS_TO_LE_4BYTE((d) + 0x14, 28, 3, v)
#define SET_TX_DESC_NAVUSEHDR_8822B(d, v)  SET_BITS_TO_LE_4BYTE((d) + 0x0C, 15, 1, v)
#define SET_TX_DESC_NDPA_8822B(d, v)       SET_BITS_TO_LE_4BYTE((d) + 0x0C, 22, 2, v)
#define SET_TX_DESC_DISQSELSEQ_8822B(d, v) SET_BITS_TO_LE_4BYTE((d) + 0x00, 31, 1, v)
#define SET_TX_DESC_G_ID_8822B(d, v)       SET_BITS_TO_LE_4BYTE((d) + 0x08, 24, 6, v)
#define SET_TX_DESC_RTY_LMT_EN_8822B(d, v) SET_BITS_TO_LE_4BYTE((d) + 0x10, 17, 1, v)
#define SET_TX_DESC_RTS_DATA_RTY_LMT_8822B(d, v) SET_BITS_TO_LE_4BYTE((d) + 0x10, 18, 6, v)
#define SET_TX_DESC_SW_DEFINE_8822B(d, v)  SET_BITS_TO_LE_4BYTE((d) + 0x18, 0, 12, v)
#define SET_TX_DESC_TXDESC_CHECKSUM_8822B(d, v) SET_BITS_TO_LE_4BYTE((d) + 0x1C, 0, 16, v)
#define SET_TX_DESC_EN_HWSEQ_8822B(d, v)   SET_BITS_TO_LE_4BYTE((d) + 0x20, 15, 1, v)
#define GET_TX_DESC_PKT_OFFSET_8822B(d)    LE_BITS_TO_4BYTE((d) + 0x04, 24, 5)

/* --- RX descriptor fields --- */
#define GET_RX_DESC_PKT_LEN_8822B(r)       LE_BITS_TO_4BYTE((r) + 0x00, 0, 14)
#define GET_RX_DESC_CRC32_8822B(r)         LE_BITS_TO_4BYTE((r) + 0x00, 14, 1)
#define GET_RX_DESC_ICV_ERR_8822B(r)       LE_BITS_TO_4BYTE((r) + 0x00, 15, 1)
#define GET_RX_DESC_DRV_INFO_SIZE_8822B(r) LE_BITS_TO_4BYTE((r) + 0x00, 16, 4)
#define GET_RX_DESC_SHIFT_8822B(r)         LE_BITS_TO_4BYTE((r) + 0x00, 24, 2)
#define GET_RX_DESC_PHYST_8822B(r)         LE_BITS_TO_4BYTE((r) + 0x00, 26, 1)
#define GET_RX_DESC_RX_RATE_8822B(r)       LE_BITS_TO_4BYTE((r) + 0x0C, 0, 7)
/* DWORD 5 = the 32-bit TSF-low latched at receive (same offset as the 8812's
 * GET_RX_STATUS_DESC_TSFL_8812 at +20). The hardware TSF timing reference. */
#define GET_RX_DESC_TSFL_8822B(r)          LE_BITS_TO_4BYTE((r) + 0x14, 0, 32)

namespace jaguar2 {

/* Port of fill_txdesc_check_sum_8822b (halmac_common_8822b.c): clear the 16-bit
 * checksum then XOR the first 16 LE words (dwords 0-7). CRITICAL: the 8822B HW
 * checksums ONLY the first 32 bytes ("HW calculates only 32byte" per the vendor,
 * fixed 8-pair loop) — NOT the full 48-byte descriptor. The earlier
 * (pkt_offset + TXDESC_SIZE/8)<<1 form (ported from the Jaguar3 path, which
 * covers 48 B) computed the checksum over dwords 0-11, so the HW's 32-byte
 * checksum never matched and the MAC silently DROPPED every TX frame at the
 * TXDMA — the MAC->BB TX-arm looked dead. Must be exactly 8 pairs / 32 bytes. */
inline void cal_txdesc_chksum_8822b(uint8_t *txdesc) {
  SET_TX_DESC_TXDESC_CHECKSUM_8822B(txdesc, 0);
  auto le16 = [&](size_t word) -> uint16_t {
    return static_cast<uint16_t>(txdesc[2 * word] |
                                 (txdesc[2 * word + 1] << 8));
  };
  uint16_t chksum = 0;
  for (uint16_t i = 0; i < 8; i++)
    chksum ^= static_cast<uint16_t>(le16(2 * i) ^ le16(2 * i + 1));
  SET_TX_DESC_TXDESC_CHECKSUM_8822B(txdesc, chksum);
}

/* Fill an 8822B data/monitor-inject TX descriptor (48 bytes, zeroed by caller)
 * and finalise its checksum. Mirrors the Jaguar3 inject field choices. */
/* Quantize a requested per-packet power delta (dB) to the nearest hardware
 * TXPWR_OFSET LUT step. The field is discrete — {0, -3, -7, -11, +3, +6} dB —
 * so an adaptive link's continuous request lands on the closest rung. Pure;
 * unit-tested in tests/txpkt_pwr_selftest.cpp. */
inline uint8_t txpkt_pwr_step_for_db(int db) {
  static const struct { int db; uint8_t step; } lut[] = {
      {0, 0}, {-3, 1}, {-7, 2}, {-11, 3}, {3, 4}, {6, 5}};
  uint8_t best = 0;
  int best_err = 1 << 30;
  for (const auto &e : lut) {
    int err = db > e.db ? db - e.db : e.db - db;
    if (err < best_err) {
      best_err = err;
      best = e.step;
    }
  }
  return best;
}

/* Inverse: the nominal dB of a LUT step (for readback / logging). */
inline int txpkt_pwr_db_for_step(uint8_t step) {
  static const int db[] = {0, -3, -7, -11, 3, 6};
  return step < 6 ? db[step] : 0;
}

inline void fill_data_tx_desc_8822b(uint8_t *d, uint16_t pkt_size,
                                    uint8_t rate_hw, uint8_t rate_id, uint8_t bw,
                                    bool short_gi, bool ldpc, uint8_t stbc,
                                    bool bmc = false, uint8_t wheader_len = 12,
                                    bool ndpa = false, uint8_t data_sc = 0,
                                    uint8_t pwr_ofset = 0) {
  SET_TX_DESC_TXPKTSIZE_8822B(d, pkt_size);
  SET_TX_DESC_OFFSET_8822B(d, static_cast<uint32_t>(TXDESC_SIZE_8822B));
  SET_TX_DESC_LS_8822B(d, 1);
  SET_TX_DESC_BMC_8822B(d, bmc ? 1 : 0);
  SET_TX_DESC_DISQSELSEQ_8822B(d, 1);
  SET_TX_DESC_G_ID_8822B(d, 0x3f);
  SET_TX_DESC_RTY_LMT_EN_8822B(d, 1);
  SET_TX_DESC_RTS_DATA_RTY_LMT_8822B(d, 12);
  SET_TX_DESC_MACID_8822B(d, 0x01);
  SET_TX_DESC_QSEL_8822B(d, 0x12);
  SET_TX_DESC_RATE_ID_8822B(d, 9);
  SET_TX_DESC_USE_RATE_8822B(d, 1);
  SET_TX_DESC_DISDATAFB_8822B(d, 0);
  SET_TX_DESC_SW_DEFINE_8822B(d, 1);
  SET_TX_DESC_DATARATE_8822B(d, rate_hw);
  SET_TX_DESC_DATA_BW_8822B(d, bw);
  /* Sub-channel: which 20/40 MHz slice a narrower-than-tuned frame occupies
   * (VHT_DATA_SC_*, rtl8821c_sc_mapping). 0 = DONT_CARE (frame BW == channel
   * BW, the common case). Non-zero places e.g. a 40-in-80 frame on the primary
   * lower-40 instead of the block centre. */
  SET_TX_DESC_DATA_SC_8822B(d, data_sc);
  /* Per-packet TX-power offset (0 = none, byte-identical to the prior desc).
   * Inside the 32-byte checksummed region (0x14), so set before the checksum. */
  if (pwr_ofset)
    SET_TX_DESC_TXPWR_OFSET_8822B(d, pwr_ofset & 0x7);
  SET_TX_DESC_DATA_SHORT_8822B(d, short_gi ? 1 : 0);
  SET_TX_DESC_DATA_LDPC_8822B(d, ldpc ? 1 : 0);
  SET_TX_DESC_DATA_STBC_8822B(d, stbc & 0x3);
  SET_TX_DESC_EN_HWSEQ_8822B(d, 1);
  /* WHEADER_LEN / CHK_EN are LEFT UNSET to match the kernel rtl88x2bu monitor-
   * inject descriptor exactly (usbmon ground truth: dword3 = 0x00000100, i.e.
   * USE_RATE only). Setting WHEADER_LEN made the 8822B MAC apply header-based
   * (security/QoS) processing to a raw-injected frame and DROP it at the TXDMA
   * check (DROP_DATA_EN) — BB TX-enable never keyed. The 16-bit descriptor
   * checksum at 0x1C is still computed (the kernel fills it even with CHK_EN=0).
   * `wheader_len` is retained in the signature for callers but not written. */
  (void)wheader_len;
  /* Beamforming self-sounding: mark the frame as an NDPA (halmac NDPA field,
   * dword3 [23:22] = 1) so the armed MAC sounding engine follows it with a
   * hardware-generated NDP — same recipe as the Jaguar-1/-3 paths: unicast
   * control frame, so no HW sequence stamp, not broadcast, use-header NAV, no
   * rate fallback. Must stay ABOVE the checksum: dword3 is inside the 32 bytes
   * the 8822B HW checksums, and a mismatch silently drops the frame at TXDMA. */
  if (ndpa) {
    SET_TX_DESC_NDPA_8822B(d, 1);
    SET_TX_DESC_EN_HWSEQ_8822B(d, 0);
    SET_TX_DESC_BMC_8822B(d, 0);
    SET_TX_DESC_NAVUSEHDR_8822B(d, 1);
    SET_TX_DESC_DISDATAFB_8822B(d, 1);
  }
  cal_txdesc_chksum_8822b(d);
}

struct Rx8822bFrame {
  const uint8_t *frame;
  uint32_t frame_len;
  bool crc_err;
  bool icv_err;
  uint8_t rx_rate;
  uint16_t drvinfo_size;
  uint8_t shift;
  uint32_t tsfl;              /* hardware TSF-low at receive */
  uint32_t next_offset;
};

/* Decode the leading RX descriptor of `buf` and locate its PSDU (24-byte desc +
 * drvinfo + shift). Returns false on a truncated/malformed buffer. */
inline bool parse_rx_8822b(const uint8_t *buf, size_t buflen,
                           Rx8822bFrame &out) {
  if (buf == nullptr || buflen < RXDESC_SIZE_8822B)
    return false;

  out.frame_len = static_cast<uint32_t>(GET_RX_DESC_PKT_LEN_8822B(buf));
  out.drvinfo_size =
      static_cast<uint16_t>(GET_RX_DESC_DRV_INFO_SIZE_8822B(buf) * 8);
  out.shift = static_cast<uint8_t>(GET_RX_DESC_SHIFT_8822B(buf));
  out.crc_err = GET_RX_DESC_CRC32_8822B(buf) != 0;
  out.icv_err = GET_RX_DESC_ICV_ERR_8822B(buf) != 0;
  out.rx_rate = static_cast<uint8_t>(GET_RX_DESC_RX_RATE_8822B(buf));
  out.tsfl = static_cast<uint32_t>(GET_RX_DESC_TSFL_8822B(buf));

  uint32_t frame_off =
      static_cast<uint32_t>(RXDESC_SIZE_8822B) + out.drvinfo_size + out.shift;
  uint64_t end = static_cast<uint64_t>(frame_off) + out.frame_len;
  if (out.frame_len == 0 || end > buflen)
    return false;

  out.frame = buf + frame_off;
  out.next_offset = (static_cast<uint32_t>(end) + 7) & ~0x7u;
  return true;
}

/* Parse the jaguar-series (jgr2) PHY-status report that precedes the PSDU when
 * APP_PHYSTS is enabled (8821C monitor). `physts` points at the 32-byte block
 * (RXDESC end); `is_cck` selects type0 (CCK) vs type1 (OFDM/HT/VHT). Fills the
 * per-path signal metrics from the vendor phy_sts_rpt_jgr2_type0/type1 layout
 * (phydm_phystatus.h): OFDM per-path RX power pwdb[i] (dBm = pwdb-110), per-path
 * SNR rxsnr[i] and per-stream EVM rxevm[i] (both s(8,1), i.e. half-dB units, as
 * the vendor stores them). Values are the raw phy-status fields, matching the
 * Jaguar-1 FrameParser convention (rssi = per-path power byte, dBm = value-110).
 * CCK (type0) reports a single path-A pwdb. Requires physts_len >= 28. */
inline void parse_phy_sts_jgr2(const uint8_t *physts, uint16_t physts_len,
                               bool is_cck, rx_pkt_attrib &a) {
  if (physts == nullptr || physts_len < 28)
    return;
  if (is_cck) {
    /* type0: DW0 = page_num(0), pwdb(1), ... */
    a.rssi[0] = physts[1];
  } else {
    /* type1: DW0/1 pwdb[4] at bytes 1..4; DW4 rxevm[4] at bytes 16..19;
     * DW5 cfo_tail[4] at bytes 20..23; DW6 rxsnr[4] at bytes 24..27. */
    for (int i = 0; i < 4; i++) {
      a.rssi[i] = physts[1 + i];
      a.evm[i] = static_cast<int8_t>(physts[16 + i]);
      a.snr[i] = static_cast<int8_t>(physts[24 + i]);
    }
    a.cfo_tail = static_cast<int8_t>(physts[20]); /* path-A CFO tail (#217) */
    /* DW1 byte7 flags: [5]=ldpc [6]=stbc [7]=beamformed (phy_sts_rpt_jgr2_type1).
     * DW1 byte5: l_rxsc[3:0] / ht_rxsc[7:4]; the vendor derives RX bandwidth from
     * the active rxsc (legacy OFDM uses l_rxsc, HT/VHT uses ht_rxsc): rxsc 1-8 =
     * 20, 9-12 = 40, >=13 = 80 MHz (phydm_get_phy_sts_type1). */
    const uint8_t f7 = physts[7];
    a.ldpc = (f7 >> 5) & 1;
    a.stbc = (f7 >> 6) & 1;
    const uint8_t l_rxsc = physts[5] & 0x0f, ht_rxsc = (physts[5] >> 4) & 0x0f;
    const uint8_t rxsc = (a.data_rate >= 4 && a.data_rate <= 11) ? l_rxsc : ht_rxsc;
    a.bw = rxsc >= 13 ? 2 : rxsc >= 9 ? 1 : 0;
  }
}

} /* namespace jaguar2 */

#endif /* FRAME_PARSER_8822B_H */
