#ifndef HALRF_8822C_H
#define HALRF_8822C_H

#include <cstdint>

#include "logger.h"
#include "RtlAdapter.h"
#include "SelectedChannel.h"
#include "Jaguar3Calibration.h"

namespace jaguar3 {

/* Halrf8822c — userspace port of the RTL8822C IQK (IQ-imbalance / LO-leakage)
 * calibration from the OpenHD/rtl88x2cu vendor halrf (hal/phydm/halrf/rtl8822c/
 * halrf_iqk_8822c.c). Replaces the hardcoded captured IQK gain registers in
 * HalJaguar3::init_rfk with a real calibration that runs fresh each boot — the
 * gains depend on chip + temperature, so the captured values don't survive
 * across boots (the cause of the flaky structured-path RX).
 *
 * This is a large multi-part port. The phydm dm_struct is replaced by a thin
 * shim over RtlAdapter: BB/MAC registers via rtw_read32/write32, RF registers
 * via the 8822C direct window (config_phydm_*_rf_reg_8822c: BB[0x3c00|0x4c00 +
 * (addr<<2)], 20-bit). The dm->IQK_info calibration state lives in IqkInfo. */
class Halrf8822c : public Jaguar3Calibration {
public:
  Halrf8822c(RtlAdapter device, Logger_t logger);

  /* phy_iq_calibrate_8822c entry — runs the full IQK (backup -> setup -> LOK/
   * TXK/RXK per path -> restore -> fill report). bw selects NB-IQK (5/10 MHz);
   * channel selects the 2.4/5 GHz band branch. */
  void phy_iq_calibrate(ChannelWidth_t bw, uint8_t channel) override;

  /* DAC calibration (halrf_dac_cal_8822c). Channel-independent; run once at
   * bring-up before IQK. Produces the operational BB DC/gain state (0x18bc/c0/
   * d8/dc + path-B) that gates strong on-air TX. */
  void dac_calibrate() override;

  /* Persistently grant the shared antenna to WLAN. On this WiFi+BT combo the
   * coex firmware otherwise arbitrates the antenna away from WL after a few
   * seconds, killing on-air TX while the MAC keeps draining frames. Two parts,
   * both required (port of rtw_coex_set_ant_path COEX_SET_ANT_WONLY):
   *  - GNT_WL=SW-high, GNT_BT=SW-low via the LTE-coex indirect window (0x38).
   *  - antenna path-control owner = WL: REG_SYS_SDIO_CTRL+3 (0x73) |=
   *    BIT_LTE_MUX_CTRL_PATH (BIT(26) -> 0x04 in byte 3). Without the owner bit
   *    GNT_WL doesn't actually hold the antenna. */
  void force_wl_antenna() override {
    /* WL scoreboard = active|on (+BT_INT_EN): tells the BT firmware WiFi is on,
     * so it leaves the SW-forced GNT alone. REG_WIFI_BT_INFO=0xAA, val =
     * 0x2 | ACTIVE(BIT0) | ONOFF(BIT1) | BIT_BT_INT_EN(BIT15) = 0x8003. */
    _device.rtw_write<uint16_t>(0xAA, 0x8003);
    set_gnt_wl_high();
    mac_write8(0x73, static_cast<uint8_t>(mac_read8(0x73) | 0x04));
  }

  /* One-time WiFi-only coex bring-up (port of rtw8822c_coex_cfg_init +
   * cfg_rfe_type + cfg_gnt_fix + rtw_coex_set_ant_path COEX_SET_ANT_WONLY, for a
   * 5 GHz WiFi-only link). Disables the LTE/BT coex arbitration and tells the BB
   * that WL TX must never be masked by GNT_BT, then forces the antenna to WL.
   * Without this the coex firmware switches the antenna away ~12 s into TX. Call
   * once after bring-up (and force_wl_antenna() periodically as a backstop). */
  void coex_wlan_only_init() override;

  /* Periodic 5 GHz coex decision (port of rtw_coex_action_wl_under5g): GNT_BT to
   * HW-PTA + GNT_WL SW-high, antenna owner = WL, the WL-wins-all PTA table, and
   * the active scoreboard. Re-applied on the coex runtime tick so the FW's PTA
   * keeps the antenna with WLAN during sustained 5 GHz TX. */
  void coex_run_5g() override;

  /* Thermal TX-power tracking (port of rtw8822c_pwr_track for 5 GHz). Each tick:
   * trigger + read the RF thermal meter (RF_T_METER 0x42), compute the delta vs
   * the cold-boot reference, look up the swing table, and write the compensating
   * index to the per-path PWR_TRACK field (0x18a0/0x41a0[6:0]). Without this the
   * upper-5 GHz PA droops with heat until the FW protection silences TX (~52 s);
   * with it the output is held and TX sustains. Reference is captured on the
   * first call (chip is coldest at bring-up). */
  void pwr_track() override;

  /* One-shot path-A meter read for GetThermalStatus (see Jaguar3Calibration):
   * trigger + read RF 0x42[6:1], like a pwr_track tick without the swing
   * write. No efuse thermal baseline is wired on the CU, so the baseline is
   * the first-read cold reference (delta = "since first read"); shares
   * _thermal_ref[0] with pwr_track so both agree on the reference. */
  bool read_thermal(uint8_t &raw, uint8_t &baseline) override;

  /* Light periodic re-assertion of the WiFi-only coex state (no RF/BB writes):
   * re-disable the LTE/BT arbitration and re-grant the antenna to WL, in case
   * the coex firmware re-enables it. Safe to call on the TX hot path. */
  void coex_keepalive() override {
    btc_write_indirect(0x38, 0x80, 0x0);      /* LTE_COEX_CTRL BIT_LTE_COEX_EN=0 */
    btc_write_indirect(0xa0, 0xffff, 0xffff); /* LTE_WL_TRX_CTRL all-pass */
    btc_write_indirect(0xa4, 0xffff, 0xffff); /* LTE_BT_TRX_CTRL all-pass */
    force_wl_antenna();
  }

private:
  /* DACK helpers (halrf_*_8822c) */
  void dack_mode(uint32_t *i_value, uint32_t *q_value); /* halrf_mode_8822c */
  void dack_poll(uint16_t addr, uint32_t mask, uint32_t data);

  /* --- dm_struct shim: register access (RFREG path = 8822C direct window) --- */
  enum RfPath { PATH_A = 0, PATH_B = 1 };
  static constexpr uint32_t RFREG_MASK = 0x000FFFFF;
  uint32_t bb_read(uint16_t addr) { return _device.rtw_read32(addr); }
  void bb_write(uint16_t addr, uint32_t val) { _device.rtw_write32(addr, val); }
  uint32_t bb_get(uint16_t addr, uint32_t mask);
  void bb_set(uint16_t addr, uint32_t mask, uint32_t val) {
    _device.phy_set_bb_reg(addr, mask, val);
  }
  void mac_write8(uint16_t addr, uint8_t val) { _device.rtw_write8(addr, val); }
  uint8_t mac_read8(uint16_t addr) { return _device.rtw_read8(addr); }
  uint32_t rf_read(uint8_t path, uint16_t addr, uint32_t mask);
  void rf_write(uint8_t path, uint16_t addr, uint32_t mask, uint32_t val);
  static void delay_us(uint32_t us);
  static void delay_ms(uint32_t ms);

  /* --- BTC/GNT indirect-register + IQK trigger/poll primitives --- */
  uint32_t btc_wait_ready();
  uint32_t btc_read_indirect(uint16_t reg);
  void btc_write_indirect(uint16_t reg, uint32_t mask, uint32_t val);
  void set_gnt_wl_high();
  void set_gnt_wl_gnt_bt(bool before_k);
  bool check_cal(uint8_t path, uint8_t cmd); /* poll 0x2d9c==0x55, read fail */
  void get_cfir(uint8_t idx, uint8_t path);  /* iqk_get_cfir_8822c (debug=0) */
  void backup_iqk(uint8_t step, uint8_t path); /* _iqk_backup_iqk_8822c */
  bool one_shot(uint8_t path, uint8_t idx);  /* _iqk_one_shot_8822c trigger+poll */

  /* --- LOK (LO-leakage) calibration chain --- */
  void cal_path_off(uint8_t path_unused);
  void rf_direct_access(uint8_t path, bool direct);
  void lok_setting(uint8_t path, uint8_t idac_bs);
  bool lok_one_shot(uint8_t path, bool for_rxk);
  bool lok_check(uint8_t path);
  void lok_tune(uint8_t path);

  /* --- TXK / RXK per-path setting steps --- */
  void txk_setting(uint8_t path);          /* _iqk_txk_setting_8822c */
  void lok_for_rxk_setting(uint8_t path);  /* _iqk_lok_for_rxk_setting_8822c */
  void rxk1_setting(uint8_t path);         /* _iqk_rxk1_setting_8822c */
  void rxk2_setting(uint8_t path, bool is_gs); /* _iqk_rxk2_setting_8822c */
  bool gain_search_fail(uint8_t path, uint8_t step); /* _iqk_rx_iqk_gain_search_fail */
  bool rx_iqk_by_path(uint8_t path);       /* _iqk_rx_iqk_by_path_8822c (RXK SM) */
  void iqk_by_path(bool segment);          /* _iqk_iqk_by_path_8822c (top SM) */
  void start_iqk(bool segment);            /* _iqk_start_iqk_8822c */

  /* --- pre-cal setup + report --- */
  void information();        /* _iqk_information_8822c (tssi/band/ch/bw) */
  void macbb();              /* _iqk_macbb_8822c */
  void bb_for_dpk_setting(); /* _iqk_bb_for_dpk_setting_8822c */
  void afe_setting(bool do_iqk); /* _iqk_afe_setting_8822c */
  void fill_report(uint8_t ch);  /* _iqk_fill_iqk_report_8822c */

  /* --- ported helpers (flat register sequences) --- */
  void nctl();                       /* _iqk_nctl_8822c (generated table) */
  void backup_mac_bb(uint32_t *mac, uint32_t *bb);
  void backup_rf(uint32_t rf[][2]);
  void restore_mac_bb(const uint32_t *mac, const uint32_t *bb);
  void restore_rf(const uint32_t rf[][2]);

  RtlAdapter _device;
  Logger_t _logger;

  /* pwr_track thermal reference (per RF path), captured on the first pwr_track()
   * call when the chip is coldest; -1 = not yet captured. _thermal_lck_ref is the
   * separate reference for the LC-recal drift check (rtw_phy_pwrtrack_need_lck). */
  int _thermal_ref[2] = {-1, -1};
  int _thermal_lck_ref = -1;

  /* LC-tank re-calibration (port of rtw8822c_do_lck): re-locks the synth so it
   * doesn't drift out of lock as the chip heats. Triggered from pwr_track when
   * the thermal delta crosses the LCK threshold (8). */
  void do_lck();

  /* dm->IQK_info — calibration state carried across the step machine. Array
   * dims mirror the vendor dm_iqk_info (SS_8822C = 2 spatial streams; the first
   * index [2] is the current/backup channel slot). */
  static constexpr int SS = 2;
  struct IqkInfo {
    uint8_t iqk_step = 0;
    uint8_t rxiqk_step = 0;
    uint8_t kcount = 0;
    uint32_t iqk_times = 0;
    uint32_t rf_reg18 = 0;
    uint32_t tmp_gntwl = 0;
    uint16_t tmp1bcc = 0;
    bool is_nb_iqk = false;
    bool is_tssi_mode = false;
    bool is_5g = true; /* band_type: true=5G false=2.4G */
    uint8_t bw_val = 0; /* band_width: 0=20 1=40 2=80 (NB uses is_nb_iqk path) */
    uint32_t rf_reg58 = 0;
    uint32_t iqc_matrix[2][SS] = {{0x20000000, 0x20000000},
                                  {0x20000000, 0x20000000}};
    bool lok_fail[SS] = {true, true};
    bool iqk_fail[2][SS] = {{true, true}, {true, true}};
    uint32_t iqk_channel[2] = {0, 0};
    uint32_t lok_idac[2][SS] = {{0}};
    uint16_t rxiqk_agc[2][SS] = {{0}};
    uint8_t bypass_iqk[2][SS] = {{0}};
    uint8_t rxiqk_fail_code[2][SS] = {{0}};
    bool iqk_fail_report[2][SS][2] = {}; /* [chslot][path][TX/RX] */
    uint16_t iqk_cfir_real[2][SS][2][17] = {};
    uint16_t iqk_cfir_imag[2][SS][2][17] = {};
    uint8_t gs_retry_count[2][SS][2] = {};
    uint8_t retry_count[2][SS][3] = {};
    uint32_t nbtxk_1b38[SS] = {0};
    uint32_t nbrxk_1b3c[SS] = {0};
    uint8_t lna_idx = 0;
    bool isbnd = false;
    uint8_t iqk_band = 0; /* 0=2G 1=5G (from RF18[16]) */
    uint8_t iqk_ch = 0;
    uint8_t iqk_bw = 0;   /* 3/2/1 = 20/40/80 (RF18[13:12]) */
  } _iqk;
};

/* register-count constants (backup lists in _phy_iq_calibrate_8822c) */
constexpr int MAC_REG_NUM_8822C = 3;
constexpr int BB_REG_NUM_8822C = 21;
constexpr int RF_REG_NUM_8822C = 3;

} /* namespace jaguar3 */

#endif /* HALRF_8822C_IQK_H */
