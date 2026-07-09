#ifndef HAL_JAGUAR3_H
#define HAL_JAGUAR3_H

#include <memory>

#include "AdapterHealth.h"
#include "logger.h"
#include "RtlAdapter.h"
#include "SelectedChannel.h"
#include "ChipVariant.h"
#include "Jaguar3PhyTables.h"
#include "Jaguar3Calibration.h"
#include "HalmacJaguar3Fw.h"
#include "HalmacJaguar3MacInit.h"
#include "PhyTableLoaderJaguar3.h"

namespace jaguar3 {

/* HalJaguar3 — Jaguar3 chip bring-up: power sequencing, queue/page/LLT init, BB /
 * AGC / RF table application, and band/channel setup. This is the Jaguar3
 * analogue of src/HalModule (which is Jaguar1-only).
 *
 * Per the locked HalMAC decision, this class hand-rolls power-on / queue / EFUSE
 * in devourer's style and delegates only the firmware download to the verbatim
 * HalMAC port in HalmacJaguar3Fw. */
class HalJaguar3 {
public:
  HalJaguar3(RtlAdapter device, Logger_t logger,
           ChipVariant variant = ChipVariant::C8822C,
           const devourer::DeviceConfig &cfg = {});

  /* Full RX-capable bring-up: power-on -> FW download -> MAC/BB/RF config ->
   * band+channel set. Mirrors HalModule::rtw_hal_init() for Jaguar1. */
  void rtw_hal_init(SelectedChannel channel);

  /* Clean de-init: stop TRX DMA, then run the card-disable PWR_SEQ so the chip
   * parks in a low-power, re-enumerable state. Mirrors the kernel rtw88 driver's
   * unbind path. Must run before the USB interface is released. Best-effort:
   * tolerates a chip that has already dropped off the bus. */
  void rtw_hal_deinit();

  /* Read REG_SYS_CFG1 and decode chip cut / vendor / RF type (port of
   * rtl8822c_ops.c read_chip_version). Populates _phy_ctx.cut_version used by
   * the BB/AGC/RF table walker. Reads a register, so it runs during bring-up;
   * exposed for the eventual chip-id-based family detection. */
  void read_chip_version();

  /* Decode the logical EFUSE map far enough to return EEPROM_RFE_OPTION_8822C
   * (logical offset 0xCA) — the RFE front-end type that gates BB/RFE config.
   * Returns 0 if the byte is unprogrammed (0xFF) or the read fails. */
  uint8_t read_efuse_rfe_type();

  /* 8822E physical efuse (OTP) access — the 8822C/Jaguar1 efuse_OneByteRead can't
   * read it (different addr layout, RDY bit, and a required sw-power-cut). */
  void efuse_pwr_cut_8822e(bool on);     /* bracket every read range */
  uint8_t efuse_phys_read_8822e(uint16_t addr); /* one byte; pwr-cut must be on */
  void config_pa_bias_8822e();           /* efuse PA-bias trim -> RF 0x60 (kfree) */

  /* Read the efuse-calibrated 5 GHz BW40 base TXAGC index for `channel` on each
   * RF path (the per-channel calibration the kernel programs; devourer otherwise
   * hardcodes JAGUAR3_TXPWR_REF_BASE). 0xFF = unprogrammed. 8822E only. */
  void read_efuse_txpwr_base_8822e(uint8_t channel, uint8_t &base_a,
                                   uint8_t &base_b);

  /* Decoded chip identity (valid after read_chip_version). */
  struct ChipVersion {
    uint8_t cut = 0;       /* 0=A,1=B,... (BIT_GET_CHIP_VER) */
    uint8_t vendor = 0;    /* 0=TSMC,1=SMIC,2=UMC */
    uint8_t rf_2t2r = 0;   /* 1 = 2T2R, 0 = 1T1R */
    bool test_chip = false;
  };
  ChipVersion chip_version() const { return _ver; }

  /* One fresh PHYSICAL logical-map read for health probing (see
   * src/AdapterHealth.h) — same holding-region window the rtw_hal_init cache
   * read uses; bypasses _efuse_cache. `len` must be sizeof(_efuse_cache).
   * Post-bring-up only, and 8822C ONLY: the 8822E's OTP is not reliably
   * readable after TX/coex bring-up by design (see cache_efuse_8822e) — a
   * stability probe there would flag healthy units. Returns false on 8822E. */
  bool probe_efuse_map(uint8_t *map, size_t len);

  /* Outcome of the fw download run by the last rtw_hal_init — forwarded from
   * the DLFW state machine's real hardware boundaries (checksum-ready bits vs
   * the 0xC078 boot handshake; see HalmacJaguar3Fw::boot_status). */
  const devourer::FwBootStatus &fw_boot_status() const {
    return _fw.boot_status();
  }

private:
  void power_off();           /* card-disable PWR_SEQ — reset from active state */
  void power_on();            /* card-enable PWR_SEQ */
  void init_rfk();            /* RF-calibration init (0x1B00 cal_init block) */
  void apply_bb_rf_agc_tables(); /* phydm BB/AGC/RF tables via PhyTableLoader */
  void bf_init();                /* rtl8822c_phy_bf_init: BF/MU + NDPA sounding */
  void config_phydm_parameter_init(); /* POST_SETTING: 3-wire + OFDM/CCK block */
  void enable_tx_path();         /* OFDM/CCK TX block + AGC/path enable (on-air TX) */
  /* monitor RX config is devourer-specific (the vendor driver has no pure
   * monitor mode). */
  void monitor_rx_cfg();

public:
  /* Enable the BB/RF RX path (port of phydm config_trx_mode RX half:
   * set_rf_mode_table + config_cck/ofdm_rx_path + bb_reset + igi_toggle, all
   * BB_PATH_AB). The IGI toggle is mandatory — it forces the BB to emit the
   * 3-wire command that puts the RF HW into RX mode; without it the RF never
   * enters RX and the chip delivers zero frames. Call AFTER the channel/BW is
   * set (the 3-wire RX-mode command must follow the channel config). */
  void enable_rx_path();

  /* Beamformer-side sounding RF/BB config (port of phydm_txbf_rfmode, the
   * su_bfee_cnt > 0 branch): RF mode-table entries + the BB TxBF antenna
   * mapping (0x1e24/0x820/0x1e2c/0x1e30) the chip needs to emit the hardware
   * NDP that follows a descriptor-marked NDPA. Per-variant RF values (8822C vs
   * 8822E). Call when arming the self-sounding beamformer, after bring-up. */
  void txbf_rfmode_sounder();

  /* Configure the 8822E RFE control pins (port of phydm_rfe_8822e). These drive
   * the external antenna-switch / PAPE (PA enable) GPIOs and are only set for
   * rfe_type 21..24; without them the TX PA is not enabled (TX dark) even when
   * RX works. No-op for 8822C. Call after the channel is tuned. */
  void config_rfe(uint8_t channel);

  /* 8822E channel-finalize TX writes the shared (8822c-derived) set_channel_bwmode
   * omits: the band-specific OFDM Tx backoff / Tx scaling (5 GHz 0x818/0x81c) and
   * the TX triangular-shaping / CFR (0xa74/0x80c/0x81c/0x8a0) from
   * config_phydm_switch_channel_8822e + phydm_tx_triangular_shap_cfg_8822e. The
   * 8822C uses a different shaping-filter scheme, so this is gated to C8822E.
   * Without the 5 GHz Tx-scaling write the EU's 5 GHz on-air power collapses
   * (~3 Mbps SDR duty vs the kernel's ~48). Call after config_rfe. No-op for 8822C. */
  void config_channel_8822e(uint8_t channel);

  /* Put the DPK gain block in explicit bypass (port of _dpk_force_bypass_8822e).
   * The kernel force-bypasses DPK for RFE type 21/22 (the EU) instead of running
   * the pre-distortion calibration. Call after IQK. No-op for 8822C. */
  void dpk_force_bypass_8822e();

private:

public:
  /* Run the ported IQK calibration. Must be called AFTER the channel is tuned
   * (IQK reads RF18 for band/ch). Replaces init_rfk's hardcoded gain regs. */
  void run_iqk(SelectedChannel channel);

  /* Persistently grant the shared antenna to WLAN (see
   * Halrf8822c::force_wl_antenna). Call after bring-up and periodically so
   * the coex firmware can't switch the antenna away mid-TX. */
  void force_wl_antenna() { _cal->force_wl_antenna(); }

  /* One-time WiFi-only coex bring-up (disables BT/LTE coex arbitration + locks
   * the antenna to WL). See Halrf8822c::coex_wlan_only_init. */
  void coex_wlan_only_init() { _cal->coex_wlan_only_init(); }
  void coex_keepalive() { _cal->coex_keepalive(); }
  void coex_run_5g() { _cal->coex_run_5g(); }
  void pwr_track() { _cal->pwr_track(); }
  /* One-shot thermal-meter read for GetThermalStatus (see Jaguar3Calibration).
   * Caller serializes against pwr_track (same RF 0x42 trigger RMW). */
  bool read_thermal(uint8_t &raw, uint8_t &baseline) {
    return _cal ? _cal->read_thermal(raw, baseline) : false;
  }

  /* Runtime H2C (host-to-firmware) over the HMEBOX mailboxes (port of
   * rtw_fw_send_h2c_command): 8-byte command, round-robin across 4 boxes. */
  void send_h2c_raw(uint32_t msg, uint32_t msg_ext);

  /* WL_PHY_INFO heartbeat (H2C 0x58, port of rtw_fw_update_wl_phy_info). The
   * firmware's idle detection relies on the host reporting WL PHY activity every
   * ~2 s; without it the FW powers the RF down after ~30 s. Call periodically
   * during RX and TX. */
  void fw_update_wl_phy_info();

  /* SET_PWR_MODE (H2C 0x20) = active / all-on (port of rtw_leave_lps_core:
   * mode 0, rlbm 0, smart_ps 0, awake_interval 1, pwr_state RTW_ALL_ON). Tells
   * the firmware to keep every power domain on instead of auto-entering power
   * save. Call at bring-up and periodically. */
  void fw_set_pwr_mode_active();

  /* Coex protocol H2C. query_bt_info (0x61) asks the FW to report BT state — on a
   * WiFi-only chip the FW then settles into "BT absent". tdma_off (0x60, all
   * params 0) disables coex time-division so the FW stops time-slicing the
   * antenna away from WLAN. */
  void fw_coex_query_bt_info() { send_h2c_raw(0x61u | (1u << 8), 0); }
  void fw_coex_tdma_off() { send_h2c_raw(0x60u, 0); }

private:

  /* Decode the packed extended-header EFUSE into a logical map up to (the block
   * holding) offset `upto`. Backs read_efuse_rfe_type + read_efuse_txpwr_base. */
  void read_efuse_logical_map(uint8_t *map, size_t len, uint16_t upto);

  /* One-shot decode of the logical EFUSE into _efuse_cache during rtw_hal_init,
   * where OTP access is reliable. RFE + per-channel TX-power base are then served
   * from the cache — the efuse is not reliably readable later (e.g. in InitWrite,
   * after TX/coex bring-up). 8822E only; a no-op leaves the cache invalid. */
  void cache_efuse_8822e();
  uint8_t _efuse_cache[0x100];
  bool _efuse_cache_valid = false;

  RtlAdapter _device;
  devourer::DeviceConfig _cfg; /* skip_iqk + calibration forward */
  Logger_t _logger;
  HalmacJaguar3Fw _fw;
  HalmacJaguar3MacInit _macinit;
  /* Per-generation calibration (IQK/DACK/pwr-track/coex) + phydm table data,
   * selected by variant. The bring-up flow, walker and apply sequence are
   * shared; only these differ between rtl8822c and rtl8822e. */
  std::unique_ptr<Jaguar3Calibration> _cal;
  std::unique_ptr<Jaguar3PhyTables> _tables;

  /* phydm table-selection context. cut_version is set from read_chip_version()
   * and rfe_type from read_efuse_rfe_type(), both during rtw_hal_init() before
   * apply_bb_rf_agc_tables() walks the tables. The initialiser values are just
   * the pre-bring-up defaults. */
  jaguar3::Jaguar3PhyContext _phy_ctx{/*cut_version=*/0, /*rfe_type=*/0};
  ChipVariant _variant;
  ChipVersion _ver{};
  uint8_t _h2c_box = 0; /* round-robin HMEBOX index for send_h2c_raw */
};

} /* namespace jaguar3 */

#endif /* HAL_JAGUAR3_H */
