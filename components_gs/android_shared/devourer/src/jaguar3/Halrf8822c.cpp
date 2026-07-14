#include "Halrf8822c.h"

#include <chrono>
#include <memory>
#include <thread>

#if defined(DEVOURER_HAVE_JAGUAR3_8822C)
extern "C" {
#include "Hal8822c_IqkNctl.h"
}
#endif

namespace jaguar3 {

#if defined(DEVOURER_HAVE_JAGUAR3_8822C)
namespace {
/* IQK type indices (halrf_iqk_8822c.h: TXIQK/RXIQK; halrf_iqk.h: trigger steps) */
constexpr int TXIQK = 0;
constexpr int RXIQK = 1;
constexpr int TX_IQK = 0;  /* get_cfir idx */
constexpr int RX_IQK = 1;
constexpr int RXIQK1 = 1;  /* one_shot trigger step */
constexpr int RXIQK2 = 2;
constexpr int RXK_STEP = 6;       /* RXK_STEP_8822C */
constexpr int IQK_STEP = 7;       /* IQK_STEP_8822C */
constexpr int rxiqk_gs_limit = 6;
constexpr int kcount_limit_80m = 2;
constexpr int kcount_limit_others = 4;
const uint8_t IQMUX[5] = {0x9, 0x12, 0x1b, 0x24, 0x24};
/* RF-window bases for the 8822C direct RF access (config_phydm_*_rf_reg_8822c) */
constexpr uint16_t RF_WIN[2] = {0x3c00, 0x4c00};

inline uint32_t mask_shift(uint32_t mask) {
  if (mask == 0)
    return 0;
  uint32_t s = 0;
  while (((mask >> s) & 1u) == 0)
    s++;
  return s;
}
} /* namespace */

Halrf8822c::Halrf8822c(RtlAdapter device, Logger_t logger)
    : _device{device}, _logger{logger} {}
#endif /* DEVOURER_HAVE_JAGUAR3_8822C */

#if defined(DEVOURER_HAVE_JAGUAR3_8822E)
/* Defined in Halrf8822e.cpp. */
std::unique_ptr<Jaguar3Calibration>
make_halrf_8822e(RtlAdapter device, Logger_t logger,
                 const devourer::DeviceConfig &cfg);
#endif

/* Calibration strategy factory: pick the per-generation halrf impl. */
std::unique_ptr<Jaguar3Calibration>
make_jaguar3_calibration(ChipVariant variant, RtlAdapter device,
                         Logger_t logger, const devourer::DeviceConfig &cfg) {
#if defined(DEVOURER_HAVE_JAGUAR3_8822E)
  if (variant == ChipVariant::C8822E)
    return make_halrf_8822e(device, logger, cfg);
#endif
  (void)cfg; /* 8822c halrf has no config-driven knobs */
#if defined(DEVOURER_HAVE_JAGUAR3_8822C)
  return std::make_unique<Halrf8822c>(device, logger);
#else
  (void)variant;
  (void)device;
  (void)logger;
  return nullptr;
#endif
}

#if defined(DEVOURER_HAVE_JAGUAR3_8822C)
uint32_t Halrf8822c::bb_get(uint16_t addr, uint32_t mask) {
  return (_device.rtw_read32(addr) & mask) >> mask_shift(mask);
}

/* config_phydm_read_rf_reg_8822c: RF read is a direct BB-window read,
 * BB[base + (addr&0xff)<<2], 20-bit. */
uint32_t Halrf8822c::rf_read(uint8_t path, uint16_t addr, uint32_t mask) {
  uint16_t direct = static_cast<uint16_t>(RF_WIN[path & 1] + ((addr & 0xff) << 2));
  return bb_get(direct, mask & RFREG_MASK);
}

/* config_phydm_direct_write_rf_reg_8822c: RF write via the same window. */
void Halrf8822c::rf_write(uint8_t path, uint16_t addr, uint32_t mask,
                             uint32_t val) {
  uint16_t direct = static_cast<uint16_t>(RF_WIN[path & 1] + ((addr & 0xff) << 2));
  bb_set(direct, mask & RFREG_MASK, val);
}

/* _iqk_nctl_8822c — load the IQK calibration-engine microcode (generated table
 * from the vendor source). Each entry is a full-dword write (mask 0xffffffff) or
 * a masked BB RMW. */
void Halrf8822c::nctl() {
  for (unsigned i = 0; i < array_mp_8822c_iqk_nctl_len; i++) {
    const iqk_nctl_write &w = array_mp_8822c_iqk_nctl[i];
    if (w.mask == 0xFFFFFFFFu)
      bb_write(w.addr, w.val);
    else
      bb_set(w.addr, w.mask, w.val);
  }
}

/* _iqk_backup_mac_bb_8822c / _iqk_backup_rf_8822c */
void Halrf8822c::backup_mac_bb(uint32_t *mac, uint32_t *bb) {
  static const uint16_t macreg[MAC_REG_NUM_8822C] = {0x520, 0x1c, 0x70};
  static const uint16_t bbreg[BB_REG_NUM_8822C] = {
      0x0820, 0x0824, 0x1c38, 0x1c68, 0x1d60, 0x180c, 0x410c,
      0x1c3c, 0x1a14, 0x1d58, 0x1d70, 0x1864, 0x4164, 0x186c,
      0x416c, 0x1a14, 0x1e70, 0x080c, 0x1e7c, 0x18a4, 0x41a4};
  for (int i = 0; i < MAC_REG_NUM_8822C; i++)
    mac[i] = bb_read(macreg[i]);
  for (int i = 0; i < BB_REG_NUM_8822C; i++)
    bb[i] = bb_read(bbreg[i]);
}

void Halrf8822c::backup_rf(uint32_t rf[][2]) {
  static const uint16_t rfreg[RF_REG_NUM_8822C] = {0x19, 0xdf, 0x9e};
  for (int i = 0; i < RF_REG_NUM_8822C; i++) {
    rf[i][PATH_A] = rf_read(PATH_A, rfreg[i], RFREG_MASK);
    rf[i][PATH_B] = rf_read(PATH_B, rfreg[i], RFREG_MASK);
  }
}

/* _iqk_restore_mac_bb_8822c */
void Halrf8822c::restore_mac_bb(const uint32_t *mac, const uint32_t *bb) {
  static const uint16_t macreg[MAC_REG_NUM_8822C] = {0x520, 0x1c, 0x70};
  static const uint16_t bbreg[BB_REG_NUM_8822C] = {
      0x0820, 0x0824, 0x1c38, 0x1c68, 0x1d60, 0x180c, 0x410c,
      0x1c3c, 0x1a14, 0x1d58, 0x1d70, 0x1864, 0x4164, 0x186c,
      0x416c, 0x1a14, 0x1e70, 0x080c, 0x1e7c, 0x18a4, 0x41a4};
  bb_write(0x1d70, 0x50505050); /* toggle IGI */
  for (int i = 0; i < MAC_REG_NUM_8822C; i++)
    bb_write(macreg[i], mac[i]);
  for (int i = 0; i < BB_REG_NUM_8822C; i++)
    bb_write(bbreg[i], bb[i]);
  /* rx-go-through-IQK enable per RXIQK pass/fail */
  bb_set(0x180c, 1u << 31, _iqk.iqk_fail_report[0][PATH_A][RXIQK] ? 0 : 1);
  bb_set(0x410c, 1u << 31, _iqk.iqk_fail_report[0][PATH_B][RXIQK] ? 0 : 1);
}

/* _iqk_restore_rf_8822c */
void Halrf8822c::restore_rf(const uint32_t rf[][2]) {
  static const uint16_t rfreg[RF_REG_NUM_8822C] = {0x19, 0xdf, 0x9e};
  rf_write(PATH_A, 0xef, 0xfffff, 0x0);
  rf_write(PATH_B, 0xef, 0xfffff, 0x0);
  for (int i = 0; i < RF_REG_NUM_8822C; i++) {
    rf_write(PATH_A, rfreg[i], 0xfffff, rf[i][PATH_A]);
    rf_write(PATH_B, rfreg[i], 0xfffff, rf[i][PATH_B]);
  }
  rf_write(PATH_A, 0xde, 1u << 16, 0x0);
  rf_write(PATH_B, 0xde, 1u << 16, 0x0);
}

void Halrf8822c::delay_us(uint32_t us) {
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}
void Halrf8822c::delay_ms(uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/* _iqk_btc_wait_indirect_reg_ready_8822c: poll 0x1703[5] before 0x1700 access. */
uint32_t Halrf8822c::btc_wait_ready() {
  uint32_t cnt = 0;
  while ((mac_read8(0x1703) & (1u << 5)) == 0) {
    delay_ms(10);
    if (++cnt >= 10)
      break;
  }
  return cnt;
}

/* _iqk_btc_read_indirect_reg_8822c */
uint32_t Halrf8822c::btc_read_indirect(uint16_t reg) {
  btc_wait_ready();
  bb_write(0x1700, 0x800F0000u | reg);
  return bb_read(0x1708);
}

/* _iqk_btc_write_indirect_reg_8822c */
void Halrf8822c::btc_write_indirect(uint16_t reg, uint32_t mask, uint32_t val) {
  if (mask == 0)
    return;
  if (mask == 0xFFFFFFFFu) {
    btc_wait_ready();
    bb_write(0x1704, val);
    bb_write(0x1700, 0xc00F0000u | reg);
  } else {
    uint32_t bitpos = mask_shift(mask);
    uint32_t cur = btc_read_indirect(reg);
    cur = (cur & ~mask) | (val << bitpos);
    btc_wait_ready();
    bb_write(0x1704, cur);
    bb_write(0x1700, 0xc00F0000u | reg);
  }
}

/* _iqk_set_gnt_wl_high_8822c (GNT_WL high during cal) */
void Halrf8822c::set_gnt_wl_high() {
  btc_write_indirect(0x38, 0xff00, 0x77); /* 0x38[15:8] */
}

/* _iqk_set_gnt_wl_gnt_bt_8822c */
void Halrf8822c::set_gnt_wl_gnt_bt(bool before_k) {
  if (before_k)
    set_gnt_wl_high();
  else
    btc_write_indirect(0x38, 0xffffffff, _iqk.tmp_gntwl);
}

/* _iqk_check_cal_8822c: poll 0x2d9c==0x55 (ready), read fail from 0x1b08[26]
 * (LOK/cmd0 never fails); 300 ms timeout. */
bool Halrf8822c::check_cal(uint8_t path, uint8_t cmd) {
  bool notready = true, fail = true;
  uint32_t cnt = 0;
  while (notready) {
    if (mac_read8(0x2d9c) == 0x55) {
      fail = (cmd == 0) ? false : (bb_get(0x1b08, 1u << 26) != 0);
      notready = false;
    } else {
      delay_us(10);
      if (++cnt >= 30000) {
        fail = true;
        _logger->error("Jaguar3 IQK: check_cal timeout");
        break;
      }
    }
  }
  mac_write8(0x1b10, 0x0);
  delay_us(10);
  bb_write(0x1b00, 0x8u | (path << 1));
  bb_set(0x1b20, (1u << 26) | (1u << 25), fail ? 0x0 : 0x2);
  (void)fail;
  return false; /* vendor returns false here (result captured via 0x1b20) */
}

/* iqk_get_cfir_8822c (debug=false -> channel slot ch=0): read the 17 CFIR
 * coefficients (the IQK filter result) from the 0x1bd8/0x1bfc readback path. */
void Halrf8822c::get_cfir(uint8_t idx, uint8_t path) {
  constexpr uint32_t bit20_16 = (1u << 20) | (1u << 19) | (1u << 18) |
                                (1u << 17) | (1u << 16);
  bb_write(0x1b00, 0x8u | (path << 1));
  if (idx == TX_IQK)
    bb_set(0x1b20, (1u << 31) | (1u << 30), 0x3);
  else
    bb_set(0x1b20, (1u << 31) | (1u << 30), 0x1);
  bb_set(0x1bd4, 1u << 21, 0x1);
  bb_set(0x1bd4, bit20_16, 0x10);
  for (int i = 0; i <= 16; i++) {
    bb_set(0x1bd8, 0xFFFFFFFF, 0xe0000001u | (i << 2));
    uint32_t tmp = bb_get(0x1bfc, 0xFFFFFFFF);
    _iqk.iqk_cfir_real[0][path][idx][i] =
        static_cast<uint16_t>((tmp & 0x0fff0000) >> 16);
    _iqk.iqk_cfir_imag[0][path][idx][i] = static_cast<uint16_t>(tmp & 0x0fff);
  }
}

/* _iqk_backup_iqk_8822c: step 0 = rotate channel slot 0->1 + reset slot 0;
 * step 1 = backup LOK idac; step 2/3 = backup TX/RX CFIR. */
void Halrf8822c::backup_iqk(uint8_t step, uint8_t path) {
  auto &q = _iqk;
  switch (step) {
  case 0:
    q.iqk_channel[1] = q.iqk_channel[0];
    for (int i = 0; i < SS; i++) {
      q.lok_idac[1][i] = q.lok_idac[0][i];
      q.rxiqk_agc[1][i] = q.rxiqk_agc[0][i];
      q.bypass_iqk[1][i] = q.bypass_iqk[0][i];
      q.rxiqk_fail_code[1][i] = q.rxiqk_fail_code[0][i];
      for (int j = 0; j < 2; j++) {
        q.iqk_fail_report[1][i][j] = q.iqk_fail_report[0][i][j];
        for (int k = 0; k <= 16; k++) {
          q.iqk_cfir_real[1][i][j][k] = q.iqk_cfir_real[0][i][j][k];
          q.iqk_cfir_imag[1][i][j][k] = q.iqk_cfir_imag[0][i][j][k];
        }
      }
    }
    for (int i = 0; i < SS; i++) {
      q.rxiqk_fail_code[0][i] = 0;
      q.rxiqk_agc[0][i] = 0;
      for (int j = 0; j < 2; j++) {
        q.iqk_fail_report[0][i][j] = true;
        q.gs_retry_count[0][i][j] = 0;
      }
      for (int j = 0; j < 3; j++)
        q.retry_count[0][i][j] = 0;
    }
    q.iqk_channel[0] = q.rf_reg18;
    break;
  case 1: /* LOK */
    q.lok_idac[0][path] = rf_read(path, 0x58, RFREG_MASK);
    break;
  case 2: /* TXIQK CFIR */
    get_cfir(TX_IQK, path);
    break;
  case 3: /* RXIQK CFIR */
    get_cfir(RX_IQK, path);
    break;
  }
}

/* _iqk_one_shot_8822c: build the per-bandwidth IQK command, trigger via 0x1b00,
 * poll check_cal, capture the pass/fail + result. (cut-E special-cases dropped:
 * the WDN1300H is cut D.) */
bool Halrf8822c::one_shot(uint8_t path, uint8_t idx) {
  bool is_nb = _iqk.is_nb_iqk;
  uint32_t temp, iqk_cmd;

  set_gnt_wl_gnt_bt(true);

  if (idx == TXIQK) {
    temp = is_nb ? ((0x1u << 8) | (1u << (path + 4)) | (path << 1))
                 : (((_iqk.bw_val + 4u) << 8) | (1u << (path + 4)) | (path << 1));
    iqk_cmd = 0x8u | temp;
    delay_us(10);
  } else if (idx == RXIQK1) {
    temp = is_nb ? ((0x2u << 8) | (1u << (path + 4)) | (path << 1))
                 : (((_iqk.bw_val + 7u) << 8) | (1u << (path + 4)) | (path << 1));
    iqk_cmd = 0x8u | temp;
    delay_us(10);
  } else { /* RXIQK2 */
    temp = is_nb ? ((0x3u << 8) | (1u << (path + 4)) | (path << 1))
                 : (((_iqk.bw_val + 0xau) << 8) | (1u << (path + 4)) | (path << 1));
    iqk_cmd = 0x8u | temp;
    delay_us(10);
  }

  bb_write(0x1b00, iqk_cmd);
  bb_write(0x1b00, iqk_cmd + 0x1);
  bool fail = check_cal(path, 0x1);

  if (path == PATH_B)
    rf_write(PATH_B, 0x00, 0xf0000, 0x1);
  set_gnt_wl_gnt_bt(false);

  if (idx == TXIQK) {
    bb_write(0x1b00, 0x8u | (path << 1));
    _iqk.iqk_fail_report[0][path][TXIQK] = fail;
    if (!fail) {
      if (is_nb)
        _iqk.nbtxk_1b38[path] = bb_read(0x1b38);
      else
        backup_iqk(0x2, path);
    }
  }
  if (idx == RXIQK2) {
    bb_write(0x1b00, 0x8u | (path << 1));
    uint32_t t = rf_read(path, 0x0, 0xFFFFF) >> 5;
    t = (t & 0xff) | (_iqk.tmp1bcc << 8);
    _iqk.rxiqk_agc[0][path] = static_cast<uint16_t>(t);
    _iqk.iqk_fail_report[0][path][RXIQK] = fail;
    if (!fail) {
      if (is_nb)
        _iqk.nbrxk_1b3c[path] = bb_read(0x1b3c);
      else
        backup_iqk(0x3, path);
    }
  }
  return fail;
}

/* _iqk_cal_path_off_8822c */
void Halrf8822c::cal_path_off(uint8_t /*unused*/) {
  bb_set(0x1bb8, 1u << 20, 0x0);
  for (int p = 0; p < SS; p++) {
    bb_set(0x1b00, (1u << 2) | (1u << 1), p);
    bb_set(0x1bcc, 0x3f, 0x3f);
  }
}

/* _iqk_rf_direct_access_8822c (PI vs direct access on 0x1c / 0xec) */
void Halrf8822c::rf_direct_access(uint8_t path, bool direct) {
  uint16_t reg = (path == PATH_A) ? 0x1c : 0xec;
  bb_set(reg, (1u << 31) | (1u << 30), direct ? 0x2 : 0x0);
}

/* _iqk_lok_setting_8822c */
void Halrf8822c::lok_setting(uint8_t path, uint8_t idac_bs) {
  cal_path_off(path);
  bb_write(0x1b00, 0x8u | (path << 1));
  bb_set(0x1b20, (1u << 31) | (1u << 30), 0x0);
  bb_set(0x1b20, 0x3e0, 0x12); /* 12 dB */
  rf_write(path, 0xdf, 1u << 4, 0x0);
  rf_write(path, 0x9e, 1u << 5, 0x0);
  rf_write(path, 0x9e, 1u << 10, 0x0);
  if (!_iqk.is_5g) {
    rf_write(path, 0xde, 1u << 16, 0x1);
    rf_write(path, 0x56, 0xfff, 0x887);
    rf_write(path, 0xef, 1u << 2, 0x1);
    rf_write(path, 0x08, 0x70, idac_bs);
    rf_write(path, 0xef, 1u << 2, 0x0);
  } else {
    rf_write(path, 0xde, 1u << 16, 0x1);
    rf_write(path, 0x56, 0xfff, 0x868);
    rf_write(path, 0xef, 1u << 2, 0x1);
    rf_write(path, 0x33, 1u << 0, 0x0);
    rf_write(path, 0x08, 0x70, idac_bs);
    rf_write(path, 0xef, 1u << 2, 0x0);
  }
  rf_write(path, 0x57, 1u << 0, 0x0);
  /* TX_LOK */
  rf_write(path, 0xef, 1u << 4, 0x1);
  rf_write(path, 0x33, 0x7f, _iqk.is_5g ? 0x20 : 0x00);
  mac_write8(0x1bcc, 0x09);
  mac_write8(0x1b10, 0x0);
  bb_set(0x1b2c, 0xfff, _iqk.is_nb_iqk ? 0x08 : 0x38);
}

/* _lok_one_shot_8822c (cut-E special-cases dropped) */
bool Halrf8822c::lok_one_shot(uint8_t path, bool for_rxk) {
  set_gnt_wl_gnt_bt(true);
  uint32_t cmd = 0x8u | (1u << (4 + path)) | (path << 1);
  delay_us(10);
  rf_direct_access(path, false);
  bb_write(0x1b00, cmd);
  bb_write(0x1b00, cmd + 1);
  delay_us(2000);
  rf_direct_access(path, true);
  rf_write(path, 0xef, 1u << 4, 0x0);
  bool notready = check_cal(path, 0x0);
  if (path == PATH_B)
    rf_write(PATH_B, 0x00, 0xf0000, 0x1);
  set_gnt_wl_gnt_bt(false);
  if (!for_rxk)
    _iqk.rf_reg58 = rf_read(path, 0x58, 0xfffff);
  if (!notready)
    backup_iqk(0x1, path);
  _iqk.lok_fail[path] = notready;
  return notready;
}

/* _lok_check_8822c: idac in-range test on RF 0x58 */
bool Halrf8822c::lok_check(uint8_t path) {
  cal_path_off(path);
  bb_write(0x1b00, 0x8u | (path << 1));
  uint32_t t = rf_read(path, 0x58, 0xfffff);
  uint8_t idac_i = static_cast<uint8_t>((t & 0xfc000) >> 14);
  uint8_t idac_q = static_cast<uint8_t>((t & 0x03f00) >> 8);
  if (idac_i <= 0x3 || idac_i >= 0x3c || idac_q <= 0x3 || idac_q >= 0x3c)
    return false;
  return true;
}

/* _iqk_lok_tune_8822c: sweep idac_bs 4..6 until LOK passes */
void Halrf8822c::lok_tune(uint8_t path) {
  uint8_t idac_bs = 0x4;
  while (1) {
    lok_setting(path, idac_bs);
    lok_one_shot(path, false);
    if (!lok_check(path)) {
      if (idac_bs == 0x6)
        break;
      idac_bs++;
    } else {
      break;
    }
  }
}

/* _iqk_txk_setting_8822c. The 5 GHz path reads the efuse thermal meter to nudge
 * rf_0x56 by 1 LSB when the chip runs hotter than calibration; the logical efuse
 * thermal map isn't wired into this module yet, so we use the no-compensation
 * value (faithful when current thermal ~= cal thermal). TODO: thermal via
 * EepromManager. */
void Halrf8822c::txk_setting(uint8_t path) {
  bb_write(0x1b00, 0x8u | (path << 1));
  bb_set(0x1bb8, 1u << 20, 0x0);
  bb_write(0x1b20, 0x00040008);
  if (!_iqk.is_5g) {
    rf_write(path, 0x56, 0xfff, 0x887);
    mac_write8(0x1bcc, 0x09);
  } else {
    /* TANK */
    uint32_t rf64 = rf_read(path, 0x64, 0xFFFFF);
    rf64 = (rf64 & 0xfff0f) | 0x010;
    rf_write(path, 0xdf, 1u << 6, 0x1);
    rf_write(path, 0x64, 0xFFFFF, rf64);
    rf_write(path, 0x56, 0xfff, 0x8c6); /* no-thermal-compensation default */
    mac_write8(0x1bcc, 0x09);
  }
  bb_set(0x1b2c, 0xfff, _iqk.is_nb_iqk ? 0x08 : 0x38);
}

/* _iqk_lok_for_rxk_setting_8822c */
void Halrf8822c::lok_for_rxk_setting(uint8_t path) {
  cal_path_off(path);
  bb_write(0x1b00, 0x8u | (path << 1));
  bb_set(0x1bb8, 1u << 20, 0x0);
  bb_set(0x1b20, (1u << 31) | (1u << 30), 0x0);
  rf_write(path, 0x53, 1u << 0, 0x1); /* force PA on */
  if (!_iqk.is_5g) {
    rf_write(path, 0x00, 0xf0000, 0x7);
    rf_write(path, 0x9e, 1u << 5, 0x1);
    rf_write(path, 0x9e, 1u << 10, 0x1);
    bb_set(0x1b20, 0x3e0, 0x12);
    rf_write(path, 0xde, 1u << 16, 0x1);
    rf_write(path, 0x56, 0xfff, 020); /* octal 020 = 0x10, per source */
    rf_write(path, 0xef, 1u << 2, 0x1);
    rf_write(path, 0x33, 1u << 0, 0x0);
    rf_write(path, 0x08, 0x70, 0x4);
    rf_write(path, 0xef, 1u << 2, 0x0);
  } else {
    rf_write(path, 0x00, 0xf0000, 0x7);
    rf_write(path, 0x9e, 1u << 5, 0x1);
    rf_write(path, 0x9e, 1u << 10, 0x1);
    rf_write(path, 0xde, 1u << 16, 0x1);
    rf_write(path, 0x56, 0xfff, 0x000);
    rf_write(path, 0xef, 1u << 2, 0x1);
    rf_write(path, 0x33, 1u << 0, 0x1);
    rf_write(path, 0x08, 0x70, 0x4);
    rf_write(path, 0xef, 1u << 2, 0x0);
  }
  rf_write(path, 0x57, 1u << 0, 0x0);
  rf_write(path, 0xef, 1u << 4, 0x1);
  rf_write(path, 0x33, 0x7f, _iqk.is_5g ? 0x20 : 0x00);
  mac_write8(0x1bcc, 0x09);
  rf_write(path, 0xef, 1u << 4, 0x1);
  mac_write8(0x1b10, 0x0);
  mac_write8(0x1bcc, 0x12);
  bb_set(0x1b2c, 0xfff, _iqk.is_nb_iqk ? 0x008 : 0x038);
}

/* _iqk_rxk1_setting_8822c */
void Halrf8822c::rxk1_setting(uint8_t path) {
  cal_path_off(path);
  bb_write(0x1b00, 0x8u | (path << 1));
  bb_set(0x1bb8, 1u << 20, 0x0);
  bb_set(0x1b20, (1u << 31) | (1u << 30), 0x0);
  bb_set(0x1b20, 0x3e0, 0x12);
  rf_write(path, 0xde, 1u << 16, 0x1);
  rf_write(path, 0x56, 0xfff, _iqk.is_5g ? 0x000 : 0x020);
  mac_write8(0x1bcc, 0x12);
  bb_set(0x1b2c, 0xfff, _iqk.is_nb_iqk ? 0x008 : 0x038);
}

/* _iqk_rxk2_setting_8822c */
void Halrf8822c::rxk2_setting(uint8_t path, bool is_gs) {
  bb_write(0x1b00, 0x8u | (path << 1));
  bb_set(0x1b20, (1u << 31) | (1u << 30), 0x0);
  if (!_iqk.is_5g) {
    if (is_gs)
      _iqk.tmp1bcc = 0x12;
    mac_write8(0x1bcc, static_cast<uint8_t>(_iqk.tmp1bcc));
    rf_write(path, 0xde, 1u << 16, 0x1);
    rf_write(path, 0x56, 0xfff, 0x020);
    bb_set(0x1b18, 1u << 1, 0x1);
    bb_write(0x1b24, 0x00071808); /* LNA=0110, RXBB=00000 */
    mac_write8(0x1b10, 0x0);
  } else {
    if (is_gs)
      _iqk.tmp1bcc = 0x12;
    rf_write(path, 0xde, 1u << 16, 0x1);
    rf_write(path, 0x56, 0xfff, 0x000);
    mac_write8(0x1bcc, static_cast<uint8_t>(_iqk.tmp1bcc));
    bb_set(0x1b18, 1u << 1, 0x1);
    bb_write(0x1b24, 0x00070c08); /* LNA=011 */
    mac_write8(0x1b10, 0x0);
  }
  bb_set(0x1b2c, 0xfff, _iqk.is_nb_iqk ? 0x008 : 0x038);
}

/* _iqk_rx_iqk_gain_search_fail_8822c: RXK gain search via the IQMUX gain table,
 * stepping tmp1bcc up/down by the measured bb_idx until in range. */
bool Halrf8822c::gain_search_fail(uint8_t path, uint8_t step) {
  bool fail = true;
  uint32_t cmd, rf_reg0, tmp, bb_idx;
  uint8_t idx = 0;

  if (step == RXIQK1) {
    cmd = 0x208u | (1u << (path + 4)) | (path << 1);
    delay_us(10);
    bb_write(0x1b00, cmd);
    bb_write(0x1b00, cmd + 0x1);
    fail = check_cal(path, 0x1);
    return fail;
  }
  /* RXIQK2 */
  for (idx = 0; idx < 4; idx++)
    if (_iqk.tmp1bcc == IQMUX[idx])
      break;
  bb_write(0x1b00, 0x8u | (path << 1));
  bb_write(0x1bcc, _iqk.tmp1bcc);
  cmd = 0x308u | (1u << (path + 4)) | (path << 1);
  delay_us(10);
  bb_write(0x1b00, cmd);
  bb_write(0x1b00, cmd + 0x1);
  delay_us(20);
  rf_reg0 = rf_read(path, 0x0, 0xFFFFF);
  bool k2fail = check_cal(path, 0x1);
  if (k2fail) {
    _iqk.tmp1bcc = IQMUX[idx++ > 3 ? 4 : idx];
    return true;
  }
  bb_write(0x1b00, 0x00000008u | (path << 1));
  tmp = (rf_reg0 & 0x1fe0) >> 5;
  _iqk.lna_idx = static_cast<uint8_t>(tmp >> 5);
  bb_idx = tmp & 0x1f;
  if (bb_idx <= 0x1) {
    if (idx != 3) idx++; else _iqk.isbnd = true;
    fail = true;
  } else if (bb_idx >= 0xa) {
    if (idx != 0) idx--; else _iqk.isbnd = true;
    fail = true;
  } else {
    fail = false;
    _iqk.isbnd = false;
  }
  if (_iqk.isbnd)
    fail = false;
  _iqk.tmp1bcc = IQMUX[idx];
  if (!fail)
    bb_write(0x1be8, (static_cast<uint32_t>(_iqk.tmp1bcc) << 8) | bb_idx);
  return fail;
}

/* _iqk_rx_iqk_by_path_8822c: the RXK sub-step machine (LOK-for-RXK -> RXK1 ->
 * gain-search RXK2 -> RXK2). Steps 1 (GS-RXK1) and 5 (XYM check) are disabled in
 * the vendor source. Returns the per-step KFAIL. */
bool Halrf8822c::rx_iqk_by_path(uint8_t path) {
  bool kfail = false, gonext;
  auto &q = _iqk;
  switch (q.rxiqk_step) {
  case 0: /* LOK for RXK */
    lok_for_rxk_setting(path);
    lok_one_shot(path, true);
    q.rxiqk_step++;
    break;
  case 1: /* gain search RXK1 — disabled in vendor source */
    q.rxiqk_step++;
    break;
  case 2: /* RXK1 */
    rxk1_setting(path);
    gonext = false;
    while (1) {
      kfail = one_shot(path, RXIQK1);
      if (kfail && q.retry_count[0][path][RXIQK1] < 2) {
        q.retry_count[0][path][RXIQK1]++;
      } else if (kfail) {
        q.rxiqk_fail_code[0][path] = 1;
        q.rxiqk_step = RXK_STEP;
        gonext = true;
      } else {
        q.rxiqk_step++;
        gonext = true;
      }
      if (gonext)
        break;
    }
    break;
  case 3: /* gain search RXK2 */
    rxk2_setting(path, true);
    q.isbnd = false;
    while (1) {
      kfail = gain_search_fail(path, RXIQK2);
      if (kfail && q.gs_retry_count[0][path][1] < rxiqk_gs_limit)
        q.gs_retry_count[0][path][1]++;
      else {
        q.rxiqk_step++;
        break;
      }
    }
    break;
  case 4: /* RXK2 */
    rxk2_setting(path, false);
    gonext = false;
    while (1) {
      kfail = one_shot(path, RXIQK2);
      if (kfail && q.retry_count[0][path][RXIQK2] < 2) {
        q.retry_count[0][path][RXIQK2]++;
      } else if (kfail) {
        q.rxiqk_fail_code[0][path] = 2;
        q.rxiqk_step = RXK_STEP;
        gonext = true;
      } else {
        q.rxiqk_step++;
        gonext = true;
      }
      if (gonext)
        break;
    }
    break;
  case 5: /* check RX XYM — disabled in vendor source */
    q.rxiqk_step++;
    break;
  }
  return kfail;
}

/* _iqk_iqk_by_path_8822c: top step machine — S0(RXK,LOK,TXK) then S1(RXK,LOK,
 * TXK), then finalize. Each invocation advances one step (segment IQK yields
 * between steps). */
void Halrf8822c::iqk_by_path(bool /*segment*/) {
  auto &q = _iqk;
  bool kfail;
  uint32_t counter;
  switch (q.iqk_step) {
  case 0: /* S0 RXIQK */
    counter = 0;
    while (1) {
      counter++;
      kfail = rx_iqk_by_path(PATH_A);
      if (!kfail && q.rxiqk_step == RXK_STEP) {
        q.iqk_step++;
        q.rxiqk_step = 0;
        break;
      }
      if (counter > 60 && q.rxiqk_step == 0) {
        q.iqk_step++;
        break;
      }
    }
    q.kcount++;
    break;
  case 1: /* S0 LOK */
    lok_tune(PATH_A);
    q.iqk_step++;
    break;
  case 2: /* S0 TXIQK */
    txk_setting(PATH_A);
    one_shot(PATH_A, TXIQK);
    rf_write(PATH_A, 0xef, 1u << 4, 0x0);
    q.kcount++;
    q.iqk_step++;
    break;
  case 3: /* S1 RXIQK */
    counter = 0;
    while (1) {
      counter++;
      kfail = rx_iqk_by_path(PATH_B);
      if (!kfail && q.rxiqk_step == RXK_STEP) {
        q.iqk_step++;
        q.rxiqk_step = 0;
        break;
      }
      if (counter > 60 && q.rxiqk_step == 0) {
        q.iqk_step++;
        break;
      }
    }
    q.kcount++;
    break;
  case 4: /* S1 LOK */
    lok_tune(PATH_B);
    q.iqk_step++;
    break;
  case 5: /* S1 TXIQK */
    txk_setting(PATH_B);
    kfail = one_shot(PATH_B, TXIQK);
    rf_write(PATH_B, 0xef, 1u << 4, 0x0);
    q.kcount++;
    if (kfail && q.retry_count[0][PATH_B][TXIQK] < 3)
      q.retry_count[0][PATH_B][TXIQK]++;
    else
      q.iqk_step++;
    break;
  case 6: /* IDFT — disabled in vendor source */
    q.iqk_step++;
    break;
  }

  if (q.iqk_step == IQK_STEP) {
    for (int path = 0; path < SS; path++) {
      bb_write(0x1b00, 0x8u | (path << 1));
      bb_set(0x1bb8, 1u << 20, 0x0);
      bb_set(0x1bcc, 0xff, 0x0);
      if (q.is_nb_iqk) {
        bb_set(0x1b20, 1u << 26, 0x0);
        bb_write(0x1b38, q.nbtxk_1b38[path]);
        bb_write(0x1b3c, q.nbrxk_1b3c[path]);
      } else {
        bb_set(0x1b20, 1u << 26, 0x1);
        bb_write(0x1b38, 0x40000000);
        bb_write(0x1b3c, 0x40000000);
      }
      rf_write(path, 0x0, 0xf0000, 0x3); /* force RX mode */
    }
  }
}

/* _iqk_start_iqk_8822c: drive iqk_by_path until the step machine completes (or
 * the segment kcount limit is hit). */
void Halrf8822c::start_iqk(bool segment) {
  uint8_t kcount_limit =
      (_iqk.bw_val == 2) ? kcount_limit_80m : kcount_limit_others;
  uint8_t i = 0;
  while (i < 100) {
    iqk_by_path(segment);
    if (_iqk.iqk_step == IQK_STEP)
      break;
    if (segment && _iqk.kcount == kcount_limit)
      break;
    i++;
  }
}

/* _iqk_information_8822c */
void Halrf8822c::information() {
  _iqk.is_tssi_mode = bb_get(0x1e7c, 1u << 30) != 0;
  uint32_t r18 = rf_read(PATH_A, 0x18, RFREG_MASK);
  _iqk.iqk_band = static_cast<uint8_t>((r18 & (1u << 16)) >> 16);
  _iqk.iqk_ch = static_cast<uint8_t>(r18 & 0xff);
  _iqk.iqk_bw = static_cast<uint8_t>((r18 & 0x3000) >> 12);
}

/* _iqk_macbb_8822c */
void Halrf8822c::macbb() {
  if (_iqk.is_tssi_mode) {
    bb_set(0x1e7c, 1u << 30, 0x0);
    bb_set(0x18a4, 1u << 28, 0x0);
    bb_set(0x41a4, 1u << 28, 0x0);
  }
  mac_write8(0x0522, 0xff); /* REG_TXPAUSE */
  bb_set(0x70, 0xff000000, 0x06);
  bb_set(0x1e24, 1u << 17, 0x1);
  bb_set(0x1cd0, (1u << 30) | (1u << 29) | (1u << 28), 0x7);
  bb_set(0x1d60, 1u << 31, 0x1);
  bb_write(0x1c38, 0xffffffff);
  bb_set(0x1a14, 0x300, 0x3);
  bb_set(0x824, 0x30000, 0x3); /* rx path on */
}

/* _iqk_bb_for_dpk_setting_8822c */
void Halrf8822c::bb_for_dpk_setting() {
  bb_set(0x1e24, 1u << 17, 0x1);
  bb_set(0x1cd0, 1u << 28, 0x1);
  bb_set(0x1cd0, 1u << 29, 0x1);
  bb_set(0x1cd0, 1u << 30, 0x1);
  bb_set(0x1cd0, 1u << 31, 0x0);
  bb_set(0x1d58, 0xff8, 0x1ff);
  bb_set(0x1864, 1u << 31, 0x1);
  bb_set(0x4164, 1u << 31, 0x1);
  bb_set(0x180c, 1u << 27, 0x1);
  bb_set(0x410c, 1u << 27, 0x1);
  bb_set(0x186c, 1u << 7, 0x1);
  bb_set(0x416c, 1u << 7, 0x1);
  bb_set(0x180c, 0x3, 0x0);
  bb_set(0x410c, 0x3, 0x0);
  bb_set(0x1a00, (1u << 1) | (1u << 0), 0x2);
}

/* _iqk_afe_setting_8822c */
void Halrf8822c::afe_setting(bool do_iqk) {
  auto ramp = [&](uint16_t reg) {
    bb_write(reg, 0x700f0001);
    for (uint32_t n = 0; n < 16; n++)
      bb_write(reg, 0x700f0001u | (n << 20));
    bb_write(reg, 0x70ff0001);
  };
  if (do_iqk) {
    ramp(0x1830);
    ramp(0x4130);
    bb_write(0x1c38, 0x0);
    delay_us(10);
    bb_write(0x1c38, 0xffffffff);
    return;
  }
  if (_iqk.is_tssi_mode) {
    uint32_t b = 0x4u >> _iqk.iqk_band;
    bb_write(0x1c38, 0xf7d5005e);
    bb_set(0x1860, 0x00007000, b);
    const uint32_t a30[] = {0x700b8041, 0x701f0040u | b, 0x702f0040u | b,
                            0x703f0040u | b, 0x704f0040u | b, 0x705b8041,
                            0x706f0040u | b};
    for (uint32_t v : a30) bb_write(0x1830, v);
    for (uint32_t v : a30) bb_write(0x4130, v);
  } else {
    static const uint32_t restore[16] = {
        0x700b8041, 0x70144041, 0x70244041, 0x70344041, 0x70444041, 0x705b8041,
        0x70644041, 0x707b8041, 0x708b8041, 0x709b8041, 0x70ab8041, 0x70bb8041,
        0x70cb8041, 0x70db8041, 0x70eb8041, 0x70fb8041};
    for (uint32_t v : restore) bb_write(0x1830, v);
    for (uint32_t v : restore) bb_write(0x4130, v);
  }
  /* BB-for-DPK restore */
  bb_set(0x1bb8, 1u << 20, 0x0);
  bb_set(0x1bcc, 0x000000ff, 0x0);
  bb_set(0x1d58, 0xff8, 0x0);
  bb_set(0x1864, 1u << 31, 0x0);
  bb_set(0x4164, 1u << 31, 0x0);
  bb_set(0x180c, 1u << 27, 0x0);
  bb_set(0x410c, 1u << 27, 0x0);
  bb_set(0x186c, 1u << 7, 0x0);
  bb_set(0x416c, 1u << 7, 0x0);
  bb_set(0x180c, (1u << 1) | (1u << 0), 0x3);
  bb_set(0x410c, (1u << 1) | (1u << 0), 0x3);
  bb_set(0x1a00, (1u << 1) | (1u << 0), 0x0);
}

/* _iqk_fill_iqk_report_8822c: latch per-path fail bits + rxiqk_agc to HW. */
void Halrf8822c::fill_report(uint8_t ch) {
  for (int i = 0; i < SS; i++) {
    uint32_t t1 = (_iqk.iqk_fail_report[ch][i][TX_IQK] & 1u) << i;
    uint32_t t2 = (_iqk.iqk_fail_report[ch][i][RX_IQK] & 1u) << (i + 4);
    uint32_t t3 = (_iqk.rxiqk_fail_code[ch][i] & 0x3u) << (i * 2 + 8);
    bb_write(0x1b00, 0x8u | (i << 1));
    bb_set(0x1bf0, 0x0000ffff, t1 | t2 | t3);
    bb_write(0x1be8, _iqk.rxiqk_agc[ch][i]);
  }
}

/* phy_iq_calibrate_8822c -> _phy_iq_calibrate_8822c (non-segment). Full IQK:
 * backup -> (macbb/dpk/afe/start_iqk/afe/restore) up to 3x -> fill report. */
void Halrf8822c::phy_iq_calibrate(ChannelWidth_t bw, uint8_t channel) {
  _iqk.is_nb_iqk = (bw == CHANNEL_WIDTH_5 || bw == CHANNEL_WIDTH_10);
  _iqk.bw_val = _iqk.is_nb_iqk ? 0 : static_cast<uint8_t>(bw); /* 0/1/2=20/40/80 */
  _iqk.is_5g = (channel > 14); /* incl. below-band 15..35 extended-synth */

  uint32_t mac[MAC_REG_NUM_8822C], bbk[BB_REG_NUM_8822C], rf[RF_REG_NUM_8822C][2];

  _iqk.rf_reg18 = rf_read(PATH_A, 0x18, RFREG_MASK);
  _iqk.iqk_times++;
  _iqk.kcount = 0;
  _iqk.iqk_step = 0;
  _iqk.rxiqk_step = 0;
  _iqk.tmp_gntwl = btc_read_indirect(0x38);

  information();
  backup_iqk(0x0, 0x0);
  backup_mac_bb(mac, bbk);
  backup_rf(rf);

  for (int i = 0; i < 3; i++) {
    macbb();
    bb_for_dpk_setting();
    afe_setting(true);
    /* agc_bnd_int is a no-op in the vendor source */
    nctl();
    start_iqk(false);
    afe_setting(false);
    restore_rf(rf);
    restore_mac_bb(mac, bbk);
    if (_iqk.iqk_step == 7 /*IQK_STEP*/)
      break;
    _iqk.kcount = 0;
    delay_ms(5);
  }
  fill_report(0);
  _logger->info("Jaguar3: IQK done (LOK fail A/B={}/{}, TXK A/B={}/{}, "
                "RXK A/B={}/{})",
                _iqk.lok_fail[0], _iqk.lok_fail[1],
                _iqk.iqk_fail_report[0][0][TX_IQK],
                _iqk.iqk_fail_report[0][1][TX_IQK],
                _iqk.iqk_fail_report[0][0][RX_IQK],
                _iqk.iqk_fail_report[0][1][RX_IQK]);
}

/* ---- DAC calibration (halrf_dac_cal_8822c) ----------------------------------
 * Per-boot, channel-independent. Produces the operational BB DC/gain state that
 * gates strong on-air TX. Ported from the vendor sequence; the warm
 * backup/restore (dack_en/dack_restore) is omitted (devourer calibrates fresh). */
namespace {
constexpr int DACK_SN = 100; /* halrf.h: #define SN 100 */

bool dack_compare(uint32_t v) {
  if (v >= 0x200 && (0x400 - v) > 0x64) return true;
  if (v < 0x200 && v > 0x64) return true;
  return false;
}
void dack_minmax(uint32_t v, uint32_t *mn, uint32_t *mx) {
  if (v >= 0x200) {
    if (*mn >= 0x200) { if (*mn > v) *mn = v; } else { *mn = v; }
    if (*mx >= 0x200) { if (*mx < v) *mx = v; }
  } else {
    if (*mn < 0x200) { if (*mn > v) *mn = v; }
    if (*mx >= 0x200) { *mx = v; } else { if (*mx < v) *mx = v; }
  }
}
void dack_swap(uint32_t *a, uint32_t *b) { uint32_t t = *a; *a = *b; *b = t; }
void dack_bubble(uint32_t *v1, uint32_t *v2) {
  if (*v1 >= 0x200 && *v2 >= 0x200) { if (*v1 > *v2) dack_swap(v1, v2); }
  else if (*v1 < 0x200 && *v2 < 0x200) { if (*v1 > *v2) dack_swap(v1, v2); }
  else if (*v1 < 0x200 && *v2 >= 0x200) { dack_swap(v1, v2); }
}
void dack_bsort(uint32_t *iv, uint32_t *qv, int n) {
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - 1 - i; j++) {
      dack_bubble(&iv[j], &iv[j + 1]);
      dack_bubble(&qv[j], &qv[j + 1]);
    }
}
} /* namespace */

void Halrf8822c::dack_poll(uint16_t add, uint32_t bmask, uint32_t data) {
  for (uint32_t c = 0; c < 100000; c++)
    if (bb_get(add, bmask) == data) break;
}

/* halrf_mode_8822c: sample the BB DC report (0x2dbc) SN times, drop outliers,
 * average the central window -> {i,q} DC offset estimate. */
void Halrf8822c::dack_mode(uint32_t *i_value, uint32_t *q_value) {
  uint32_t iv[DACK_SN] = {}, qv[DACK_SN] = {}, temp;
  int i = 0;
  for (uint32_t c = 0; i < DACK_SN && c < 10000; c++) {
    temp = bb_get(0x2dbc, 0x3fffff);
    iv[i] = (temp & 0x3ff000) >> 12;
    qv[i] = temp & 0x3ff;
    if (!(dack_compare(iv[i]) || dack_compare(qv[i]))) i++;
  }
  /* If the BB DC report never settled we exit with i<DACK_SN; the arrays are
   * zero-initialised so the averaging below stays well-defined, but warn since
   * the DACK estimate will be skewed. */
  if (i < DACK_SN)
    _logger->info("Jaguar3: DACK sample loop timed out ({}/{} settled)", i,
                  DACK_SN);
  for (uint32_t c = 0; c < 100; c++) {
    uint32_t i_mn = iv[0], i_mx = iv[0], q_mn = qv[0], q_mx = qv[0];
    for (int k = 0; k < DACK_SN; k++) {
      dack_minmax(iv[k], &i_mn, &i_mx);
      dack_minmax(qv[k], &q_mn, &q_mx);
    }
    uint32_t i_d = (i_mx < 0x200 && i_mn < 0x200) || (i_mx >= 0x200 && i_mn >= 0x200)
                       ? i_mx - i_mn : i_mx + (0x400 - i_mn);
    uint32_t q_d = (q_mx < 0x200 && q_mn < 0x200) || (q_mx >= 0x200 && q_mn >= 0x200)
                       ? q_mx - q_mn : q_mx + (0x400 - q_mn);
    dack_bsort(iv, qv, DACK_SN);
    if (i_d > 5 || q_d > 5) {
      temp = bb_get(0x2dbc, 0x3fffff); iv[0] = (temp & 0x3ff000) >> 12; qv[0] = temp & 0x3ff;
      temp = bb_get(0x2dbc, 0x3fffff); iv[DACK_SN-1] = (temp & 0x3ff000) >> 12; qv[DACK_SN-1] = temp & 0x3ff;
    } else break;
  }
  uint32_t m, p, t;
  m = p = 0;
  for (int k = 10; k < DACK_SN - 10; k++) { if (iv[k] > 0x200) m += 0x400 - iv[k]; else p += iv[k]; }
  if (p > m) t = (p - m) / (DACK_SN - 20);
  else { t = (m - p) / (DACK_SN - 20); if (t) t = 0x400 - t; }
  *i_value = t;
  m = p = 0;
  for (int k = 10; k < DACK_SN - 10; k++) { if (qv[k] > 0x200) m += 0x400 - qv[k]; else p += qv[k]; }
  if (p > m) t = (p - m) / (DACK_SN - 20);
  else { t = (m - p) / (DACK_SN - 20); if (t) t = 0x400 - t; }
  *q_value = t;
}

void Halrf8822c::dac_calibrate() {
  static const uint16_t bp_reg[16] = {0x180c,0x1810,0x410c,0x4110,0x1c3c,0x1c24,
      0x1d70,0x09b4,0x1a00,0x1a14,0x1d58,0x1c38,0x1e24,0x1e28,0x1860,0x4160};
  uint32_t bp[16], bprf[2];
  for (int i = 0; i < 16; i++) bp[i] = bb_read(bp_reg[i]);
  bprf[0] = rf_read(PATH_A, 0x8f, RFREG_MASK);
  bprf[1] = rf_read(PATH_B, 0x8f, RFREG_MASK);

  uint32_t ic = 0, qc = 0, temp = 0, temp1;
  uint32_t adc_ic_a = 0, adc_qc_a = 0, adc_ic_b = 0, adc_qc_b = 0;

  bb_set(0x1d58, 0xff8, 0x1ff);
  bb_set(0x1a00, 0x3, 0x2);
  bb_set(0x1a14, 0x300, 0x3);
  bb_write(0x1d70, 0x7e7e7e7e);
  bb_set(0x180c, 0x3, 0x0);
  bb_set(0x410c, 0x3, 0x0);
  bb_write(0x1b00, 0x00000008); mac_write8(0x1bcc, 0x3f);
  bb_write(0x1b00, 0x0000000a); mac_write8(0x1bcc, 0x3f);
  bb_set(0x1e24, 1u<<31, 0x0);
  bb_set(0x1e28, 0xf, 0x3);

  /* ---- path A ---- */
  bb_set(0x1830, 1u<<30, 0x0);
  bb_write(0x1860, 0xf0040ff0);
  bb_write(0x180c, 0xdff00220);
  bb_write(0x1810, 0x02dd08c4);
  bb_write(0x180c, 0x10000260);
  rf_write(PATH_A, 0x0, RFREG_MASK, 0x10000);
  rf_write(PATH_B, 0x0, RFREG_MASK, 0x10000);
  for (int i = 0; i < 10; i++) {
    bb_write(0x1c3c, 0x00088003); bb_write(0x1c24, 0x00010002);
    dack_mode(&ic, &qc);
    if (ic) { ic = 0x400 - ic; adc_ic_a = ic; }
    if (qc) { qc = 0x400 - qc; adc_qc_a = qc; }
    temp = (ic & 0x3ff) | ((qc & 0x3ff) << 10);
    bb_write(0x1868, temp);
    bb_write(0x1c3c, 0x00088103);
    dack_mode(&ic, &qc);
    if (ic >= 0x200) ic = 0x400 - ic;
    if (qc >= 0x200) qc = 0x400 - qc;
    if (ic < 5 && qc < 5) break;
  }
  bb_write(0x1c3c, 0x00000003);
  bb_write(0x180c, 0x10000260);
  bb_write(0x1810, 0x02d508c4);
  rf_write(PATH_A, 0x8f, 1u<<13, 0x1);
  for (int i = 0; i < 10; i++) {
    bb_write(0x1868, temp);
    bb_write(0x180c, 0xdff00220); bb_write(0x1860, 0xf0040ff0);
    bb_write(0x1c38, 0xffffffff); bb_write(0x1810, 0x02d508c5);
    bb_write(0x09b4, 0xdb66db00);
    bb_write(0x18b0, 0x0a11fb88); bb_write(0x18bc, 0x0008ff81); bb_write(0x18c0, 0x0003d208);
    bb_write(0x18cc, 0x0a11fb88); bb_write(0x18d8, 0x0008ff81); bb_write(0x18dc, 0x0003d208);
    bb_write(0x18b8, 0x60000000); delay_ms(2);
    bb_write(0x18bc, 0x000aff8d); delay_ms(2);
    bb_write(0x18b0, 0x0a11fb89); bb_write(0x18cc, 0x0a11fb89); delay_ms(1);
    bb_write(0x18b8, 0x62000000); bb_write(0x18d4, 0x62000000); delay_ms(1);
    dack_poll(0x2808, 0x7fff80, 0xffff); dack_poll(0x2834, 0x7fff80, 0xffff);
    bb_write(0x18b8, 0x02000000); delay_ms(1);
    bb_write(0x18bc, 0x0008ff87); bb_write(0x09b4, 0xdb6db600);
    bb_write(0x1810, 0x02d508c5); bb_write(0x18bc, 0x0008ff87); bb_write(0x1860, 0xf0000000);
    bb_set(0x18bc, 0xf0000000, 0x0); bb_set(0x18c0, 0xf, 0x8);
    bb_set(0x18d8, 0xf0000000, 0x0); bb_set(0x18dc, 0xf, 0x8);
    bb_write(0x1b00, 0x00000008); mac_write8(0x1bcc, 0x3f);
    bb_write(0x180c, 0xdff00220); bb_write(0x1810, 0x02d508c5);
    bb_write(0x1c3c, 0x00088103);
    dack_mode(&ic, &qc);
    if (ic) ic = 0x400 - ic;
    if (qc) qc = 0x400 - qc;
    if (ic < 0x300) { ic = ic * 2 * 6 / 5; ic += 0x80; } else { ic = (0x400 - ic) * 2 * 6 / 5; ic = 0x7f - ic; }
    if (qc < 0x300) { qc = qc * 2 * 6 / 5; qc += 0x80; } else { qc = (0x400 - qc) * 2 * 6 / 5; qc = 0x7f - qc; }
    bb_write(0x180c, 0xdff00220); bb_write(0x1810, 0x02d508c5); bb_write(0x09b4, 0xdb66db00);
    bb_write(0x18b0, 0x0a11fb88); bb_write(0x18bc, 0xc008ff81); bb_write(0x18c0, 0x0003d208);
    bb_set(0x18bc, 0xf0000000, ic & 0xf); bb_set(0x18c0, 0xf, (ic & 0xf0) >> 4);
    bb_write(0x18cc, 0x0a11fb88); bb_write(0x18d8, 0xe008ff81); bb_write(0x18dc, 0x0003d208);
    bb_set(0x18d8, 0xf0000000, qc & 0xf); bb_set(0x18dc, 0xf, (qc & 0xf0) >> 4);
    bb_write(0x18b8, 0x60000000); delay_ms(2);
    bb_set(0x18bc, 0xe, 0x6); delay_ms(2);
    bb_write(0x18b0, 0x0a11fb89); bb_write(0x18cc, 0x0a11fb89); delay_ms(1);
    bb_write(0x18b8, 0x62000000); bb_write(0x18d4, 0x62000000); delay_ms(1);
    dack_poll(0x2824, 0x07f80000, ic); dack_poll(0x2850, 0x07f80000, qc);
    bb_write(0x18b8, 0x02000000); delay_ms(1);
    bb_set(0x18bc, 0xe, 0x3); bb_write(0x09b4, 0xdb6db600);
    temp1 = ((adc_ic_a + 0x10) & 0x3ff) | (((adc_qc_a + 0x10) & 0x3ff) << 10);
    bb_write(0x1868, temp1);
    bb_write(0x1810, 0x02d508c5); bb_write(0x1860, 0xf0000000);
    dack_mode(&ic, &qc);
    ic = (ic >= 0x10) ? ic - 0x10 : 0x400 - (0x10 - ic);
    qc = (qc >= 0x10) ? qc - 0x10 : 0x400 - (0x10 - qc);
    if (ic >= 0x200) ic = 0x400 - ic;
    if (qc >= 0x200) qc = 0x400 - qc;
    if (ic < 5 && qc < 5) break;
  }
  bb_write(0x1868, 0x0); bb_write(0x1810, 0x02d508c4);
  bb_set(0x18bc, 0x1, 0x0); bb_set(0x1830, 1u<<30, 0x1);

  /* ---- path B ---- */
  bb_set(0x4130, 1u<<30, 0x0);
  bb_write(0x4130, 0x30db8041); bb_write(0x4160, 0xf0040ff0);
  bb_write(0x410c, 0xdff00220); bb_write(0x4110, 0x02dd08c4); bb_write(0x410c, 0x10000260);
  rf_write(PATH_A, 0x0, RFREG_MASK, 0x10000);
  rf_write(PATH_B, 0x0, RFREG_MASK, 0x10000);
  for (int i = 0; i < 10; i++) {
    bb_write(0x1c3c, 0x000a8003); bb_write(0x1c24, 0x00010002);
    dack_mode(&ic, &qc);
    if (ic) { ic = 0x400 - ic; adc_ic_b = ic; }
    if (qc) { qc = 0x400 - qc; adc_qc_b = qc; }
    temp = (ic & 0x3ff) | ((qc & 0x3ff) << 10);
    bb_write(0x4168, temp);
    bb_write(0x1c3c, 0x000a8103);
    dack_mode(&ic, &qc);
    if (ic >= 0x200) ic = 0x400 - ic;
    if (qc >= 0x200) qc = 0x400 - qc;
    if (ic < 5 && qc < 5) break;
  }
  bb_write(0x1c3c, 0x00000003);
  bb_write(0x410c, 0x10000260); bb_write(0x4110, 0x02d508c4);
  rf_write(PATH_B, 0x8f, 1u<<13, 0x1);
  for (int i = 0; i < 10; i++) {
    bb_write(0x4168, temp);
    bb_write(0x410c, 0xdff00220); bb_write(0x4110, 0x02d508c5); bb_write(0x09b4, 0xdb66db00);
    bb_write(0x41b0, 0x0a11fb88); bb_write(0x41bc, 0x0008ff81); bb_write(0x41c0, 0x0003d208);
    bb_write(0x41cc, 0x0a11fb88); bb_write(0x41d8, 0x0008ff81); bb_write(0x41dc, 0x0003d208);
    bb_write(0x41b8, 0x60000000); delay_ms(2);
    bb_write(0x41bc, 0x000aff8d); delay_ms(2);
    bb_write(0x41b0, 0x0a11fb89); bb_write(0x41cc, 0x0a11fb89); delay_ms(1);
    bb_write(0x41b8, 0x62000000); bb_write(0x41d4, 0x62000000); delay_ms(1);
    dack_poll(0x4508, 0x7fff80, 0xffff); dack_poll(0x4534, 0x7fff80, 0xffff);
    bb_write(0x41b8, 0x02000000); delay_ms(1);
    bb_write(0x41bc, 0x0008ff87); bb_write(0x09b4, 0xdb6db600);
    bb_write(0x4110, 0x02d508c5); bb_write(0x41bc, 0x0008ff87); bb_write(0x4160, 0xf0000000);
    bb_set(0x41bc, 0xf0000000, 0x0); bb_set(0x41c0, 0xf, 0x8);
    bb_set(0x41d8, 0xf0000000, 0x0); bb_set(0x41dc, 0xf, 0x8);
    bb_write(0x1b00, 0x0000000a); mac_write8(0x1bcc, 0x3f);
    bb_write(0x410c, 0xdff00220); bb_write(0x4110, 0x02d508c5);
    bb_write(0x1c3c, 0x000a8103);
    dack_mode(&ic, &qc);
    if (ic) ic = 0x400 - ic;
    if (qc) qc = 0x400 - qc;
    if (ic < 0x300) { ic = ic * 2 * 6 / 5; ic += 0x80; } else { ic = (0x400 - ic) * 2 * 6 / 5; ic = 0x7f - ic; }
    if (qc < 0x300) { qc = qc * 2 * 6 / 5; qc += 0x80; } else { qc = (0x400 - qc) * 2 * 6 / 5; qc = 0x7f - qc; }
    bb_write(0x410c, 0xdff00220); bb_write(0x4110, 0x02d508c5); bb_write(0x09b4, 0xdb66db00);
    bb_write(0x41b0, 0x0a11fb88); bb_write(0x41bc, 0xc008ff81); bb_write(0x41c0, 0x0003d208);
    bb_set(0x41bc, 0xf0000000, ic & 0xf); bb_set(0x41c0, 0xf, (ic & 0xf0) >> 4);
    bb_write(0x41cc, 0x0a11fb88); bb_write(0x41d8, 0xe008ff81); bb_write(0x41dc, 0x0003d208);
    bb_set(0x41d8, 0xf0000000, qc & 0xf); bb_set(0x41dc, 0xf, (qc & 0xf0) >> 4);
    bb_write(0x41b8, 0x60000000); delay_ms(2);
    bb_set(0x41bc, 0xe, 0x6); delay_ms(2);
    bb_write(0x41b0, 0x0a11fb89); bb_write(0x41cc, 0x0a11fb89); delay_ms(1);
    bb_write(0x41b8, 0x62000000); bb_write(0x41d4, 0x62000000); delay_ms(1);
    dack_poll(0x4524, 0x07f80000, ic); dack_poll(0x4550, 0x07f80000, qc);
    bb_write(0x41b8, 0x02000000); delay_ms(1);
    bb_set(0x41bc, 0xe, 0x3); bb_write(0x09b4, 0xdb6db600);
    temp1 = ((adc_ic_b + 0x10) & 0x3ff) | (((adc_qc_b + 0x10) & 0x3ff) << 10);
    bb_write(0x4168, temp1);
    bb_write(0x4110, 0x02d508c5); bb_write(0x4160, 0xf0000000);
    dack_mode(&ic, &qc);
    ic = (ic >= 0x10) ? ic - 0x10 : 0x400 - (0x10 - ic);
    qc = (qc >= 0x10) ? qc - 0x10 : 0x400 - (0x10 - qc);
    if (ic >= 0x200) ic = 0x400 - ic;
    if (qc >= 0x200) qc = 0x400 - qc;
    if (ic < 5 && qc < 5) break;
  }
  bb_write(0x4168, 0x0); bb_write(0x4110, 0x02d508c4);
  bb_set(0x41bc, 0x1, 0x0); bb_set(0x4130, 1u<<30, 0x1);
  bb_write(0x1b00, 0x00000008); mac_write8(0x1bcc, 0x0);
  bb_write(0x1b00, 0x0000000a); mac_write8(0x1bcc, 0x0);

  /* restore working regs; cal results (0x18bc/c0/d8/dc + path B) persist */
  for (int i = 0; i < 16; i++) bb_write(bp_reg[i], bp[i]);
  rf_write(PATH_A, 0x8f, RFREG_MASK, bprf[0]);
  rf_write(PATH_B, 0x8f, RFREG_MASK, bprf[1]);
  _logger->info("Jaguar3: DACK (DAC calibration) complete");
}

void Halrf8822c::pwr_track() {
  /* 5 GHz swing tables (rtw8822c_pwrtrk_5g{a,b}_{p,n}; all 3 sub-bands are
   * identical in the vendor data, so one set covers UNII-1/2/3). Index = thermal
   * delta (0..29); value = TX-power-index compensation in 0.25 dB steps. */
  static const uint8_t p5a[30] = {0, 1, 2,  3,  4,  5,  6,  7,  8,  9,
                                  10, 11, 12, 13, 14, 15, 16, 17, 18, 20,
                                  21, 22, 23, 24, 25, 26, 27, 28, 29, 30};
  static const uint8_t n5a[30] = {0, 1, 2,  4,  5,  6,  7,  8,  9,  10,
                                  11, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                                  23, 24, 25, 26, 27, 28, 29, 30, 31, 33};
  static const uint8_t p5b[30] = {0, 1, 2,  3,  4,  5,  6,  7,  8,  9,
                                  10, 10, 11, 12, 13, 14, 15, 16, 17, 18,
                                  19, 20, 21, 22, 22, 23, 24, 25, 26, 27};
  static const uint8_t n5b[30] = {0, 1, 2,  3,  5,  6,  7,  8,  9,  10,
                                  11, 12, 13, 14, 15, 16, 18, 19, 20, 21,
                                  22, 23, 24, 25, 26, 27, 28, 29, 30, 32};
  const uint8_t *pos[2] = {p5a, p5b}, *neg[2] = {n5a, n5b};
  const uint16_t ref_reg[2] = {0x18a0, 0x41a0}; /* per-path PWR_TRACK field */
  for (uint8_t p = 0; p < 2; p++) { /* trigger a fresh thermal reading */
    rf_write(p, 0x42, 1u << 19, 1);
    rf_write(p, 0x42, 1u << 19, 0);
    rf_write(p, 0x42, 1u << 19, 1);
  }
  delay_us(15);
  int t[2];
  for (uint8_t p = 0; p < 2; p++) {
    t[p] = static_cast<int>(rf_read(p, 0x42, 0x7e)); /* RF_T_METER[6:1] */
    if (_thermal_ref[p] < 0)
      _thermal_ref[p] = t[p]; /* cold-boot reference */
  }
  /* need_lck: when path-A thermal drifts >= the LCK threshold (8) from the last
   * LCK reference, re-lock the synth. In practice the chip warms only ~3 thermal
   * units during TX so this rarely triggers; it's here for faithfulness and
   * long sessions. */
  if (_thermal_lck_ref < 0)
    _thermal_lck_ref = t[0];
  int dlck = t[0] > _thermal_lck_ref ? t[0] - _thermal_lck_ref
                                     : _thermal_lck_ref - t[0];
  if (dlck >= 8) {
    do_lck();
    _thermal_lck_ref = t[0];
  }
  for (uint8_t p = 0; p < 2; p++) {
    int delta = t[p] > _thermal_ref[p] ? t[p] - _thermal_ref[p]
                                       : _thermal_ref[p] - t[p];
    if (delta > 29)
      delta = 29;
    int idx = t[p] > _thermal_ref[p] ? pos[p][delta] : -neg[p][delta];
    bb_set(ref_reg[p], 0x7f, static_cast<uint32_t>(idx) & 0x7f);
  }
}

bool Halrf8822c::read_thermal(uint8_t &raw, uint8_t &baseline) {
  /* Trigger a fresh reading, same sequence as the pwr_track tick. Caller
   * serializes against pwr_track (identical RF 0x42[19] RMW). */
  rf_write(PATH_A, 0x42, 1u << 19, 1);
  rf_write(PATH_A, 0x42, 1u << 19, 0);
  rf_write(PATH_A, 0x42, 1u << 19, 1);
  delay_us(15);
  raw = static_cast<uint8_t>(rf_read(PATH_A, 0x42, 0x7e)); /* RF_T_METER[6:1] */
  if (raw == 0)
    return false; /* meter not ready */
  if (_thermal_ref[0] < 0)
    _thermal_ref[0] = raw; /* first read = the cold reference pwr_track uses */
  baseline = static_cast<uint8_t>(_thermal_ref[0]);
  return true;
}

void Halrf8822c::do_lck() {
  /* Port of rtw8822c_do_lck (RF_SYN_CTRL=0xbb, SYN_PFD=0xb0, AAC_CTRL=0xca,
   * SYN_AAC=0xc9, FAST_LCK=0xcc). Re-locks the LC tank at the current freq. */
  rf_write(PATH_A, 0xbb, RFREG_MASK, 0x80010);
  rf_write(PATH_A, 0xb0, RFREG_MASK, 0x1f0fa);
  delay_us(1);
  rf_write(PATH_A, 0xca, RFREG_MASK, 0x80000);
  rf_write(PATH_A, 0xc9, RFREG_MASK, 0x80001);
  for (int i = 0; i < 100; i++) { /* wait AAC done (RF_AAC_CTRL[12] -> 0) */
    if (rf_read(PATH_A, 0xca, 0x1000) != 1)
      break;
    delay_ms(1);
  }
  rf_write(PATH_A, 0xb0, RFREG_MASK, 0x1f0f8);
  rf_write(PATH_B, 0xbb, RFREG_MASK, 0x80010);
  rf_write(PATH_A, 0xcc, RFREG_MASK, 0x0f000);
  rf_write(PATH_A, 0xcc, RFREG_MASK, 0x4f000);
  delay_us(1);
  rf_write(PATH_A, 0xcc, RFREG_MASK, 0x0f000);
  _logger->info("Jaguar3: LCK (synth re-lock) done");
}

void Halrf8822c::coex_run_5g() {
  /* COEX_SET_ANT_5G: GNT_BT = HW-PTA (state 0), GNT_WL = SW-high (state 3),
   * path-control owner = WL. GNT fields in LTE_COEX_CTRL (0x38): BT @ 0xc000 &
   * 0x0c00, WL @ 0x3000 & 0x0300. */
  btc_write_indirect(0x38, 0xc000, 0x0);
  btc_write_indirect(0x38, 0x0c00, 0x0);
  btc_write_indirect(0x38, 0x3000, 0x3);
  btc_write_indirect(0x38, 0x0300, 0x3);
  mac_write8(0x73, static_cast<uint8_t>(mac_read8(0x73) | 0x04));
  /* PTA table, 5 GHz shared-ant case 0 = WL wins all (REG_BT_COEX_TABLE0/1 +
   * break table). */
  _device.rtw_write32(0x6c0, 0xffffffffu);
  _device.rtw_write32(0x6c4, 0xffffffffu);
  _device.rtw_write32(0x6c8, 0xf0ffffffu);
  /* keep the scoreboard active|on so the BT side knows WL owns the medium */
  _device.rtw_write<uint16_t>(0xAA, 0x8003);
}

void Halrf8822c::coex_wlan_only_init() {
  /* byte read-modify-write (rtw_write8_mask): val is the field value, placed at
   * the mask's low bit. */
  auto w8m = [&](uint16_t reg, uint8_t mask, uint8_t val) {
    uint8_t o = _device.rtw_read<uint8_t>(reg);
    /* trailing-zero count of mask (portable; mask is a non-zero field mask) */
    uint8_t sh = 0;
    for (uint8_t m = mask; m && !(m & 1); m >>= 1)
      ++sh;
    o = static_cast<uint8_t>((o & ~mask) | ((val << sh) & mask));
    _device.rtw_write<uint8_t>(reg, o);
  };
  auto set32 = [&](uint16_t reg, uint32_t bits) {
    _device.rtw_write<uint32_t>(reg, _device.rtw_read<uint32_t>(reg) | bits);
  };

  /* --- rtw8822c_coex_cfg_init: base PTA hardware config --- */
  w8m(0x550, 0x08, 1);            /* REG_BCN_CTRL: enable TBTT/beacon function */
  w8m(0x790, 0x3f, 0x5);          /* REG_BT_TDMA_TIME: BT report sample rate */
  _device.rtw_write<uint8_t>(0x778, 0x1); /* REG_BT_STAT_CTRL: BT counters on */
  set32(0x40, (1u << 5) | (1u << 9)); /* REG_GPIO_MUXCFG: BT_PTA_EN|PO_BT_PTA */
  w8m(0x4c6, 0x10, 1);            /* REG_QUEUE_CTRL: PTA_WL_TX_EN */
  w8m(0x4c6, 0x20, 0);            /* clear PTA_EDCCA_EN (WL TX not via EDCCA) */
  _device.rtw_write<uint16_t>(                /* REG_BT_COEX_V2: GNT_BT polarity */
      0x762, static_cast<uint16_t>(_device.rtw_read<uint16_t>(0x762) | (1u << 12)));
  w8m(0x4fc, 0x03, 0);            /* REG_DUMMY_PAGE4_V1: clear BTCCA_CTRL */

  /* --- rtw8822c_coex_cfg_rfe_type: disable the LTE/BT coex arbitration on the
   * WiFi side entirely, and let WL/BT TRX through unconditionally. --- */
  btc_write_indirect(0x38, 0x80, 0x0);        /* LTE_COEX_CTRL BIT_LTE_COEX_EN=0 */
  btc_write_indirect(0xa0, 0xffff, 0xffff);   /* LTE_WL_TRX_CTRL = all-pass */
  btc_write_indirect(0xa4, 0xffff, 0xffff);   /* LTE_BT_TRX_CTRL = all-pass */

  /* --- rtw8822c_coex_cfg_gnt_fix (5 GHz): WL TX must not be masked by GNT_BT.
   * Enable the "DAC off if GNT_WL=0" guard (harmless since we force GNT_WL high)
   * and tell the BB to ignore GNT_BT when gating TX. --- */
  rf_write(PATH_B, 0x01, RFREG_MASK, 0x42000); /* RF param error workaround +S1 */
  w8m(0x1c32, 0x40, 1);           /* REG_ANAPAR+2: BIT_ANAPAR_BTPS (0x1c30[22]) */
  w8m(0x1c39, 0x10, 0);           /* REG_RSTB_SEL+1: 0x1c38[12]=0 */
  w8m(0x1c3b, 0x10, 1);           /* REG_RSTB_SEL+3: 0x1c38[28]=1 (DAC-off en) */
  w8m(0x4160, 0x08, 1);           /* REG_IGN_GNTBT4: PI_IGNORE_GNT_BT */
  w8m(0x1860, 0x08, 0);           /* REG_IGN_GNT_BT1: don't ignore (5G path) */
  w8m(0x1ca7, 0x08, 1);           /* REG_NOMASK_TXBT: WL TX not masked by BT */

  /* --- COEX_SET_ANT_WONLY: antenna fixed to WLAN --- */
  force_wl_antenna();
  _logger->info("Jaguar3: coex WiFi-only init applied (antenna locked to WL)");
}
#endif /* DEVOURER_HAVE_JAGUAR3_8822C */

} /* namespace jaguar3 */
