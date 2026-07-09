#ifndef HALRF_8821C_H
#define HALRF_8821C_H

#include <cstdint>

#include "logger.h"
#include "RtlAdapter.h"
#include "Jaguar2Calibration.h"

namespace jaguar2 {

/* Halrf8821c — RTL8821C (1T1R) halrf IQ calibration, ported verbatim from
 * reference/8821cu/hal/phydm/halrf/rtl8821c/halrf_iqk_8821c.c (core IQK only —
 * the DPK / TXGAPK / IMR-test / FW-IQK paths are out of scope).
 *
 * Same algorithm family as the 8822B port (Halrf8822b): LOK/TXK/RXK per path
 * via the 0x1b00 nctl IQK-engine bank, backing up and restoring MAC/BB/RF
 * around it. The 8821C is single-stream, so every per-path loop runs once on
 * RF_PATH_A only; the S1 path-B branches are dropped.
 *
 * 8821C-specific divergences from the 8822B flow that are preserved here:
 *   - LOK sweeps 8 PAD-index stages (RF_0x33[2:0] = i, i = 0..7).
 *   - The BTG (Bluetooth-shared) vs WLG/WLA front-end split (is_btg, read from
 *     BB 0xcb8[16]) selects different RF register recipes throughout.
 *   - RF read = direct BB shadow window (0x2800 + addr<<2); RF write = 3-wire
 *     LSSI (0xC90). BB access via phy_set_bb_reg / rtw_read32.  These primitives
 *     are chip-shared with the 8822B (same Jaguar2 3-wire/LSSI).
 *
 * Non-segment, non-reload, 20 MHz monitor bring-up: a single full LOK/TXK/RXK
 * pass with retries capped exactly as the vendor caps them. */
class Halrf8821c : public Jaguar2Calibration {
public:
  Halrf8821c(RtlAdapter device, Logger_t logger, uint8_t cut, bool is_2t2r);

  /* phy_iq_calibrate_8821c entry (SW path). band2g: true=2.4G. */
  void iqk_trigger(bool band2g) override;

  /* Thermal TX-power tracking — see Jaguar2Calibration. 1T1R:
   * path A only (0xc94[6:1] TXAGC + 0xc1c[31:21] BB scale). */
  void set_pwr_track_ctx(uint8_t baseline, uint8_t channel) override;
  void pwr_track(int current_ofdm_index) override;

private:
  /* MIX_MODE split + register write, path A (get_mix_mode..._8822b family,
   * 8821C register layout). */
  void pwr_track_write(int swing, int current_ofdm_index);

  /* --- MASKDWORD/RF primitives (dm API shims; shared with 8822B) --- */
  uint32_t bb_get(uint16_t addr, uint32_t mask);
  void bb_set(uint16_t addr, uint32_t mask, uint32_t data);
  uint32_t r32(uint16_t addr) { return _device.rtw_read32(addr); }
  void w32(uint16_t addr, uint32_t v) { _device.rtw_write32(addr, v); }
  void w8(uint16_t addr, uint8_t v) { _device.rtw_write8(addr, v); }
  uint32_t rf_get(uint8_t path, uint32_t addr); /* direct-BB read, 20-bit */
  void rf_set(uint8_t path, uint32_t addr, uint32_t mask, uint32_t data);

  /* --- backup / restore (_iqk_backup_*, _iqk_restore_*) --- */
  void backup_mac_bb(uint32_t *mac_bk, uint32_t *bb_bk,
                     const uint32_t *mac_reg, const uint32_t *bb_reg);
  void backup_rf(uint32_t *rf_bk, const uint32_t *reg);
  void restore_mac_bb(const uint32_t *mac_bk, const uint32_t *bb_bk,
                      const uint32_t *mac_reg, const uint32_t *bb_reg);
  void restore_rf(const uint32_t *reg, const uint32_t *rf_bk);
  void agc_bnd_int();
  void bb_reset();
  void afe_setting(bool do_iqk);
  void rfe_setting(bool ext_pa_on);
  void rf_setting();
  void configure_macbb();

  /* --- per-K settings --- */
  void lok_setting(uint8_t path, uint8_t pad_index);
  void txk_setting(uint8_t path);
  void rxk1_setting(uint8_t path);
  void rxk2_setting(uint8_t path, bool is_gs);

  /* --- nctl (0x1b00 bank) indirect gnt access + measurement one-shots --- */
  uint32_t indirect_read(uint16_t reg);
  void indirect_write(uint16_t reg, uint32_t mask, uint32_t val);
  void set_gnt_wl_gnt_bt(bool before_k);
  bool check_cal(uint32_t iqk_cmd);            /* returns fail */
  bool check_nctl_done(uint8_t path, uint32_t iqk_cmd);
  bool lok_one_shot(uint8_t path, uint8_t pad_index);
  bool rxk_gsearch(uint8_t path, uint8_t step);
  bool one_shot(uint8_t path, uint8_t idx);
  bool rxiqk_by_step(uint8_t path);
  void iqk_by_path();
  void start_iqk();
  void iqk_init();
  void fill_iqk_report();

  RtlAdapter _device;
  Logger_t _logger;
  uint8_t _cut;
  bool _2t2r;
  bool _band2g = true; /* set per iqk_trigger */
  uint8_t _bw = 0;     /* 0=20MHz (monitor) */
  bool _is_btg = false;/* BB 0xcb8[16]; BTG shared-antenna front-end */

  /* IQK run state (dm_iqk_info subset; path-A only, fresh IQK, no reload) */
  int _iqk_step = 1;
  int _rxiqk_step = 1;
  int _kcount = 0;
  uint8_t _tmp1bcc = 0x12;
  uint8_t _lna_idx = 0;
  bool _isbnd = false;
  uint32_t _tmp_gntwl = 0;

  uint8_t _retry[3] = {};    /* [TXIQK|RXIQK1|RXIQK2] */
  uint8_t _gs_retry[2] = {}; /* [gs1|gs2] */
  bool _lok_fail = true;
  uint8_t _rxiqk_fail_code = 0;
  bool _iqk_fail_report[2] = {true, true}; /* [TX_IQK|RX_IQK] */
  uint16_t _rxiqk_agc[2] = {};
  uint32_t _iqc_matrix[2] = {0x20000000, 0x20000000}; /* inert (no reload) */

  /* --- thermal TX-power tracking state (path A only) --- */
  uint8_t _pt_baseline = 0xff; /* efuse 0xBA; 0xff = disabled */
  uint8_t _pt_channel = 0;
  uint8_t _pt_default_ofdm = 24; /* reverse-mapped 0xc1c[31:21]; 24 = 0 dB */
  int _pt_avg[4] = {};
  int _pt_avg_idx = 0;
  int _pt_last_swing = 0x7fffffff;
};

} /* namespace jaguar2 */

#endif /* HALRF_8821C_H */
