/* ToneMask — RX-side per-subcarrier masking, shared across chip generations.
 *
 * Two receive-path mechanisms ported from vendor phydm (phydm_api.c):
 *
 *   CSI mask — a per-tone receive-equalizer de-weight table. Tells the BB "these
 *   subcarriers carry garbage" so a jammed slice stops dragging the whole-frame
 *   channel estimate / equalization down. This is the closest an 11ac chip gets
 *   to Wi-Fi 7 preamble puncturing, and only on the RX side: the vendor wrapper
 *   (phydm_csi_mask_setting) masks a handful of tones around one interferer
 *   frequency; here the low-level per-tone setter is looped so an entire 20 MHz
 *   slice of an 80 MHz channel can be de-weighted (DEVOURER_RX_CSI_MASK).
 *
 *   NBI notch — a narrowband-interference notch filter at one tone position
 *   (phydm_nbi_setting). Much coarser lever (one notch, LUT-quantized), kept
 *   for vendor parity as DEVOURER_RX_NBI.
 *
 * Neither touches the TX path — the silicon exposes no TX tone nulling, and a
 * VHT preamble must be contiguous full-band (true puncturing is 11be EHT-SIG
 * signaling). See docs/pseudo-preamble-puncturing.md.
 *
 * Register recipes transcribed from:
 *   11ac (Jaguar-1 8812/8811/8821/8814 + Jaguar-2 8822B):
 *     reference/rtl8812au/hal/phydm/phydm_api.c — phydm_csi_mask_enable
 *     (0x874[0]), phydm_set_csi_mask (bit arrays 0x880-0x88F positive /
 *     0x890-0x89F negative, one bit per 312.5 kHz subcarrier),
 *     phydm_set_nbi_reg (LUT -> 0x87C[19:14]), phydm_nbi_enable (0x87C[13];
 *     8822B additionally 0xC20[28]/0xE20[28]).
 *   Jaguar-3 (8822C/8822E):
 *     reference/rtl88x2cu/hal/phydm/phydm_api.c — the _jgr3 variants: CSI mask
 *     is an indexed table behind 0x1D94 (0x1EE8[1:0] write bracket, BYTE2 =
 *     table address = tone_idx/2, [7:0] = two 4-bit tone entries: BIT3 enable |
 *     3-bit weight), enable 0xC0C[3]; NBI tone index at 0x1944/0x4044[20:12]
 *     per path, enable 0x818[3] (inverted on 8822C/E) + 0x818[11] +
 *     0x1D3C[30:27] + 0x1940/0x4040[31].
 *
 * All tone math is in 312.5 kHz subcarrier units relative to the wide channel's
 * center frequency fc (phydm_find_fc port). The pure helpers (find_fc_mhz,
 * enumerate_tones, parse_*) have no device dependency so they can be unit-
 * checked headlessly.
 */
#ifndef DEVOURER_TONE_MASK_H
#define DEVOURER_TONE_MASK_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "RtlAdapter.h"
#include "SelectedChannel.h"
#include "logger.h"

namespace devourer {
namespace tonemask {

/* Which register recipe applies. AC1 = 8812/8811/8821 (NBI LUT always
 * 256-FFT); AC2 = 8814A / 8822B (NBI LUT 128-FFT at 20/40, 256-FFT at 80;
 * 8822B adds the 0xC20/0xE20 NBI enable bits). */
enum class Family { AC1, AC2_8814, AC2_8822B, JGR3 };

/* Vendor secondary-channel position (PHYDM_DONT_CARE/ABOVE/BELOW). */
enum SecCh : int { kSecDontCare = 0, kSecAbove = 1, kSecBelow = 2 };

/* HAL_PRIME_CHNL_OFFSET_* (SelectedChannel.ChannelOffset) -> vendor secondary
 * position: primary LOWER means the secondary (and fc) sit ABOVE the primary. */
inline int sec_from_prime_offset(uint8_t prime_offset) {
  if (prime_offset == 1) return kSecAbove; /* HAL_PRIME_CHNL_OFFSET_LOWER */
  if (prime_offset == 2) return kSecBelow; /* HAL_PRIME_CHNL_OFFSET_UPPER */
  return kSecDontCare;
}

inline uint32_t bw_mhz_from(ChannelWidth_t bw) {
  switch (bw) {
  case CHANNEL_WIDTH_40: return 40;
  case CHANNEL_WIDTH_80: return 80;
  default: return 20; /* 20 + the Jaguar-3 5/10 MHz re-clocks tune as 20 */
  }
}

/* phydm_find_fc port — wide-channel center frequency in MHz from the PRIMARY
 * 20 MHz channel + bandwidth (+ secondary position, 2.4 GHz 40 MHz only).
 * Returns 0 on an invalid combination. */
inline uint32_t find_fc_mhz(uint32_t channel, uint32_t bw, int sec_ch) {
  /* 2.4G */
  if (channel >= 1 && channel <= 14) {
    if (bw == 80) return 0;
    uint32_t fc = 2412 + (channel - 1) * 5;
    if (bw == 40 && sec_ch == kSecAbove) {
      if (channel >= 10) return 0;
      fc += 10;
    } else if (bw == 40 && sec_ch == kSecBelow) {
      if (channel <= 2) return 0;
      fc -= 10;
    }
    return fc;
  }
  /* 5G */
  if (channel >= 16 && channel <= 253) {
    static constexpr uint32_t start_40m[] = {20, 28, 36, 44, 52, 60, 68, 76,
                                             84, 92, 100, 108, 116, 124, 132,
                                             140, 149, 157, 165, 173, 181, 189,
                                             197, 205, 213, 221, 229, 237, 245};
    static constexpr uint32_t start_80m[] = {20, 36, 52, 68, 84, 100, 116,
                                             132, 149, 165, 181, 197, 213, 229};
    if (bw == 40 || bw == 80) {
      const uint32_t *start = (bw == 40) ? start_40m : start_80m;
      size_t n = (bw == 40) ? sizeof(start_40m) / sizeof(start_40m[0])
                            : sizeof(start_80m) / sizeof(start_80m[0]);
      uint32_t off = (bw == 40) ? 2 : 6; /* CH_OFFSET_40M / CH_OFFSET_80M */
      for (size_t i = 0; i + 1 < n; i++) {
        if (channel < start[i + 1]) {
          channel = start[i] + off;
          break;
        }
      }
    }
    return 5080 + (channel - 16) * 5;
  }
  return 0;
}

/* Signed subcarrier indices (312.5 kHz units, relative to fc) covering
 * [f_lo_khz, f_hi_khz], clipped to the RF bandwidth edge (bw_mhz * 1.6 =
 * bw/2 / 0.3125 subcarriers each side). */
inline std::vector<int> enumerate_tones(uint32_t fc_mhz, uint32_t bw_mhz,
                                        uint32_t f_lo_khz, uint32_t f_hi_khz) {
  std::vector<int> tones;
  if (!fc_mhz || f_hi_khz < f_lo_khz) return tones;
  int64_t fc_khz = int64_t(fc_mhz) * 1000;
  int k_edge = int(bw_mhz * 8 / 5); /* bw/2 / 0.3125 */
  /* k*312.5 kHz = k*625/2 — ceil/floor in integer math on 2*offset/625. */
  int64_t lo2 = (int64_t(f_lo_khz) - fc_khz) * 2;
  int64_t hi2 = (int64_t(f_hi_khz) - fc_khz) * 2;
  auto div_ceil = [](int64_t a, int64_t b) {
    return (a >= 0) ? (a + b - 1) / b : -((-a) / b);
  };
  auto div_floor = [](int64_t a, int64_t b) {
    return (a >= 0) ? a / b : -((-a + b - 1) / b);
  };
  int k_lo = int(div_ceil(lo2, 625));
  int k_hi = int(div_floor(hi2, 625));
  if (k_lo > k_hi && f_lo_khz == f_hi_khz) {
    /* Single-frequency spec — mask the nearest tone (vendor rounding). */
    int64_t k10 = (int64_t(f_lo_khz) - fc_khz) * 32 / 1000; /* x10 units */
    k_lo = k_hi = int((k10 + ((k10 >= 0) ? 5 : -5)) / 10);
  }
  for (int k = k_lo; k <= k_hi; k++) {
    if (k < -k_edge || k > k_edge) continue;
    tones.push_back(k);
  }
  return tones;
}

/* ---- env spec parsing ---------------------------------------------------- */

struct CsiMaskSpec {
  bool valid = false;
  uint32_t f_lo_khz = 0, f_hi_khz = 0;
  uint8_t wgt = 7; /* Jaguar-3 3-bit suppression weight (7 = strongest) */
};

/* "5210-5230" (MHz range) | "5220" (single frequency) with optional "/W"
 * Jaguar-3 weight suffix, e.g. "5210-5230/7". */
inline CsiMaskSpec parse_csi_spec(const char *s) {
  CsiMaskSpec spec;
  if (!s || !*s) return spec;
  char *end = nullptr;
  unsigned long lo = std::strtoul(s, &end, 10);
  if (end == s) return spec;
  unsigned long hi = lo;
  if (*end == '-') {
    const char *p = end + 1;
    hi = std::strtoul(p, &end, 10);
    if (end == p) return spec;
  }
  if (*end == '/') {
    const char *p = end + 1;
    unsigned long w = std::strtoul(p, &end, 10);
    if (end != p && w <= 7) spec.wgt = uint8_t(w);
  }
  if (*end != '\0' || hi < lo) return spec;
  spec.f_lo_khz = uint32_t(lo) * 1000;
  spec.f_hi_khz = uint32_t(hi) * 1000;
  spec.valid = true;
  return spec;
}

/* ---- 11ac (Jaguar-1 + Jaguar-2) ------------------------------------------ */

/* CSI-mask bit arrays: 0x880-0x88F = positive tones 0..127 (one bit per
 * subcarrier), 0x890-0x89F = negative (bit index 128 - |k|). Builds the full
 * 32-byte shadow host-side and writes 8 dwords, then sets the 0x874[0] enable.
 * Returns the number of tones masked. */
inline size_t csi_mask_apply_11ac(RtlAdapter &dev, uint32_t fc_mhz,
                                  uint32_t bw_mhz, const CsiMaskSpec &spec) {
  auto tones = enumerate_tones(fc_mhz, bw_mhz, spec.f_lo_khz, spec.f_hi_khz);
  uint8_t shadow[32] = {};
  for (int k : tones) {
    uint32_t idx;
    if (k >= 0) { /* FREQ_POSITIVE */
      idx = (uint32_t(k) >= 127) ? 127 : uint32_t(k);
    } else {
      uint32_t a = uint32_t(-k);
      idx = 128 + (128 - ((a >= 128) ? 128 : a));
      if (idx >= 256) idx = 255;
    }
    shadow[idx >> 3] |= uint8_t(1u << (idx & 7));
  }
  for (int i = 0; i < 8; i++) {
    uint32_t dw = uint32_t(shadow[i * 4]) | (uint32_t(shadow[i * 4 + 1]) << 8) |
                  (uint32_t(shadow[i * 4 + 2]) << 16) |
                  (uint32_t(shadow[i * 4 + 3]) << 24);
    dev.rtw_write32(uint16_t(0x880 + i * 4), dw);
  }
  dev.phy_set_bb_reg(0x874, 0x1, tones.empty() ? 0 : 1);
  return tones.size();
}

inline void csi_mask_clear_11ac(RtlAdapter &dev) {
  for (int i = 0; i < 8; i++)
    dev.rtw_write32(uint16_t(0x880 + i * 4), 0);
  dev.phy_set_bb_reg(0x874, 0x1, 0);
}

/* NBI notch, 11ac: quantize the tone offset (x10 fixed point, vendor units)
 * into the register LUT and write 0x87C[19:14] + the per-family enables.
 * f_intf must fall inside the tuned bandwidth. */
inline bool nbi_apply_11ac(RtlAdapter &dev, Family fam, uint32_t fc_mhz,
                           uint32_t bw_mhz, uint32_t f_intf_mhz,
                           bool paths_ge_2t) {
  /* tone_idx x10 units */
  static constexpr uint32_t nbi_128[] = {25, 55, 85, 115, 135, 155, 185, 205,
                                         225, 245, 265, 285, 305, 335, 355,
                                         375, 395, 415, 435, 455, 485, 505,
                                         525, 555, 585, 615, 635};
  static constexpr uint32_t nbi_256[] = {
      25,  55,  85,  115, 135, 155, 175, 195, 225,  245,  265,  285,
      305, 325, 345, 365, 385, 405, 425, 445, 465,  485,  505,  525,
      545, 565, 585, 605, 625, 645, 665, 695, 715,  735,  755,  775,
      795, 815, 835, 855, 875, 895, 915, 935, 955,  975,  995,  1015,
      1035, 1055, 1085, 1105, 1125, 1145, 1175, 1195, 1225, 1255, 1275};
  uint32_t bw_up = fc_mhz + bw_mhz / 2, bw_low = fc_mhz - bw_mhz / 2;
  if (f_intf_mhz < bw_low || f_intf_mhz > bw_up) return false;
  uint32_t dist = (f_intf_mhz > fc_mhz) ? (f_intf_mhz - fc_mhz)
                                        : (fc_mhz - f_intf_mhz);
  uint32_t idx10 = dist << 5; /* 10 * (dist / 0.3125) */

  bool use_256 = (fam == Family::AC1) || (bw_mhz == 80);
  const uint32_t *lut = use_256 ? nbi_256 : nbi_128;
  size_t n = use_256 ? sizeof(nbi_256) / sizeof(nbi_256[0])
                     : sizeof(nbi_128) / sizeof(nbi_128[0]);
  uint32_t reg_idx = 0;
  for (size_t i = 0; i < n; i++) {
    if (idx10 < lut[i]) {
      reg_idx = uint32_t(i) + 1;
      break;
    }
  }
  dev.phy_set_bb_reg(0x87c, 0xfc000, reg_idx);
  dev.phy_set_bb_reg(0x87c, 1u << 13, 1);
  if (fam == Family::AC2_8822B) {
    dev.phy_set_bb_reg(0xc20, 1u << 28, 1);
    if (paths_ge_2t) dev.phy_set_bb_reg(0xe20, 1u << 28, 1);
  }
  return true;
}

inline void nbi_disable_11ac(RtlAdapter &dev, Family fam,
                             bool paths_ge_2t) {
  dev.phy_set_bb_reg(0x87c, 1u << 13, 0);
  if (fam == Family::AC2_8822B) {
    dev.phy_set_bb_reg(0xc20, 1u << 28, 0);
    if (paths_ge_2t) dev.phy_set_bb_reg(0xe20, 1u << 28, 0);
  }
}

/* ---- Jaguar-3 (8822C / 8822E) -------------------------------------------- */

/* The CSI-mask table lives behind an indexed window: 0x1EE8[1:0]=3 opens it,
 * 0x1D94[31:30]=1 selects write mode, BYTE2 = table address (= tone_idx/2),
 * [7:0] = two 4-bit entries (low nibble even tone, high nibble odd tone),
 * each BIT3-enable | 3-bit weight. tone_num tracks the RF bandwidth via
 * 0x9B0[3:2] (RF80 -> 128, else 64). Adjacent tones share a table byte, so the
 * per-address byte is accumulated host-side first — the vendor's one-tone
 * setter would clobber its neighbour's nibble when looped over a range. */
inline size_t csi_mask_apply_jgr3(RtlAdapter &dev, uint32_t fc_mhz,
                                  uint32_t bw_mhz, const CsiMaskSpec &spec) {
  uint8_t rf_bw = dev.rtw_read8(0x9b0);
  uint32_t tone_num = (((rf_bw & 0xc) >> 2) == 0x2) ? 128 : 64;
  auto tones = enumerate_tones(fc_mhz, bw_mhz, spec.f_lo_khz, spec.f_hi_khz);

  uint8_t table[128] = {}; /* 256 tone entries / 2 per byte */
  uint8_t nib = uint8_t(0x8 | (spec.wgt & 0x7));
  for (int k : tones) {
    uint32_t idx;
    if (k >= 0) {
      idx = (uint32_t(k) >= tone_num - 1) ? (tone_num - 1) : uint32_t(k);
    } else {
      uint32_t a = uint32_t(-k);
      if (a >= tone_num) a = tone_num;
      idx = (tone_num << 1) - a;
    }
    if (idx >= 256) continue;
    table[idx >> 1] |= (idx & 1) ? uint8_t(nib << 4) : nib;
  }

  uint32_t idx_lmt = tone_num; /* full table for the current FFT size */
  dev.phy_set_bb_reg(0x1ee8, 0x3, 0x3);
  dev.phy_set_bb_reg(0x1d94, 0xc0000000, 0x1);
  for (uint32_t a = 0; a < idx_lmt; a++) {
    dev.phy_set_bb_reg(0x1d94, 0x00ff0000, a);
    dev.phy_set_bb_reg(0x1d94, 0x000000ff, table[a]);
  }
  dev.phy_set_bb_reg(0x1ee8, 0x3, 0x0);
  dev.phy_set_bb_reg(0xc0c, 1u << 3, tones.empty() ? 0 : 1);
  return tones.size();
}

inline void csi_mask_clear_jgr3(RtlAdapter &dev) {
  dev.phy_set_bb_reg(0x1ee8, 0x3, 0x3);
  dev.phy_set_bb_reg(0x1d94, 0xc0000000, 0x1);
  for (uint32_t a = 0; a < 128; a++) {
    dev.phy_set_bb_reg(0x1d94, 0x00ff0000, a);
    dev.phy_set_bb_reg(0x1d94, 0x000000ff, 0);
  }
  dev.phy_set_bb_reg(0x1ee8, 0x3, 0x0);
  dev.phy_set_bb_reg(0xc0c, 1u << 3, 0);
}

/* NBI notch, Jaguar-3: tone index (312.5 kHz units, kHz-exact per the x2cu
 * tree) into 0x1944/0x4044[20:12] per path; enable 0x818[3]=0 (inverted on
 * 8822C/E) + 0x818[11]=1 + 0x1D3C[30:27]=0xF + 0x1940/0x4040[31]. */
inline bool nbi_apply_jgr3(RtlAdapter &dev, uint32_t fc_mhz,
                           uint32_t bw_mhz, uint32_t f_intf_khz, int n_paths) {
  uint32_t fc_khz = fc_mhz * 1000;
  uint32_t bw_up = fc_khz + bw_mhz * 500, bw_low = fc_khz - bw_mhz * 500;
  if (f_intf_khz < bw_low || f_intf_khz > bw_up) return false;
  uint32_t dist = (f_intf_khz > fc_khz) ? (f_intf_khz - fc_khz)
                                        : (fc_khz - f_intf_khz);
  uint32_t idx = dist / 312;

  uint8_t rf_bw = dev.rtw_read8(0x9b0);
  uint32_t tone_num = (((rf_bw & 0xc) >> 2) == 0x2) ? 128 : 64;
  if (f_intf_khz >= fc_khz) { /* FREQ_POSITIVE */
    if (idx >= tone_num - 1) idx = tone_num - 1;
  } else {
    if (idx >= tone_num) idx = tone_num;
    idx = (tone_num << 1) - idx;
  }

  static constexpr uint16_t tone_reg[] = {0x1944, 0x4044};
  static constexpr uint16_t en_reg[] = {0x1940, 0x4040};
  if (n_paths > 2) n_paths = 2; /* 8822C/E are 2T2R */
  for (int p = 0; p < n_paths; p++)
    dev.phy_set_bb_reg(tone_reg[p], 0x001FF000, idx);

  dev.phy_set_bb_reg(0x818, 1u << 3, 0); /* !enable on 8822C/8822E */
  dev.phy_set_bb_reg(0x818, 1u << 11, 1);
  dev.phy_set_bb_reg(0x1d3c, 0x78000000, 0xf);
  for (int p = 0; p < n_paths; p++)
    dev.phy_set_bb_reg(en_reg[p], 1u << 31, 1);
  return true;
}

/* phydm_nbi_reset_jgr3 (8822C/E subset). */
inline void nbi_disable_jgr3(RtlAdapter &dev) {
  dev.phy_set_bb_reg(0x818, 1u << 3, 1);
  dev.phy_set_bb_reg(0x1d3c, 0x78000000, 0);
  dev.phy_set_bb_reg(0x818, 1u << 3, 0);
  dev.phy_set_bb_reg(0x818, 1u << 11, 0);
  dev.phy_set_bb_reg(0x1940, 1u << 31, 0);
  dev.phy_set_bb_reg(0x4040, 1u << 31, 0);
}

/* ---- spec-driven entry point ----------------------------------------------
 *
 * Applies whichever of the two specs is non-null (DeviceConfig rx.csi_mask /
 * rx.nbi), for the current channel state. Call after the channel set (like
 * rx.path_spec, the mask is the final word for a single-channel capture; a
 * later channel switch does not re-derive it). n_paths: RX chains to arm for
 * the Jaguar-3 NBI / the 8822B second-path enable. */
inline void apply(RtlAdapter &dev, Logger_t logger, Family fam,
                  const SelectedChannel &ch, int n_paths,
                  const char *csi_env, const char *nbi_env) {
  if (!csi_env && !nbi_env) return;

  uint32_t bw_mhz = bw_mhz_from(ch.ChannelWidth);
  int sec = sec_from_prime_offset(ch.ChannelOffset);
  uint32_t fc = find_fc_mhz(ch.Channel, bw_mhz, sec);
  if (!fc) {
    logger->error("ToneMask: no fc for ch={} bw={} sec={} — knobs ignored",
                  unsigned(ch.Channel), bw_mhz, sec);
    return;
  }

  if (csi_env) {
    CsiMaskSpec spec = parse_csi_spec(csi_env);
    if (!spec.valid) {
      logger->error("DEVOURER_RX_CSI_MASK — bad spec '{}' "
                    "(want <fstart>[-<fend>][/wgt], MHz)", csi_env);
    } else {
      size_t masked = (fam == Family::JGR3)
                          ? csi_mask_apply_jgr3(dev, fc, bw_mhz, spec)
                          : csi_mask_apply_11ac(dev, fc, bw_mhz, spec);
      if (fam == Family::JGR3) {
        logger->info("DEVOURER_RX_CSI_MASK: {} tones masked (fc={} MHz, "
                     "wgt={}), 0xc0c=0x{:08x}",
                     masked, fc, spec.wgt, dev.rtw_read32(0xc0c));
      } else {
        logger->info("DEVOURER_RX_CSI_MASK: {} tones masked (fc={} MHz), "
                     "0x874=0x{:08x} mask[0x880..0x89c]={:08x} {:08x} {:08x} "
                     "{:08x} {:08x} {:08x} {:08x} {:08x}",
                     masked, fc, dev.rtw_read32(0x874), dev.rtw_read32(0x880),
                     dev.rtw_read32(0x884), dev.rtw_read32(0x888),
                     dev.rtw_read32(0x88c), dev.rtw_read32(0x890),
                     dev.rtw_read32(0x894), dev.rtw_read32(0x898),
                     dev.rtw_read32(0x89c));
      }
      if (!masked)
        logger->warn("DEVOURER_RX_CSI_MASK: spec '{}' covers no tone inside "
                     "the tuned {} MHz channel — mask disabled", csi_env,
                     bw_mhz);
    }
  }

  if (nbi_env) {
    char *end = nullptr;
    unsigned long f_mhz = std::strtoul(nbi_env, &end, 10);
    if (end == nbi_env || *end != '\0') {
      logger->error("DEVOURER_RX_NBI — bad frequency '{}' (want MHz)", nbi_env);
    } else {
      bool ok = (fam == Family::JGR3)
                    ? nbi_apply_jgr3(dev, fc, bw_mhz, uint32_t(f_mhz) * 1000,
                                     n_paths)
                    : nbi_apply_11ac(dev, fam, fc, bw_mhz, uint32_t(f_mhz),
                                     n_paths >= 2);
      if (ok) {
        if (fam == Family::JGR3)
          logger->info("DEVOURER_RX_NBI: notch at {} MHz (fc={}), "
                       "0x818=0x{:08x} 0x1944=0x{:08x}",
                       f_mhz, fc, dev.rtw_read32(0x818),
                       dev.rtw_read32(0x1944));
        else
          logger->info("DEVOURER_RX_NBI: notch at {} MHz (fc={}), "
                       "0x87c=0x{:08x}",
                       f_mhz, fc, dev.rtw_read32(0x87c));
      } else {
        logger->warn("DEVOURER_RX_NBI: {} MHz outside the tuned {} MHz "
                     "channel (fc={}) — notch not armed", f_mhz, bw_mhz, fc);
      }
    }
  }
}

} /* namespace tonemask */
} /* namespace devourer */

#endif /* DEVOURER_TONE_MASK_H */
