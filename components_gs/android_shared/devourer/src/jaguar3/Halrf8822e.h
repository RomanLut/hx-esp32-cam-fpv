#ifndef HALRF_8822E_H
#define HALRF_8822E_H

#include <cstdint>

#include "logger.h"
#include "RtlAdapter.h"
#include "SelectedChannel.h"
#include "Jaguar3Calibration.h"

namespace jaguar3 {

/* Halrf8822e — userspace port of the RTL8822E (RTL8812EU/8822EU) halrf
 * calibration from OpenHD/rtl88x2eu (hal/phydm/halrf/rtl8822e/). Sibling of
 * Halrf8822c behind the Jaguar3Calibration interface: the bring-up flow,
 * register windows (RF direct window 0x3c00/0x4c00 — identical to 8822c) and
 * the table walker are shared; the calibration ALGORITHMS differ.
 *
 * Most notably the DACK: 8822c samples the BB DC report (0x2dbc) while 8822e
 * uses the AFE S0/S1 banks (halrf_dack_s0/s1_8822e) — applying the 8822c DACK
 * to 8822e silicon does not settle, which is why this separate port exists.
 *
 * PORT STATUS (Phase C, in progress — NOT landing until on-air validated on the
 * BL-M8812EU2): the register shim + structure are in place; the DSP routines
 * (DACK, IQK, TSSI, DPK, TXGAPK) are ported incrementally and hardware-iterated.
 */
class Halrf8822e : public Jaguar3Calibration {
public:
  Halrf8822e(RtlAdapter device, Logger_t logger,
             const devourer::DeviceConfig &cfg = {});

  void phy_iq_calibrate(ChannelWidth_t bw, uint8_t channel) override;
  void dac_calibrate() override;
  void pwr_track() override;
  void set_pwr_track_ctx(uint8_t thermal_base_a, uint8_t thermal_base_b,
                         uint8_t channel) override;
  void force_wl_antenna() override;
  void coex_wlan_only_init() override;
  void coex_run_5g() override;
  void coex_keepalive() override;

private:
  /* dm_struct register-access shim (RF path = the 8822c/8822e direct window:
   * BB[0x3c00|0x4c00 + (addr<<2)], 20-bit). Identical to Halrf8822c's shim; a
   * shared Jaguar3RfAccess base is a noted follow-up once both impls mature. */
  static constexpr uint32_t RFREG_MASK = 0x000FFFFF;
  uint32_t bb_read(uint16_t addr) { return _device.rtw_read32(addr); }
  void bb_write(uint16_t addr, uint32_t val) { _device.rtw_write32(addr, val); }
  uint32_t bb_get(uint16_t addr, uint32_t mask);
  void bb_set(uint16_t addr, uint32_t mask, uint32_t val);
  uint8_t mac_read8(uint16_t addr) { return _device.rtw_read8(addr); }
  void mac_write8(uint16_t addr, uint8_t val) { _device.rtw_write8(addr, val); }
  uint32_t rf_read(uint8_t path, uint16_t addr, uint32_t mask);
  void rf_write(uint8_t path, uint16_t addr, uint32_t mask, uint32_t val);
  static void delay_us(uint32_t us);
  static void delay_ms(uint32_t ms);

  /* --- DAC calibration (port of halrf_dac_cal_8822e / halrf_8822e.c) ---
   * 8822e DACK uses the AFE S0/S1 banks (0x3800/0x3900) rather than 8822c's BB
   * DC report (0x2dbc): per path/IQ, run MSB-bias + DC-offset auto-cal, read
   * back the coefficients, and reload them into the AFE coefficient registers.
   * ADDCK (ADC DC) runs first, then DACK with a checkfail retry loop. */
  void wdack(uint16_t reg, uint32_t mask, uint32_t data); /* double-write */
  bool afereg_check(uint16_t wa);
  void write_check_afe(uint16_t add, uint32_t data);
  void dack_soft_rst();
  void dack_reset();
  void dac_fifo_reset();
  void check_addc(uint8_t path);
  void addck_s0();
  void addck_s1();
  void dack_backup_s0();
  void dack_backup_s1();
  void dack_reload_by_path(uint8_t path, uint8_t index);
  void dack_reload(uint8_t path);
  void dack_s0();
  void dack_s1();
  void dack_val(bool from_reg);
  bool dack_checkfail();

  /* --- IQK (port of halrf_iqk_8822e.c, in progress) ---
   * The 8822e IQK has no separate NCTL microcode (unlike 8822c); the 0x1b00
   * engine config lives in the cal_init table applied at bring-up. Ported
   * incrementally: support layer (cal-ready poll, BTC-indirect/GNT, CFIR +
   * MAC/BB/RF backup-restore) first, then the per-path LOK/TXK/RXK cores. */
  static constexpr uint8_t SS_8822E = 2;
  static constexpr uint8_t IQK_TXIQK = 0, IQK_RXIQK = 1, IQK_RXIQK1 = 1,
                           IQK_RXIQK2 = 2, IQK_NBTXK = 3, IQK_NBRXK = 4,
                           IQK_LOK1 = 5, IQK_LOK2 = 6;
  static constexpr uint8_t IQK_BAND_2G = 0; /* iqk_band: 0 = 2.4G, 1 = 5G */

  /* poll the NCTL "cal ready" handshake (0x2d9c==0x55, 0x1bfc==0x8000); returns
   * the fail flag (0x1b08[26], except LOK which can't fail this way). */
  bool iqk_check_cal(uint8_t path, uint8_t cmd);
  /* LTE/BTC indirect register window (0x1700/0x1704/0x1708) used for GNT. */
  uint32_t btc_wait_indirect_ready();
  uint32_t btc_read_indirect(uint16_t reg);
  void btc_write_indirect(uint16_t reg, uint32_t mask, uint32_t val);
  void iqk_set_gnt_wl_high();
  void iqk_set_gnt_wl_gnt_bt(bool before_k);
  /* CFIR (calibration FIR coefficient) backup/reload + AFE setup. */
  void iqk_backup_txcfir(uint8_t path);
  void iqk_backup_rxcfir(uint8_t path);
  void iqk_reload_txcfir(uint8_t ch, uint8_t path);
  void iqk_reload_rxcfir(uint8_t ch, uint8_t path);
  void iqk_backup_cfir(uint8_t path, uint8_t idx);
  void iqk_reload_cfir(uint8_t ch, uint8_t path, uint8_t idx);
  void iqk_ex_dac_fifo_rst() { dack_soft_rst(); } /* halrf_ex_dac_fifo_rst_8822e */
  void iqk_afe_setting();
  void iqk_afe_restore();
  /* MAC/BB/RF register backup-restore (over the fixed backup tables). */
  void iqk_backup_mac_bb();
  void iqk_backup_rf();
  void iqk_restore_mac_bb();
  void iqk_restore_rf();
  void iqk_switch_table();      /* copy ch-0 results to ch-1 slot */
  void iqk_tx_pause();          /* halt TX (TXPAUSE + hw stop) before cal */
  void iqk_macbb_setting();     /* MAC/BB into IQK mode */
  void iqk_macbb_restore();     /* MAC/BB back to normal */
  void iqk_reload_lok_setting(uint8_t path);
  bool iqk_lok1_check(uint8_t path);
  bool iqk_one_shot(uint8_t path, uint8_t idx); /* trigger one cal step */

  /* Per-path LOK/TXK/RXK cores (port of the _iqk_{5g,2g}_{txk,rxk}_iqk and
   * rx_gain_search1 functions). TXK returns the kfail flag; RXK records its
   * fail into _iqk and is void. */
  bool iqk_5g_txk(uint8_t path);
  bool iqk_2g_txk(uint8_t path);
  bool iqk_5g_rx_gain_search1(uint8_t path, bool force);
  bool iqk_2g_rx_gain_search1(uint8_t path, bool force);
  void iqk_5g_rxk(uint8_t path);
  void iqk_2g_rxk(uint8_t path);
  void iqk_preset() {} /* _iqk_preset_8822e is a no-op on this silicon */
  void iqk_by_path();       /* loop both paths: TXK then (on pass) RXK + BK CFIR */
  void iqk_start_iqk();     /* GNT handshake around iqk_by_path */
  void iqk_information(ChannelWidth_t bw); /* read rf18 → band/ch/bw/nbiqk */
  void iqk_init();          /* one-time state reset (firstrun) */

  /* IQK calibration state (subset of the vendor dm_iqk_info). Grown as the
   * per-path cores are ported. */
  struct IqkInfo {
    uint32_t tmp_gntwl = 0;
    uint32_t rf_reg18 = 0;
    uint32_t rf_reg58 = 0;
    uint16_t iqk_cfir_real[3][2][2][17] = {}; /* [ch][path][TX/RX][tap] */
    uint16_t iqk_cfir_imag[3][2][2][17] = {};
    uint32_t txxy[3][2] = {};
    uint32_t rxxy[3][2] = {};
    uint32_t cfir_en[3][2] = {};
    uint32_t iqk_tab[2] = {};
    uint32_t lok_idac[2][2] = {};
    bool lok_fail[2] = {};
    bool iqk_fail[2][2] = {};
    bool iqk_fail_report[3][2][2] = {}; /* [ch][path][TX/RX] */
    uint32_t iqc_matrix[2][2] = {};
    uint8_t iqk_band = 0;  /* 0 = 2.4G, 1 = 5G */
    uint8_t iqk_ch = 0;
    uint8_t iqk_bw = 0;
    bool is_nbiqk = false;
    uint8_t kcount = 0, fail_count = 0, fail_step = 0;
    uint8_t iqk_step = 0, iqk_times = 0;
  };
  IqkInfo _iqk;
  bool _iqk_inited = false;

  /* IQK register backup storage (over the fixed backup tables). */
  uint32_t _mac_backup[4] = {};
  uint32_t _bb_backup[21] = {};
  uint32_t _rf_backup[3][2] = {};

  /* DACK result state (subset of the vendor dm_dack_info we actually use). */
  struct DackInfo {
    uint8_t new_msbk_d[2][2][16] = {};
    uint16_t new_biask_d[2] = {};
    uint8_t dadck_d[2][2] = {};
    uint16_t addc[2][2] = {};
    uint16_t addck_d[2][2] = {};
    bool addck_timeout[2] = {};
    bool dadck_timeout[2] = {};
    bool msbk_timeout[2] = {};
  };
  DackInfo _dack;

  /* --- TXGAPK (TX gain calibration, port of halrf_txgapk_8822e.c, in progress) ---
   * The 8822E gross 5 GHz TX gain comes from this per-path BB/AFE-IQK-based gain
   * calibration (devourer ran only DACK+IQK, so the 5 GHz PA was uncalibrated and
   * ~12 dB low). Ported incrementally: register-sequence backup/restore + the
   * BB/AFE IQK setup first, then the offset calculation + gain-table write. */
  static constexpr uint8_t TXGAPK_GAIN_NUM = 12;  /* RF_GAIN_TABLE_NUM */
  struct TxgapkInfo {
    uint32_t rf3f_bp[5][TXGAPK_GAIN_NUM][2] = {}; /* [band][idx][path] */
    uint8_t rf3f_same[5][TXGAPK_GAIN_NUM][2] = {}; /* consecutive-equal [11:5] */
    int8_t offset[TXGAPK_GAIN_NUM][2] = {};
    int8_t final_offset[TXGAPK_GAIN_NUM][2] = {};
    int32_t track_d[2][11] = {};
    int32_t track_ta[2][11] = {};
    int32_t power_d[2][11] = {};
    int32_t power_ta[2][11] = {};
    uint8_t read_txgain = 0;
    bool is_txgapk_ok = false;
  };
  TxgapkInfo _txgapk;

  void txgapk_backup_bb(const uint16_t *reg, uint32_t *backup, uint32_t n);
  void txgapk_reload_bb(const uint16_t *reg, const uint32_t *backup, uint32_t n);
  void txgapk_backup_kip(const uint16_t *reg, uint32_t backup[][2], uint32_t n);
  void txgapk_reload_kip(const uint16_t *reg, const uint32_t backup[][2], uint32_t n);
  void txgapk_tx_pause();
  void txgapk_bb_iqk(uint8_t path);
  void txgapk_afe_iqk(uint8_t path);
  void txgapk_afe_iqk_restore(uint8_t path);
  void txgapk_bb_iqk_restore(uint8_t path);
  void txgapk_calculate_offset(uint8_t path, uint8_t channel);
  void txgapk_rf_restore(uint8_t path);
  uint32_t txgapk_calc_tx_gain(uint32_t original, int8_t offset);
  void txgapk_write_gain_bb_table();
  void txgapk_save_all();             /* read all-band gain tables into rf3f_bp */
  void txgapk_write_tx_gain(uint8_t channel); /* apply offsets -> RF gain table */
  /* Orchestrator: prime the gain table (once) then run per-path BB/AFE-IQK gain
   * measurement and write the corrected gains for the tuned channel. Port of
   * halrf_txgapk_8822e. Run after IQK; sets the 8822E TX gain (incl. 5 GHz). */
  void do_txgapk(uint8_t channel);

  /* --- Thermal TX-power tracking (port of odm_txpowertracking_new_callback_
   * thermal_meter + odm_tx_pwr_track_set_pwr8822e). Holds TX power flat as the PA
   * heats over a sustained link: read the RF thermal meter (RF 0x42[6:1]) per
   * path, compare to the efuse baseline, look up a per-band swing index, and
   * write it to the BB TX-power-adjust register (0x18a0/0x41a0[7:0]). */
  void thermal_track_8822e();

public:
  /* One-shot path-A meter read for GetThermalStatus (see Jaguar3Calibration):
   * shares the tracker's one-time RF 0x42[19] trigger; baseline = efuse 0xd0. */
  bool read_thermal(uint8_t &raw, uint8_t &baseline) override;

private:
  uint8_t _therm_base[2] = {0xFF, 0xFF}; /* efuse 0xd0/0xd1 baseline per path */
  uint8_t _track_channel = 0;
  bool _tm_triggered = false;
  uint8_t _therm_avg[2][4] = {};         /* rolling avg buffer */
  uint8_t _therm_avg_idx[2] = {0, 0};
  uint8_t _therm_avg_cnt[2] = {0, 0};
  int _last_swing[2] = {0x7fff, 0x7fff}; /* log-on-change sentinel */

  RtlAdapter _device;
  Logger_t _logger;
  bool _skip_txgapk = false; /* tuning.skip_txgapk */
  bool _gaintab_dbg = false; /* debug.gaintab_dbg */
};

} /* namespace jaguar3 */

#endif /* HALRF_8822E_H */
