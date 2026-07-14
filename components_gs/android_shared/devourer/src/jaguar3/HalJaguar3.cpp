#include "HalJaguar3.h"
#include <cstdlib>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace jaguar3 {

namespace {
constexpr uint32_t MASKDWORD = 0xFFFFFFFFu;

/* Run a bring-up calibration step that issues a very large number of USB
 * control-IN reads (DACK/IQK poll loops fire tens of thousands of rtw_read
 * calls), re-running it if a transient USB glitch makes rtw_read throw
 * std::ios_base::failure. Without this a single mid-DACK NAK/timeout aborts the
 * whole bring-up (std::terminate) — the same class of glitch StartRxLoop already
 * retry-wraps for post-InitWrite reads. Each calibration pass resets the AFE, so
 * a re-run is idempotent. Rethrows after `tries` exhausted (a persistent failure
 * is a real error, not a glitch). */
template <typename F>
void retry_cal(Logger_t &logger, const char *what, F &&step, int tries = 3) {
  for (int attempt = 1;; ++attempt) {
    try {
      step();
      return;
    } catch (const std::exception &e) {
      if (attempt >= tries)
        throw;
      logger->error("Jaguar3 {}: USB read glitch ({}) — retry {}/{}", what,
                    e.what(), attempt, tries - 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }
}

/* Writer for BB / AGC tables: handles the 0xf9..0xfe pseudo-address delay
 * encoding (see odm_config_bb_phy_8822c) and otherwise does a full-dword BB
 * write. */
void write_bb(RtlAdapter &dev, uint32_t addr, uint32_t data) {
  switch (addr) {
  case 0xfe: std::this_thread::sleep_for(std::chrono::milliseconds(50)); return;
  case 0xfd: std::this_thread::sleep_for(std::chrono::milliseconds(5)); return;
  case 0xfc: std::this_thread::sleep_for(std::chrono::milliseconds(1)); return;
  case 0xfb: std::this_thread::sleep_for(std::chrono::microseconds(50)); return;
  case 0xfa: std::this_thread::sleep_for(std::chrono::microseconds(5)); return;
  case 0xf9: std::this_thread::sleep_for(std::chrono::microseconds(1)); return;
  default: dev.phy_set_bb_reg(static_cast<uint16_t>(addr), MASKDWORD, data);
  }
}
} /* namespace */

HalJaguar3::HalJaguar3(RtlAdapter device, Logger_t logger,
                       ChipVariant variant, const devourer::DeviceConfig &cfg)
    : _device{device}, _cfg{cfg}, _logger{logger},
      _fw{device, logger, variant}, _macinit{device, logger},
      _cal{make_jaguar3_calibration(variant, device, logger, cfg)},
      _tables{make_jaguar3_phy_tables(variant)}, _variant{variant} {}

/* Run the ported IQK calibration (halrf_iqk_8822c). Called after the channel is
 * tuned. Replaces the captured hardcoded IQK gain registers with a real per-boot
 * calibration. */
void HalJaguar3::run_iqk(SelectedChannel channel) {
  if (_cfg.tuning.skip_iqk) {
    _logger->info("Jaguar3: IQK SKIPPED (tuning.skip_iqk debug)");
    _device.rtw_write16(0x0522, 0x0000);
    return;
  }
  /* IQK shares DACK's heavy USB-read poll profile — retry on a transient glitch
   * rather than aborting bring-up. */
  retry_cal(_logger, "IQK", [&] {
    _cal->phy_iq_calibrate(channel.ChannelWidth, channel.Channel);
  });
  /* IQK's macbb() sets REG_TXPAUSE (0x522)=0xff to halt TX during calibration,
   * and the IQK MAC restore only covers {0x520,0x1c,0x70} — TXPAUSE is left
   * paused. The vendor clears it when bringing TX up; for monitor inject we must
   * clear it here, or queued frames never transmit (TX FIFO fills and stalls). */
  _device.rtw_write16(0x0522, 0x0000);
}

/* Full vendor-source bring-up in HalMAC _halmac_init_hal order (hal/hal_halmac.c
 * of rtl88x2cu): power-off (reset from any state) -> pre_init_system_cfg ->
 * power_on (card-enable) -> init_system_cfg -> DLFW -> init_mac_cfg ->
 * init_usb_cfg -> enable_bb_rf -> init_phy (BB/AGC/RF + RFK) -> monitor RX cfg.
 * Every step is ported from vendor source. */
void HalJaguar3::rtw_hal_init(SelectedChannel channel) {
  ChannelWidth_t bw = channel.ChannelWidth;

  _macinit.pre_init_system_cfg();
  power_on();                 /* mac_power_switch(on) — card_en_flow */
  read_chip_version();        /* needs MAC alive; supplies cut for system_cfg */
  cache_efuse_8822e();        /* one-shot OTP decode while access is reliable */
  if (_variant == ChipVariant::C8822E && _efuse_cache_valid)
    /* Efuse thermal baseline (0xd0/0xd1) + channel for pwr_track thermal tracking. */
    _cal->set_pwr_track_ctx(_efuse_cache[0xd0], _efuse_cache[0xd1],
                            channel.Channel);
  _phy_ctx.rfe_type = read_efuse_rfe_type(); /* EEPROM_RFE_OPTION (0xCA) */
  /* RTL8822E RFE default. The 8822E phydm BB/AGC/RF tables are keyed by rfe_type;
   * the only rfe values the 8822E front-end logic (phydm_rfe_8822e) handles are
   * 21..24, and the RX path is only configured for those cases. The BL-M8812EU2's
   * efuse rfe reads 0, which selects table blocks that leave the RX front-end /
   * antenna switch unconfigured -> the chip receives nothing (BB-reg diff vs the
   * kernel 8822eu driver showed the rfe-21 antenna-switch pins 0x1840/0x1844/
   * 0x4140/0x4144 zeroed). Default to 21 (the kernel's effective value for this
   * module) so the correct front-end table blocks apply. 8822C is unaffected. */
  if (_variant == ChipVariant::C8822E &&
      (_phy_ctx.rfe_type == 0 || _phy_ctx.rfe_type == 0xff))
    _phy_ctx.rfe_type = 21;
  _logger->info("Jaguar3: rfe_type=0x{:02x}", _phy_ctx.rfe_type);
  _macinit.init_system_cfg(bw, _ver.cut);

  if (!_fw.download_default_firmware()) {
    _logger->error("Jaguar3: firmware download FAILED (structured)");
    return;
  }
  _logger->info("Jaguar3: firmware booted (structured)");

  if (!_macinit.init_mac_cfg(bw)) {
    _logger->error("Jaguar3: init_mac_cfg FAILED (structured)");
    return;
  }
  _macinit.init_usb_cfg();         /* USB RX-DMA mode — RX delivery to bulk-IN */
  _macinit.enable_bb_rf(true);     /* set_hw_value(EN_BB_RF) */
  apply_bb_rf_agc_tables();        /* init_phy: BB + AGC + RF tables */
  config_pa_bias_8822e();          /* kfree: efuse PA-bias trim -> RF 0x60 */
  config_phydm_parameter_init();   /* POST_SETTING: 3-wire + OFDM/CCK block + bb-reset */
  init_rfk();                      /* RF cal_init (0x1B00); IQK runs via run_iqk */
  /* halrf DACK — DAC cal before IQK. Retry-wrapped: its status-poll loops issue
   * tens of thousands of USB reads and an intermittent glitch was aborting
   * bring-up (rtw_read iostream error, seen on 8822EU). */
  retry_cal(_logger, "DACK", [this] { _cal->dac_calibrate(); });
  bf_init();                       /* rtl8822c_phy_bf_init (rtl8822c_halinit.c) */
  monitor_rx_cfg();                /* devourer monitor-mode RX enable */
  enable_tx_path();                /* enable OFDM/CCK TX block (gates on-air TX) */
  _logger->info("Jaguar3: bring-up complete");
}

/* Port of config_phydm_parameter_init_8822c(ODM_POST_SETTING): turn on the
 * 3-wire RF interface (0x180c/0x410c), enable the OFDM/CCK block (0x1c3c[1:0]=3),
 * then BB-reset. Run after the BB phy table is applied. The CCK GI-bound/PD-init
 * sub-tuning is dynamic adaptivity, omitted here. */
void HalJaguar3::config_phydm_parameter_init() {
  _device.phy_set_bb_reg(0x180c, 0x3, 0x3);
  _device.phy_set_bb_reg(0x180c, 1u << 28, 0x1);
  _device.phy_set_bb_reg(0x410c, 0x3, 0x3);
  _device.phy_set_bb_reg(0x410c, 1u << 28, 0x1);
  _device.phy_set_bb_reg(0x1c3c, 0x3, 0x3); /* enable OFDM and CCK block */
  uint32_t v = _device.rtw_read32(0x0);     /* phydm_bb_reset: 0x0[16] 1->0->1 */
  _device.rtw_write32(0x0, v | (1u << 16));
  _device.rtw_write32(0x0, v & ~(1u << 16));
  _device.rtw_write32(0x0, v | (1u << 16));
}

/* TX-path BB bits the phy-table leaves at pre-setting values, re-asserted after
 * the calibrations (DACK/IQK touch 0x1c3c). Each traced to vendor source; with
 * the 3-wire enable (config_phydm_parameter_init) + DACK this gives full on-air
 * TX (SDR-validated +43 dB, cold).
 *   0x1c3c[1:0]=3   enable OFDM and CCK block (parameter_init POST).
 *   0x1c80[29:24]=0x22  TRX-path setting (phydm trx_mode/rfe path config).
 *   0x1c90[15]=0    enable writing the TX-AGC table (bbrstb TX-AGC report).
 *   0x1cd0[30:28]=7 TX-gain-K path enables (halrf_txgapk_8822c). */
void HalJaguar3::enable_tx_path() {
  _device.phy_set_bb_reg(0x1c3c, 0x00000003, 0x3);
  _device.phy_set_bb_reg(0x1c80, 0x3F000000, 0x22);
  _device.phy_set_bb_reg(0x1c90, 0x00008000, 0x0);
  _device.phy_set_bb_reg(0x1cd0, 0x70000000, 0x7);
  _logger->info("Jaguar3: TX path enabled (OFDM/CCK block + AGC/path bits)");
}

/* Port of _dpk_force_bypass_8822e (halrf_dpk_8822e.c): put the DPK gain block in
 * explicit bypass on both RF paths. do_dpk_8822e force-bypasses DPK for RFE type
 * 21/22 (the BL-M8812EU2 is rfe 21) rather than running the pre-distortion
 * calibration, so on the EU there is no real DPK — but the kernel still writes
 * the bypass state (subpage-2: 0x1b08[15:14]=3 / 0x1b04[7:0]=0x5b for S0,
 * 0x1b60[15:14]=3 / 0x1b5c[7:0]=0x5b for S1) so the DPK LUT is a clean
 * passthrough. Only the bypass path is ported; real DPK (rfe 23/24 on 5 GHz) is
 * not. 8822E only. */
void HalJaguar3::dpk_force_bypass_8822e() {
  if (_variant != ChipVariant::C8822E)
    return;
  _device.phy_set_bb_reg(0x1b00, (1u << 2) | (1u << 1), 0x2); /* DPK subpage 2 */
  _device.phy_set_bb_reg(0x1b08, (1u << 15) | (1u << 14), 0x3);
  _device.phy_set_bb_reg(0x1b04, 0x000000ff, 0x5b);           /* S0 bypass */
  _device.phy_set_bb_reg(0x1b60, (1u << 15) | (1u << 14), 0x3);
  _device.phy_set_bb_reg(0x1b5c, 0x000000ff, 0x5b);           /* S1 bypass */
  _device.phy_set_bb_reg(0x1b00, (1u << 2) | (1u << 1), 0x0); /* subpage 0 */
  _logger->info("Jaguar3(8822e): DPK force-bypass applied (rfe {})",
                _phy_ctx.rfe_type);
}

/* Port of phydm_rfe_8822e (phydm_hal_api8822e.c): drive the external RFE control
 * pins (antenna switch + PAPE/PA-enable). Only rfe_type 21..24 are handled by the
 * vendor; for other rfe the function is a no-op (and the 8822C has no equivalent).
 * The path selection collapses to BB_PATH_NON for 2G rfe21/22 and 5G rfe23/24. */
void HalJaguar3::config_rfe(uint8_t channel) {
  if (_variant != ChipVariant::C8822E)
    return;
  const uint8_t rfe = _phy_ctx.rfe_type;
  if (!(rfe == 21 || rfe == 22 || rfe == 23 || rfe == 24))
    return;

  const bool is_2g = channel <= 14;
  /* tx/rx ant status is BB_PATH_AB in our single-config use; default path = AB. */
  enum { BB_PATH_NON = 0, BB_PATH_A = 1, BB_PATH_B = 2, BB_PATH_AB = 3 };
  int path = BB_PATH_AB;
  if (is_2g) {
    if (rfe == 21 || rfe == 22)
      path = BB_PATH_NON;
  } else {
    if (rfe == 23 || rfe == 24)
      path = BB_PATH_NON;
  }

  switch (path) {
  case BB_PATH_NON:
    _device.phy_set_bb_reg(0x1840, 0xFFFFFFFF, 0x00007000);
    _device.phy_set_bb_reg(0x1844, 0xFFFFFFFF, 0x00007007);
    _device.phy_set_bb_reg(0x4140, 0xFFFFFFFF, 0x70700000);
    _device.phy_set_bb_reg(0x4144, 0xFFFFFFFF, 0x00000070);
    break;
  case BB_PATH_A:
    _device.phy_set_bb_reg(0x1840, 0xFFFFFFFF, 0x00002000);
    _device.phy_set_bb_reg(0x1844, 0xFFFFFFFF, 0x00003000);
    _device.phy_set_bb_reg(0x4140, 0xFFFFFFFF, 0x70700000);
    _device.phy_set_bb_reg(0x4144, 0xFFFFFFFF, 0x00000070);
    break;
  case BB_PATH_B:
    _device.phy_set_bb_reg(0x1840, 0xFFFFFFFF, 0x00007000);
    _device.phy_set_bb_reg(0x1844, 0xFFFFFFFF, 0x00007007);
    _device.phy_set_bb_reg(0x4140, 0xFFFFFFFF, 0x00200000);
    _device.phy_set_bb_reg(0x4144, 0xFFFFFFFF, 0x00000030);
    break;
  case BB_PATH_AB:
    _device.phy_set_bb_reg(0x1840, 0xFFFFFFFF, 0x00002000);
    _device.phy_set_bb_reg(0x1844, 0xFFFFFFFF, 0x00003000);
    _device.phy_set_bb_reg(0x4140, 0xFFFFFFFF, 0x00200000);
    _device.phy_set_bb_reg(0x4144, 0xFFFFFFFF, 0x00000030);
    break;
  }
  _logger->info("Jaguar3(8822e): RFE pins set (rfe={} ch={} path={})", rfe,
                channel, path);
}

/* Port of the band-specific TX writes in config_phydm_switch_channel_8822e plus
 * phydm_tx_triangular_shap_cfg_8822e. The shared set_channel_bwmode is a 8822C
 * port and omits these; the 8822C uses a separate shaping-filter scheme, so this
 * is C8822E-only. The 5 GHz Tx-scaling (0x81c[20:14]=0x4) + Tx-backoff
 * (0x818[26:22]=0xc) are what bring EU 5 GHz on-air power up to parity. */
void HalJaguar3::config_channel_8822e(uint8_t channel) {
  if (_variant != ChipVariant::C8822E)
    return;
  const bool is_2g = channel <= 14;

  if (!is_2g) {
    /* 5 GHz: Tx backoff OFDM + Tx scaling (config_phydm_switch_channel_8822e). */
    _device.phy_set_bb_reg(0x818, 0x07c00000, 0xc); /* Tx backoff OFDM */
    _device.phy_set_bb_reg(0x81c, 0x001fc000, 0x4); /* Tx scaling */
  }

  /* phydm_config_tx_path_8822e (BB_PATH_AB): OFDM + CCK TX antenna routing. The
   * phy_reg table leaves 0x820 at its default (0x11111111 = path-A-only on [7:0]);
   * the vendor runtime overrides it to the AB mapping. Without this the OFDM TX
   * path is not routed to both antennas — on this module the 5 GHz output collapses
   * (the antenna for 5 GHz is not on the default path) while 2 GHz still works.
   *   OFDM 0x820[7:0]=0x33 (AB 2ss/1ss) + 0x1e2c[15:0]=0x0404
   *   CCK  0x1a04[31:28]=0xc (AB) */
  _device.phy_set_bb_reg(0x820, 0xff, 0x33);
  _device.phy_set_bb_reg(0x1e2c, 0xffff, 0x0404);
  _device.phy_set_bb_reg(0x1a04, 0xf0000000, 0xc);

  /* phydm_tx_triangular_shap_cfg_8822e: CFR + triangular TX shaping (both bands). */
  _device.phy_set_bb_reg(0xa74, 1u << 31, 0x1);
  _device.phy_set_bb_reg(0x808, 0x70, 0x3);
  if (is_2g) {
    _device.phy_set_bb_reg(0xa74, 0x3ff, 0x15);
    _device.phy_set_bb_reg(0xa74, 0xffc00, 0x13);
    _device.phy_set_bb_reg(0x80c, 0xf, 0x5);
    _device.phy_set_bb_reg(0x81c, 0x000000ff, 0xff);
    _device.phy_set_bb_reg(0x81c, 0x0f000000, 0x0);
    _device.phy_set_bb_reg(0x8a0, 0xf0000000, 0xb);
  } else {
    _device.phy_set_bb_reg(0xa74, 0x3ff, 0x3f);
    _device.phy_set_bb_reg(0xa74, 0xffc00, 0x3f);
    _device.phy_set_bb_reg(0x80c, 0xf, 0x8);
    _device.phy_set_bb_reg(0x81c, 0x000000ff, 0x55);
    _device.phy_set_bb_reg(0x81c, 0x0f000000, 0x7);
    _device.phy_set_bb_reg(0x8a0, 0xf0000000, 0x0);
  }
  _logger->info("Jaguar3(8822e): channel TX finalize (ch={} {}G tx-scaling+shaping)",
                channel, is_2g ? 2 : 5);
}

/* Port of phydm config_trx_mode's RX half (phydm_hal_api8822c.c), BB_PATH_AB.
 * Shared 8822c/8822e — these BB/RF-path functions are byte-identical across the
 * two generations. devourer applied the BB table (which carries some RX-path
 * defaults) and enable_tx_path, but never ran this runtime RX-path config nor
 * the IGI toggle, so the RF HW was never commanded into RX mode and the chip
 * delivered zero frames (kernel rtw88 runs this at hw-start). */
void HalJaguar3::enable_rx_path() {
  constexpr uint32_t kAB = 0x3;

  /* --- set_rf_mode_table (rx_path != A => 0x4100 = 0x33312) --- */
  _device.phy_set_bb_reg(0x4100, 0x000fffff, 0x33312);

  /* --- config_cck_rx_path (AB) --- */
  _device.phy_set_bb_reg(0x1a04, 0x0f000000, 0x1); /* antA->CCK1, antB->CCK2 */
  _device.phy_set_bb_reg(0x1a2c, 1u << 5, 0x0);    /* enable RX clk gated */
  _device.phy_set_bb_reg(0x1a2c, 0x00060000, 0x1); /* enable MRC CCK barker */
  _device.phy_set_bb_reg(0x1a2c, 0x00600000, 0x1); /* enable MRC CCK CCA */

  /* --- config_ofdm_rx_path (AB), non-mp branch --- */
  _device.phy_set_bb_reg(0x0cc0, 0x7ff, 0x400);
  _device.phy_set_bb_reg(0x0cc0, 1u << 22, 0x0);
  _device.phy_set_bb_reg(0x0cc8, 0x7ff, 0x400);
  _device.phy_set_bb_reg(0x0cc8, 1u << 22, 0x0);
  _device.phy_set_bb_reg(0x1d30, 0x300, 0x1);       /* ht_mcs_limit */
  _device.phy_set_bb_reg(0x1d30, 0x600000, 0x1);    /* vht_nss_limit */
  _device.phy_set_bb_reg(0x0c44, 1u << 17, 0x1);    /* enable antenna weighting */
  _device.phy_set_bb_reg(0x0c54, 1u << 20, 0x1);    /* htstf ant-wgt enable */
  _device.phy_set_bb_reg(0x0c38, 1u << 24, 0x1);    /* MRC modified-ZF eqz */
  _device.phy_set_bb_reg(0x0824, 0x000f0000, kAB);   /* Rx_ant */
  _device.phy_set_bb_reg(0x0824, 0x0f000000, kAB);   /* Rx_CCA */

  /* --- bb_reset: toggle MAC 0x0[16] 1->0->1 --- */
  for (uint32_t v : {1u, 0u, 1u}) {
    uint32_t r = _device.rtw_read32(0x0);
    r = (r & ~(1u << 16)) | (v << 16);
    _device.rtw_write32(0x0, r);
  }

  /* --- igi_toggle: 0x1d70 -= 0x202 then restore, to emit the 3-wire RX-mode
   * command after the channel/BW config (the comment's key point). --- */
  uint32_t igi = _device.rtw_read32(0x1d70);
  _device.rtw_write32(0x1d70, igi - 0x202);
  _device.rtw_write32(0x1d70, igi);

  _logger->info("Jaguar3: RX path enabled (rf-mode + cck/ofdm rx-path + IGI toggle)");
}

/* Port of rtl8822c_phy_bf_init (hal/rtl8822c/rtl8822c_phy.c): beamforming /
 * MU-MIMO defaults + NDPA-sounding setup. devourer never ran this, leaving the
 * NDPA/TXBF path unconfigured; the MAC TX-protocol engine needs it (REG_NDPA_
 * OPT_CTRL / TXBF_CTRL "use NDPA parameter") before it will dequeue injected
 * frames from TXPKTBUF. Single-user monitor inject doesn't sound, so MU-MIMO is
 * left disabled (as the vendor does until sounding completes). */
void HalJaguar3::bf_init() {
  constexpr uint16_t REG_MU_TX_CTL = 0x14C0;
  constexpr uint16_t REG_MU_BF_OPTION = 0x167C;
  constexpr uint16_t REG_WMAC_MU_BF_CTL = 0x1680;
  constexpr uint16_t REG_TXBF_CTRL = 0x042C;
  constexpr uint16_t REG_NDPA_OPT_CTRL = 0x045F;

  uint32_t v32 = _device.rtw_read32(REG_MU_TX_CTL);
  v32 |= (1u << 16);                       /* R_MU_P1_WAIT_STATE_EN */
  v32 = (v32 & ~(0xfu << 12)) | (0xAu << 12); /* R_MU_RL = 0xA */
  v32 &= ~(1u << 7);                        /* disable MU-MIMO until sounding */
  v32 &= ~0x3fu;                            /* clear R_MU_TABLE_VALID */
  _device.rtw_write32(REG_MU_TX_CTL, v32);

  _device.rtw_write8(REG_MU_BF_OPTION,
                     static_cast<uint8_t>((3u << 4) | (1u << 6))); /* ACKPOLICY=3, EN */
  _device.rtw_write16(REG_WMAC_MU_BF_CTL, 0);

  /* MU NDPA rate/BW sourced from 0x45F (not the Tx desc): TXBF_CTRL[30] = 1 */
  uint8_t v = _device.rtw_read8(REG_TXBF_CTRL + 3);
  v |= 0x40; /* BIT_USE_NDPA_PARAMETER (BIT30) >> 24 */
  _device.rtw_write8(REG_TXBF_CTRL + 3, v);
  _device.rtw_write8(REG_NDPA_OPT_CTRL, 0x10); /* OFDM 6M, BW20 */

  /* STA2 CSI rate fixed at 6M (temp setting per vendor) */
  uint8_t v6df = _device.rtw_read8(0x06DF);
  _device.rtw_write8(0x06DF, static_cast<uint8_t>((v6df & 0xC0) | 0x04));
  _logger->info("Jaguar3: bf_init (NDPA/MU TX setup) applied");
}

/* Port of phydm_txbf_rfmode (phydm_hal_txbf_api.c), su_bfee_cnt > 0 branch —
 * the beamformer-side RF/BB sounding config. The RF mode-table entries make
 * the RF front-end able to transmit while the mode table says "RX" (the NDP
 * goes out SIFS after the NDPA, while the chip is turning to receive the
 * report); the BB writes enable the TxBF antenna mapping so the 2-antenna NDP
 * is emitted on both paths. RF registers are written through the direct
 * per-path BB window (0x3c00/0x4c00 + (addr<<2), 20-bit), the same mechanism
 * as the RF table load and the halrf calibration code. All masks are
 * contiguous, so phy_set_bb_reg's field-value semantics match the vendor
 * odm_set_rf_reg. */
void HalJaguar3::txbf_rfmode_sounder() {
  auto rf = [this](int path, uint16_t addr, uint32_t mask, uint32_t val) {
    uint16_t direct = static_cast<uint16_t>((path ? 0x4c00 : 0x3c00) +
                                            ((addr & 0xff) << 2));
    _device.phy_set_bb_reg(direct, mask & 0xfffff, val);
  };
  /* Bracket the RF mode-table writes with rstb_3wire like the RF table load
   * (RadioManagementJaguar3 / apply_bb_rf_agc_tables do the same) — without
   * it the mode-table entry writes can be dropped by the live 3-wire engine. */
  _device.phy_set_bb_reg(0x1c90, 1u << 8, 0);
  if (_variant == jaguar3::ChipVariant::C8822C) {
    /* Path A: RX-mode table entry with TX IQ generator on */
    rf(0, 0xef, 1u << 19, 1);      /* mode-table write enable */
    rf(0, 0x33, 0xF, 3);           /* select RX mode entry */
    rf(0, 0x3e, 0x3, 0x2);
    rf(0, 0x3f, 0xfffff, 0x65AFF);
    rf(0, 0xef, 1u << 19, 0);
    /* Path B: RX-mode + standby-mode entries */
    rf(1, 0xef, 1u << 19, 1);
    rf(1, 0x33, 0xF, 3);
    rf(1, 0x3f, 0xfffff, 0x996BF);
    rf(1, 0x33, 0xF, 1);           /* select standby entry */
    rf(1, 0x3f, 0xfffff, 0x99230);
    rf(1, 0xef, 1u << 19, 0);
  } else { /* C8822E */
    rf(0, 0xef, 1u << 19, 1);
    rf(0, 0x33, 0xF, 3);
    rf(0, 0x3e, 0xF, 0x4);
    rf(0, 0x3f, 0xfffff, 0xc1aff);
    rf(0, 0xef, 1u << 19, 0);
    rf(1, 0xef, 1u << 19, 1);
    rf(1, 0x33, 0xF, 3);
    rf(1, 0x3e, 0xF, 0x1);
    rf(1, 0x3f, 0xfffff, 0x306bf);
    rf(1, 0xef, 1u << 19, 0);
  }
  /* rstb_3wire back on + force anapar update (same tail as the table load). */
  _device.phy_set_bb_reg(0x1c90, 1u << 8, 1);
  _device.phy_set_bb_reg(0x1830, 1u << 29, 1);
  _device.phy_set_bb_reg(0x4130, 1u << 29, 1);
  /* BB TxBF antenna mapping (same on both variants). */
  _device.phy_set_bb_reg(0x1e24, 1u << 11, 1); /* Nsts > Nc: no V matrix */
  _device.phy_set_bb_reg(0x1e24, (1u << 28) | (1u << 29), 0x2);
  _device.phy_set_bb_reg(0x1e24, 1u << 30, 1);
  /* TX BF logic map + TX path enable for Nsts 1..2 */
  _device.phy_set_bb_reg(0x0820, 0xff, 0x33);
  _device.phy_set_bb_reg(0x1e2c, 0xffff, 0x404);
  _device.phy_set_bb_reg(0x0820, 0xffff0000, 0x33);
  _device.phy_set_bb_reg(0x1e30, 0xffff, 0x404);
  _logger->info("Jaguar3: txbf rf-mode (beamformer sounding RF/BB) applied");
}

/* Clean de-init — the counterpart to rtw_hal_init, run on shutdown. The kernel
 * rtw88 driver does this on unbind and the adapter always re-enumerates cleanly
 * afterwards; devourer skipping it is what left the chip hung (RX DMA running,
 * RX FIFO overflowing) and unable to re-enumerate (-71). Stop the TRX engine
 * first (clear CR MACRXEN/MACTXEN + close RX filter), then run the card-disable
 * power sequence. Best-effort: any write may fail if the chip is already gone. */
void HalJaguar3::rtw_hal_deinit() {
  _logger->info("Jaguar3: clean de-init (stop TRX + card-disable)");
  _device.rtw_write16(0x0100, 0x0000);   /* CR: clear MACRXEN/MACTXEN — halt TRX */
  _device.rtw_write32(0x0608, 0x00000000); /* RCR: drop all RX (stop FIFO fill) */
  power_off();                            /* card-disable PWR_SEQ → re-enumerable */
}

/* Monitor-mode RX configuration (devourer-specific; the vendor driver has no
 * pure-monitor path). Accept all frames incl. CRC/ICV errors, append PHY status
 * drvinfo (so parse_rx_8822c's drvinfo_size is consistent), all RX filter maps
 * open. RCR bits: AAP/APM/AM/AB/ACF/AICV/ACRC32 + APP_PHYSTS. */
void HalJaguar3::monitor_rx_cfg() {
  constexpr uint16_t REG_RCR_8822C = 0x0608;
  constexpr uint16_t REG_RXFLTMAP0_8822C = 0x06A0;
  constexpr uint16_t REG_RXFLTMAP1_8822C = 0x06A2;
  constexpr uint16_t REG_RXFLTMAP2_8822C = 0x06A4;
  constexpr uint16_t REG_RX_DRVINFO_SZ_8822C = 0x060F;
  /* Enable the full MAC: CR = TRX-DMA | PROTOCOL_EN | SCHEDULE_EN | MACTXEN |
   * MACRXEN(+ENSWBCN). init_mac_cfg only set CR=0x0F (DMA enable); without
   * MACRXEN (BIT7) the MAC RX engine never runs — the structured-path RX gap. */
  _device.rtw_write16(0x0100, 0x06FF);
  /* accept-all + keep FCS/ICV-error frames + append phy-status (BIT28).
   * BIT0 (AAP) is what makes monitor mode promiscuous for unicast: without it
   * the WMAC passes only broadcast/multicast/physical-match (APM|AM|AB) up,
   * silently dropping unicast frames addressed to third parties — e.g. NDPA
   * control frames and VHT beamforming reports, which is why the beamformee
   * (whose arm programs the self-MAC to the NDPA RA) saw sounding frames while
   * a plain monitor did not. */
  _device.rtw_write32(REG_RCR_8822C, 0xF410400F | (1u << 28));
  _device.rtw_write8(REG_RX_DRVINFO_SZ_8822C, 0x04);
  _device.rtw_write16(REG_RXFLTMAP0_8822C, 0xFFFF);
  _device.rtw_write16(REG_RXFLTMAP1_8822C, 0xFFFF);
  _device.rtw_write16(REG_RXFLTMAP2_8822C, 0xFFFF);
  _logger->info("Jaguar3: monitor RX config applied (accept-all + phystatus)");
}

/* Port of rtl8822c_ops.c read_chip_version(). REG_SYS_CFG1_8822C=0x00F0:
 *   [23] RTL_ID (1=test chip), [27] RF_TYPE_ID (1=2T2R), [15:12] CHIP_VER (cut),
 *   [19:16] VENDOR_ID -> >>2 -> 0=TSMC/1=SMIC/2=UMC. */
void HalJaguar3::read_chip_version() {
  constexpr uint16_t REG_SYS_CFG1_8822C = 0x00F0;
  uint32_t v = _device.rtw_read32(REG_SYS_CFG1_8822C);

  _ver.test_chip = (v & (1u << 23)) != 0;
  _ver.cut = static_cast<uint8_t>((v >> 12) & 0xf);
  _ver.rf_2t2r = (v & (1u << 27)) ? 1 : 0;
  uint8_t vend = static_cast<uint8_t>(((v >> 16) & 0xf) >> 2);
  _ver.vendor = (vend <= 2) ? vend : 0;

  _phy_ctx.cut_version = _ver.cut; /* feeds the BB/AGC/RF table walker */

  static const char *vn[] = {"TSMC", "SMIC", "UMC"};
  _logger->info("Jaguar3 chip: 8822C cut={} ({}{}) {} (SYS_CFG1=0x{:08x})",
                _ver.cut, vn[_ver.vendor], _ver.test_chip ? ",test" : "",
                _ver.rf_2t2r ? "2T2R" : "1T1R", v);
}

/* Decode the packed (extended-header) EFUSE into a logical map up to the byte we
 * need and return logical offset 0xCA (EEPROM_RFE_OPTION_8822C). Standard
 * Realtek format: per section a header byte (or header+ext byte) gives a logical
 * block offset + 4-bit word-enable; each enabled 2-byte word follows. */
/* 8822E "software power cut" sequence around physical efuse (OTP) access — port
 * of enable/disable_efuse_sw_pwr_cut (halmac_efuse_8822e.c), read variant
 * (is_write=0, so the PMC unlock-code write is skipped). The 8822E OTP is only
 * readable while EV2EF power is cut in to the efuse macro via REG_SYS_ISO_CTRL;
 * the 8822C/Jaguar1 reader (RtlAdapter::efuse_OneByteRead) never does this, so
 * 8822E efuse reads return 0xff. Bracket every physical read range with on/off. */
void HalJaguar3::efuse_pwr_cut_8822e(bool on) {
  constexpr uint16_t SYS_ISO = 0x0000, PMC = 0x00CC;
  constexpr uint16_t EFC1 = 0x00A4;      /* REG_EFUSE_CTRL_1 */
  constexpr uint32_t kBurst = 1u << 19;  /* BIT_EF_BURST */
  constexpr uint16_t kEbCore = 1u << 8, kPwcS = 1u << 14, kPwcB = 1u << 15;
  constexpr uint8_t kWrMsk = 1u << 2;
  if (on) {
    _device.rtw_write8(PMC, static_cast<uint8_t>(_device.rtw_read8(PMC) | kWrMsk));
    _device.rtw_write16(SYS_ISO, static_cast<uint16_t>(_device.rtw_read16(SYS_ISO) | kPwcS));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    _device.rtw_write16(SYS_ISO, static_cast<uint16_t>(_device.rtw_read16(SYS_ISO) | kPwcB));
    _device.rtw_write16(SYS_ISO, static_cast<uint16_t>(_device.rtw_read16(SYS_ISO) & ~kEbCore));
    /* Enable OTP burst mode. Mandatory for reliable 8822E OTP reads (port of
     * read_hw_efuse_8822e): without it the per-byte reads intermittently return
     * 0xff, so the packed-map walk breaks early and returns an empty logical map
     * (efuse RFE + per-channel TX-power base then read as unprogrammed). */
    _device.rtw_write32(EFC1, _device.rtw_read32(EFC1) | kBurst);
  } else {
    _device.rtw_write32(EFC1, _device.rtw_read32(EFC1) & ~kBurst);
    _device.rtw_write16(SYS_ISO, static_cast<uint16_t>(_device.rtw_read16(SYS_ISO) | kEbCore));
    _device.rtw_write16(SYS_ISO, static_cast<uint16_t>(_device.rtw_read16(SYS_ISO) & ~kPwcB));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    _device.rtw_write16(SYS_ISO, static_cast<uint16_t>(_device.rtw_read16(SYS_ISO) & ~kPwcS));
    _device.rtw_write8(PMC, static_cast<uint8_t>(_device.rtw_read8(PMC) & ~kWrMsk));
  }
}

/* Single physical efuse byte via the 8822E "V1" EFUSE_CTRL layout: address at
 * BIT_EF_ADDR_V1 (shift 16 / mask 0x7ff), RDY = BIT(29) (NOT the 8822C BIT(31)),
 * data in [7:0]. Caller must have efuse_pwr_cut_8822e(true) + burst mode active. */
uint8_t HalJaguar3::efuse_phys_read_8822e(uint16_t addr) {
  constexpr uint16_t EFC = 0x0030;
  constexpr uint32_t kRdy = 1u << 29;          /* BIT_EF_RDY */
  constexpr uint32_t kAddr = 0x7ffu << 16;     /* BITS_EF_ADDR_V1 */
  constexpr uint32_t kData = 0xffffu;          /* BIT_MASK_EF_DATA_V1 */
  /* Read-modify-write preserving the other EFUSE_CTRL bits (port of
   * read_hw_efuse_8822e): clear only the addr+data fields, set the address, and
   * clear RDY to trigger. Writing a bare addr<<16 (zeroing the rest) left required
   * control bits clear, so low-address reads returned 0xff and the packed-map walk
   * broke at physical offset 0 — the whole logical map (RFE + TX-power base) read
   * as unprogrammed. */
  uint32_t v = _device.rtw_read32(EFC);
  v = (v & ~(kAddr | kData | kRdy)) | ((static_cast<uint32_t>(addr) & 0x7ff) << 16);
  _device.rtw_write32(EFC, v);
  for (int i = 0; i < 1000; ++i) {
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    uint32_t t = _device.rtw_read32(EFC);
    if (t & kRdy)
      return static_cast<uint8_t>(t & 0xff);
  }
  return 0xff;
}

/* Port of phydm_get_set_pa_bias_offset_8822e (halrf_kfree.c): read the per-path
 * 2G/5G PA-bias trim nibbles from physical efuse and program RF 0x60 (2G [15:12],
 * 5G [19:16]). Matches the kernel — on the BL-M8812EU2 the efuse holds 2G=0x2 /
 * 5G=0x4 (kernel RF 0x60=0x42000). No-op for 8822C or unprogrammed efuse (0xff).
 * Requires the working 8822E efuse path (efuse_pwr_cut + V1 read). */
void HalJaguar3::config_pa_bias_8822e() {
  if (_variant != ChipVariant::C8822E)
    return;
  efuse_pwr_cut_8822e(true);
  uint8_t pg2a = efuse_phys_read_8822e(0x5c6); /* PPG_PABIAS_2GA_22E */
  uint8_t pg2b = efuse_phys_read_8822e(0x5c5); /* PPG_PABIAS_2GB_22E */
  uint8_t pg5a = efuse_phys_read_8822e(0x5c8); /* PPG_PABIAS_5GA_22E */
  uint8_t pg5b = efuse_phys_read_8822e(0x5c7); /* PPG_PABIAS_5GB_22E */
  efuse_pwr_cut_8822e(false);
  if (pg2a == 0xff) {
    _logger->info("Jaguar3(8822e): PA bias not programmed in efuse (no-op)");
    return;
  }
  auto rf60 = [this](uint16_t base, uint32_t mask, uint32_t v) {
    _device.phy_set_bb_reg(static_cast<uint16_t>(base + (0x60 << 2)), mask, v);
  };
  rf60(0x3c00, 0x0000f000, pg2a & 0xf); /* 2G s0 -> RF_A 0x60[15:12] */
  rf60(0x4c00, 0x0000f000, pg2b & 0xf); /* 2G s1 -> RF_B */
  rf60(0x3c00, 0x000f0000, pg5a & 0xf); /* 5G s0 -> RF_A 0x60[19:16] */
  rf60(0x4c00, 0x000f0000, pg5b & 0xf); /* 5G s1 -> RF_B */
  _logger->info("Jaguar3(8822e): PA bias applied (2G A=0x{:x} B=0x{:x}; 5G A=0x{:x} B=0x{:x})",
                pg2a & 0xf, pg2b & 0xf, pg5a & 0xf, pg5b & 0xf);
}

/* Decode the packed (extended-header) EFUSE into a logical map, up to (and
 * including the block holding) logical offset `upto`. Shared by read_efuse_rfe_type
 * and read_efuse_txpwr_base_8822e. `map` must be zero-init'd by the caller (this
 * fills 0xFF for gaps). Standard Realtek section format: header (or header+ext)
 * gives a logical block offset + 4-bit word-enable; each enabled 2-byte word
 * follows. */
bool HalJaguar3::probe_efuse_map(uint8_t *map, size_t len) {
  /* 8822E OTP reads are not reliable after TX/coex bring-up (by design — see
   * cache_efuse_8822e); probing there would flag healthy units. 8822C only. */
  if (_variant != ChipVariant::C8822C)
    return false;
  if (map == nullptr || len != sizeof(_efuse_cache))
    return false;
  read_efuse_logical_map(map, len, 0xFA);
  return true;
}

void HalJaguar3::read_efuse_logical_map(uint8_t *map, size_t len, uint16_t upto) {
  constexpr uint16_t kPhysMax = 1024; /* EFUSE_REAL_CONTENT_LEN_8822C */
  for (size_t i = 0; i < len; ++i) map[i] = 0xFF;

  /* Physical-byte reader. The 8822E OTP needs the V1 layout + sw-power-cut
   * (efuse_OneByteRead, the 8822C/Jaguar1 path, returns 0xff on it); so for the
   * EU bracket the whole packed-map walk in one pwr-cut and read via the V1
   * primitive. Returns the byte, or 0xff on failure (the walk treats 0xff as
   * end-of-map / skip, same as the original). */
  const bool eu = (_variant == ChipVariant::C8822E);
  if (eu)
    efuse_pwr_cut_8822e(true);
  auto rd = [this, eu](uint16_t a) -> uint8_t {
    if (eu)
      return efuse_phys_read_8822e(a);
    uint8_t d = 0xFF;
    return _device.efuse_OneByteRead(a, &d) ? d : 0xFF;
  };

  if (eu) {
    /* 8822E packed OTP format: 2-byte block headers h0/h1 with
     *   h0 = 0x30 | (block >> 4),  h1 = ((block & 0x0F) << 4) | word_en,
     * i.e. block = ((h0 & 0x0F) << 4) | (h1 >> 4); word_en low nibble of h1
     * (bit set = word absent). The map has leading 0xFF padding, so 0xFF at a
     * header position is skipped (not treated as end-of-map — that was the bug
     * that returned an empty map). Any other non-0x3X byte ends the walk.
     * (Verified against the kernel's efuse_map: logical 0x22=0x49, 0x4C=0x52,
     * 0xCA=0x15.) */
    uint16_t phys = 0;
    int ff_run = 0;
    while (phys < kPhysMax) {
      uint8_t h0 = rd(phys++);
      if (h0 == 0xFF) {
        if (++ff_run >= 64) break; /* long 0xFF run = past programmed area */
        continue;
      }
      ff_run = 0;
      if ((h0 & 0xF0) != 0x30)
        break; /* not a header marker — end of map */
      uint8_t h1 = rd(phys++);
      uint16_t block = static_cast<uint16_t>(((h0 & 0x0F) << 4) | (h1 >> 4));
      uint8_t word_en = h1 & 0x0F;
      uint16_t base = static_cast<uint16_t>(block << 3);
      for (uint8_t i = 0; i < 4; i++) {
        if (word_en & (1u << i))
          continue; /* word absent */
        for (uint8_t k = 0; k < 2; k++) {
          uint8_t d = rd(phys++);
          uint16_t idx = static_cast<uint16_t>(base + i * 2 + k);
          if (idx < len)
            map[idx] = d;
        }
      }
    }
    efuse_pwr_cut_8822e(false);
    return;
  }

  uint16_t phys = 0;
  while (phys < kPhysMax) {
    uint8_t hdr = rd(phys++);
    if (hdr == 0xFF)
      break;
    uint16_t offset;
    uint8_t word_en;
    if ((hdr & 0x1F) == 0x0F) { /* extended header */
      uint8_t ext = rd(phys++);
      if ((ext & 0x0F) == 0x0F)
        continue; /* invalid / skip */
      offset = static_cast<uint16_t>(((ext & 0xF0) >> 1) | ((hdr & 0xE0) >> 5));
      word_en = ext & 0x0F;
    } else {
      offset = (hdr >> 4) & 0x0F;
      word_en = hdr & 0x0F;
    }
    uint16_t base = static_cast<uint16_t>(offset << 3); /* offset * 8 bytes */
    for (uint8_t i = 0; i < 4; i++) {
      if (word_en & (1u << i))
        continue; /* word skipped */
      for (uint8_t k = 0; k < 2; k++) {
        uint8_t d = rd(phys++);
        uint16_t idx = static_cast<uint16_t>(base + i * 2 + k);
        if (idx < len)
          map[idx] = d;
      }
    }
    if (base > upto + 8)
      break; /* past the byte we need */
  }
}

void HalJaguar3::cache_efuse_8822e() {
  if (_variant != ChipVariant::C8822E)
    return;
  read_efuse_logical_map(_efuse_cache, sizeof(_efuse_cache), 0xFA);
  _efuse_cache_valid = true;
  _logger->info("Jaguar3(8822e): efuse decoded (0x22={:x} 0x4c={:x} 0xca={:x})",
                _efuse_cache[0x22], _efuse_cache[0x4c], _efuse_cache[0xca]);
}

uint8_t HalJaguar3::read_efuse_rfe_type() {
  constexpr uint16_t kRfeLogicalOff = 0x00CA;
  uint8_t rfe;
  if (_efuse_cache_valid) {
    rfe = _efuse_cache[kRfeLogicalOff];
  } else {
    uint8_t map[0x100 + 0x40]; /* enough to cover block holding 0xCA */
    read_efuse_logical_map(map, sizeof(map), kRfeLogicalOff);
    rfe = map[kRfeLogicalOff];
  }
  return (rfe == 0xFF) ? 0 : rfe;
}

/* 5G channel -> efuse channel-group index (0..13), port of rtw_get_ch_group's 5G
 * branch (hal_com_phycfg.c). The efuse stores one BW40 base index per group. */
static int chnl_group_5g(uint8_t ch) {
  static const uint8_t hi[14] = {42, 48, 58, 64, 106, 114, 122, 130,
                                 138, 144, 155, 161, 171, 177};
  for (int g = 0; g < 14; ++g)
    if (ch <= hi[g])
      return g;
  return 13;
}

/* Read the efuse-calibrated 5 GHz BW40 base TXAGC index for `channel` on the two
 * RF paths (logical map: path A 5G section @0x22, path B @0x4C, indexed by
 * channel group). Returns the per-path base bytes; 0xFF means unprogrammed. This
 * is the per-channel calibration the kernel programs and devourer otherwise
 * hardcodes (JAGUAR3_TXPWR_REF_BASE). 8822E only. */
void HalJaguar3::read_efuse_txpwr_base_8822e(uint8_t channel, uint8_t &base_a,
                                             uint8_t &base_b) {
  base_a = base_b = 0xFF;
  if (_variant != ChipVariant::C8822E)
    return;
  if (channel <= 14) /* 2.4G base is a different efuse section (0x10/0x3A) with a
                      * distinct CCK/BW40 layout — not handled here; caller falls
                      * back to the tuned default. */
    return;
  constexpr uint16_t k5gA = 0x22, k5gB = 0x4C; /* EEPROM_TX_PWR_INX_8822E 5G start */
  const uint8_t *map;
  uint8_t local[0x80];
  if (_efuse_cache_valid) {
    map = _efuse_cache; /* decoded early where OTP access is reliable */
  } else {
    read_efuse_logical_map(local, sizeof(local), k5gB + 14);
    map = local;
  }
  int g = chnl_group_5g(channel);
  base_a = map[k5gA + g];
  base_b = map[k5gB + g];
  /* Add the per-path OFDM/HT BW20 differential (efuse byte 14 of each path's 5G
   * section, low nibble, signed 4-bit) so the ref matches the kernel's MCS7
   * TXAGC index (base + diff): e.g. ch36 A 0x49+2=0x4b, B 0x52+2=0x54. */
  auto s4 = [](uint8_t v) -> int { int n = v & 0x0F; return (n & 0x8) ? n - 16 : n; };
  if (base_a != 0xFF) base_a = static_cast<uint8_t>(base_a + s4(map[k5gA + 14]));
  if (base_b != 0xFF) base_b = static_cast<uint8_t>(base_b + s4(map[k5gB + 14]));
  _logger->info("Jaguar3(8822e): efuse 5G ch{} grp{} ref A=0x{:x} B=0x{:x}",
                channel, g, base_a, base_b);
}

namespace {
/* RTL8822C USB power-on sequence (card-disable -> card-emulation -> active),
 * transcribed from the halmac WLAN_PWR_CFG flow card_en_flow_8822c
 * (TRANS_CARDDIS_TO_CARDEMU_8822C + TRANS_CARDEMU_TO_ACT_8822C), keeping the
 * USB/ALL-interface entries (PCI/SDIO-only steps dropped). Each step is a
 * read-modify-write or a poll on an 8-bit MAC register. */
enum PwrCmd { PC_WRITE, PC_POLL, PC_END };
struct PwrCfg { uint16_t off; uint8_t cmd; uint8_t msk; uint8_t val; };
constexpr uint8_t B(int n) { return static_cast<uint8_t>(1u << n); }

const PwrCfg kPwrOn8822cUsb[] = {
    /* --- card-disable -> card-emulation --- */
    {0x002E, PC_WRITE, B(2), B(2)},
    {0x002D, PC_WRITE, B(0), 0},
    {0x007F, PC_WRITE, B(7), 0},
    {0x004A, PC_WRITE, B(0), 0},
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(3) | B(4)), 0},
    /* --- card-emulation -> active --- */
    {0x0000, PC_WRITE, B(5), 0},
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(4) | B(3) | B(2)), 0},
    {0x0006, PC_POLL, B(1), B(1)},
    {0xFF1A, PC_WRITE, 0xFF, 0},
    {0x002E, PC_WRITE, B(3), 0},
    {0x0006, PC_WRITE, B(0), B(0)},
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(4) | B(3)), 0},
    {0x1018, PC_WRITE, B(2), B(2)},
    {0x0005, PC_WRITE, B(0), B(0)},
    {0x0005, PC_POLL, B(0), 0},
    {0x001F, PC_WRITE, static_cast<uint8_t>(B(7) | B(6)), B(7)},
    {0x00EF, PC_WRITE, static_cast<uint8_t>(B(7) | B(6)), B(7)},
    {0x1045, PC_WRITE, B(4), B(4)},
    {0x0010, PC_WRITE, B(2), B(2)},
    {0x1064, PC_WRITE, B(1), B(1)},
    {0, PC_END, 0, 0},
};
} /* namespace */

namespace {
/* RTL8822C USB power-OFF sequence (card_dis_flow_8822c = TRANS_ACT_TO_CARDEMU +
 * TRANS_CARDEMU_TO_CARDDIS, USB/ALL-interface entries). Run before power-on so
 * devourer resets the MAC from a kernel-left "active" state (where running only
 * the card-enable flow leaves RX dead). Polls are best-effort (non-fatal) so a
 * truly-cold chip — already off — isn't blocked. */
const PwrCfg kPwrOff8822cUsb[] = {
    /* --- active -> card-emulation --- */
    {0x0093, PC_WRITE, B(3), 0},
    {0x001F, PC_WRITE, 0xFF, 0},
    {0x00EF, PC_WRITE, 0xFF, 0},
    {0x1045, PC_WRITE, B(4), 0},
    {0xFF1A, PC_WRITE, 0xFF, 0x30},
    {0x0049, PC_WRITE, B(1), 0},
    {0x0006, PC_WRITE, B(0), B(0)},
    {0x0002, PC_WRITE, B(1), 0},
    {0x0005, PC_WRITE, B(1), B(1)},
    {0x0005, PC_POLL, B(1), 0},
    {0x0000, PC_WRITE, B(5), B(5)},
    /* --- card-emulation -> card-disable --- */
    {0x0007, PC_WRITE, 0xFF, 0x00},
    {0x0067, PC_WRITE, B(5), 0},
    {0x004A, PC_WRITE, B(0), 0},
    {0x0081, PC_WRITE, static_cast<uint8_t>(B(7) | B(6)), 0},
    {0x0090, PC_WRITE, B(1), 0},
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(3) | B(4)), B(3)},
    {0, PC_END, 0, 0},
};
} /* namespace */

void HalJaguar3::power_off() {
  for (const PwrCfg *p = kPwrOff8822cUsb; p->cmd != PC_END; ++p) {
    if (p->cmd == PC_WRITE) {
      uint8_t v = _device.rtw_read8(p->off);
      v = static_cast<uint8_t>((v & ~p->msk) | (p->val & p->msk));
      _device.rtw_write8(p->off, v);
    } else { /* PC_POLL — best-effort, don't wedge a cold chip */
      uint32_t cnt = 2000;
      while ((_device.rtw_read8(p->off) & p->msk) != (p->val & p->msk)) {
        if (--cnt == 0)
          break;
        std::this_thread::sleep_for(std::chrono::microseconds(10));
      }
    }
  }
  _logger->info("Jaguar3: power-off (card-disable) sequence applied");
}

void HalJaguar3::power_on() {
  /* Reset the MAC from any prior state (e.g. kernel-left active) first. */
  power_off();
  for (const PwrCfg *p = kPwrOn8822cUsb; p->cmd != PC_END; ++p) {
    if (p->cmd == PC_WRITE) {
      uint8_t v = _device.rtw_read8(p->off);
      v = static_cast<uint8_t>((v & ~p->msk) | (p->val & p->msk));
      _device.rtw_write8(p->off, v);
    } else { /* PC_POLL */
      uint32_t cnt = 5000;
      while ((_device.rtw_read8(p->off) & p->msk) != (p->val & p->msk)) {
        if (--cnt == 0)
          throw std::runtime_error(
              "Jaguar3: power-on poll timeout (chip not responding)");
        std::this_thread::sleep_for(std::chrono::microseconds(10));
      }
    }
  }
  _logger->info("Jaguar3: power-on sequence complete (card active)");
}

void HalJaguar3::apply_bb_rf_agc_tables() {
  /* BB + AGC baseline via the validated halbb walker (src/jaguar3/
   * PhyTableLoaderJaguar3). _phy_ctx must be populated from the chip-version +
   * EFUSE read (done earlier in rtw_hal_init) before the tables are walked. */
  auto bb = [this](uint32_t addr, uint32_t data) {
    write_bb(_device, addr, data);
  };
  const auto phy_reg = _tables->phy_reg();
  const auto agc_tab = _tables->agc_tab();
  _logger->info("Jaguar3: applying BB phy_reg table ({} words)", phy_reg.len);
  PhyTableLoaderJaguar3::Load(phy_reg.data, phy_reg.len, _phy_ctx, bb);
  _logger->info("Jaguar3: applying AGC table ({} words)", agc_tab.len);
  PhyTableLoaderJaguar3::Load(agc_tab.data, agc_tab.len, _phy_ctx, bb);

  /* RF radio tables. On 8822C an RF register write is a direct BB write to a
   * per-path window: BB[base + (rf_addr&0xff)*4], 20-bit mask, base 0x3c00
   * (path A) / 0x4c00 (path B) — config_phydm_direct_write_rf_reg_8822c. The
   * load is bracketed by phydm_rstb_3wire_8822c(false/true). */
  constexpr uint32_t RFREG_MASK = 0x000fffff;
  auto rf_writer = [this](uint16_t base) {
    return [this, base](uint32_t addr, uint32_t data) {
      switch (addr) {
      case 0xffe: std::this_thread::sleep_for(std::chrono::milliseconds(50)); return;
      case 0xfe:  std::this_thread::sleep_for(std::chrono::microseconds(100)); return;
      case 0xffff: std::this_thread::sleep_for(std::chrono::microseconds(1)); return;
      default:
        _device.phy_set_bb_reg(static_cast<uint16_t>(base + ((addr & 0xff) << 2)),
                               RFREG_MASK, data);
      }
    };
  };
  /* rstb_3wire(false) */
  const auto radioa = _tables->radioa();
  const auto radiob = _tables->radiob();
  _device.phy_set_bb_reg(0x1c90, 1u << 8, 0);
  _logger->info("Jaguar3: applying RF radioA ({} words) + radioB ({} words)",
                radioa.len, radiob.len);
  PhyTableLoaderJaguar3::Load(radioa.data, radioa.len, _phy_ctx, rf_writer(0x3c00));
  PhyTableLoaderJaguar3::Load(radiob.data, radiob.len, _phy_ctx, rf_writer(0x4c00));
  /* rstb_3wire(true) + force anapar update */
  _device.phy_set_bb_reg(0x1c90, 1u << 8, 1);
  _device.phy_set_bb_reg(0x1830, 1u << 29, 1);
  _device.phy_set_bb_reg(0x4130, 1u << 29, 1);
  _logger->info("Jaguar3: BB/AGC/RF tables applied");
}

/* RF-calibration (RFK) init end-state. The kernel rtw88_8822cu runs
 * halrf_rfk_init_8822c (a ~2500-line IQK/DPK/LCK procedure) which programs the
 * 0x1B00 block + a few RF gain regs; without it the RX path delivers nothing.
 * Here we write the captured end-state directly (same chip) — enough to bring
 * RX up; a full RFK port is future work. Most of the 0x1B00 block is static
 * config; the 0x3d08/0x4d08 gain regs are IQK-computed (chip-specific). */
void HalJaguar3::init_rfk() {
  /* odm_read_and_config_mp_8822c_cal_init (halrf_rfk_init_8822c.c): a flat
   * (addr,value) BB-write table (the 0x1B00 IQK/DPK RX-path block) preceded by
   * the iqk/dpk clock/reset/enable bits at 0x1cd0. NOT conditional. */
  _device.phy_set_bb_reg(0x1cd0, 1u << 28, 1); /* iqk_dpk clock src */
  _device.phy_set_bb_reg(0x1cd0, 1u << 29, 1); /* iqk_dpk reset src */
  _device.phy_set_bb_reg(0x1cd0, 1u << 30, 1); /* en IQK_dpk */
  _device.phy_set_bb_reg(0x1cd0, 1u << 31, 0);
  const auto cal_init = _tables->cal_init();
  for (uint32_t i = 0; i + 1 < cal_init.len; i += 2)
    write_bb(_device, cal_init.data[i], cal_init.data[i + 1]);

  /* The actual IQK gains are now produced by the ported halrf_iqk
   * (run_iqk), which runs after the channel is tuned — see
   * RtlJaguar3Device. No more hardcoded captured gains. */
  _logger->info("Jaguar3: cal_init ({} pairs) applied", cal_init.len / 2);
}

/* Port of rtw_fw_send_h2c_command: 4 HMEBOX mailboxes (msg at 0x1d0+box*4,
 * msg_ext at 0x1f0+box*4), round-robin. Poll REG_HMETFR (0x1cc) until the box's
 * bit clears (box consumed), then write msg_ext first, then msg. */
void HalJaguar3::send_h2c_raw(uint32_t msg, uint32_t msg_ext) {
  uint8_t box = _h2c_box & 0x3;
  uint16_t box_reg = static_cast<uint16_t>(0x1d0 + box * 4);
  uint16_t box_ex_reg = static_cast<uint16_t>(0x1f0 + box * 4);
  for (int i = 0; i < 30; ++i) { /* ~3 ms, mirrors the kernel's 100us*30 poll */
    if (((_device.rtw_read<uint8_t>(0x1cc) >> box) & 0x1) == 0)
      break;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  _device.rtw_write<uint32_t>(box_ex_reg, msg_ext);
  _device.rtw_write<uint32_t>(box_reg, msg);
  _h2c_box = static_cast<uint8_t>((_h2c_box + 1) & 0x3);
}

void HalJaguar3::fw_update_wl_phy_info() {
  /* H2C_CMD_WL_PHY_INFO (0x58). word0 = id[7:0] | tx_tp[17:8] | rx_tp[27:18];
   * word1 = tx_rate[7:0] | rx_rate[15:8] | rx_evm[23:16]. A non-zero tx_tp tells
   * the firmware WLAN is actively transmitting so it keeps the RF powered. */
  const uint32_t tx_tp = 100; /* Mbps — signal "WL busy" */
  uint32_t msg = 0x58u | ((tx_tp & 0x3ffu) << 8);
  send_h2c_raw(msg, 0);
}

void HalJaguar3::fw_set_pwr_mode_active() {
  /* word0: id[7:0]=0x20, mode[14:8]=0, rlbm[19:16]=0, smart_ps[23:20]=0,
   *        awake_interval[31:24]=1.
   * word1: port_id[7:5]=0, pwr_state[15:8]=RTW_ALL_ON(0xc). */
  uint32_t msg = 0x20u | (1u << 24);
  uint32_t ext = (0xcu << 8);
  send_h2c_raw(msg, ext);
}

} /* namespace jaguar3 */
