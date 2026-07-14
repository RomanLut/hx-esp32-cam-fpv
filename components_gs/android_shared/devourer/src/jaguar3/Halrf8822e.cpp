#include "Halrf8822e.h"
#include <cstdlib>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace jaguar3 {

namespace {
/* RF direct-write window bases (path A / path B) — identical to 8822c. */
constexpr uint16_t RF_WIN[2] = {0x3c00, 0x4c00};

/* IQK MAC/BB/RF register backup tables (halrf_iqk_8822e.c _phy_iq_calibrate). */
constexpr uint16_t kBackupMacReg[4] = {0x520, 0x1c, 0xec, 0x70};
constexpr uint16_t kBackupBbReg[21] = {
    0x0820, 0x0824, 0x1c38, 0x1c68, 0x1d60, 0x180c, 0x410c, 0x1c3c, 0x1a14,
    0x1d58, 0x1d70, 0x1864, 0x4164, 0x186c, 0x416c, 0x1a14, 0x1e70, 0x080c,
    0x1e7c, 0x18a4, 0x41a4};
constexpr uint16_t kBackupRfReg[3] = {0x19, 0x9e, 0x0};

/* trailing-zero count of a non-zero mask (portable; no __builtin_ctz so the
 * MSVC/mingw CI toolchains build). */
uint32_t mask_shift(uint32_t mask) {
  uint32_t s = 0;
  if (mask == 0)
    return 0;
  while (!(mask & 1)) {
    mask >>= 1;
    ++s;
  }
  return s;
}
} /* namespace */

Halrf8822e::Halrf8822e(RtlAdapter device, Logger_t logger,
                       const devourer::DeviceConfig &cfg)
    : _device{device}, _logger{logger}, _skip_txgapk{cfg.tuning.skip_txgapk},
      _gaintab_dbg{cfg.debug.gaintab_dbg} {}

/* --- dm_struct register-access shim (mirrors Halrf8822c) --- */
uint32_t Halrf8822e::bb_get(uint16_t addr, uint32_t mask) {
  return (_device.rtw_read32(addr) & mask) >> mask_shift(mask);
}
void Halrf8822e::bb_set(uint16_t addr, uint32_t mask, uint32_t val) {
  uint32_t o = _device.rtw_read32(addr);
  _device.rtw_write32(addr, (o & ~mask) | ((val << mask_shift(mask)) & mask));
}
uint32_t Halrf8822e::rf_read(uint8_t path, uint16_t addr, uint32_t mask) {
  uint16_t direct = static_cast<uint16_t>(RF_WIN[path & 1] + ((addr & 0xff) << 2));
  return bb_get(direct, mask & RFREG_MASK);
}
void Halrf8822e::rf_write(uint8_t path, uint16_t addr, uint32_t mask,
                          uint32_t val) {
  uint16_t direct = static_cast<uint16_t>(RF_WIN[path & 1] + ((addr & 0xff) << 2));
  bb_set(direct, mask & RFREG_MASK, val);
}

/* --- calibration (Phase C: ported incrementally, hardware-iterated) --- */

void Halrf8822e::delay_us(uint32_t us) {
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}
void Halrf8822e::delay_ms(uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/* halrf_wdack_8822e: every DACK BB write is issued twice (AFEDIG write race). */
void Halrf8822e::wdack(uint16_t reg, uint32_t mask, uint32_t data) {
  bb_set(reg, mask, data);
  bb_set(reg, mask, data);
}

/* halrf_afereg_check / halrf_write_check_afe: an AFE write must be confirmed
 * (the bank reads back non-zero); retry up to 100x past an IO race. */
bool Halrf8822e::afereg_check(uint16_t wa) {
  return bb_get(wa, 0xFFFFFFFFu) != 0x0;
}
void Halrf8822e::write_check_afe(uint16_t add, uint32_t data) {
  uint32_t wd = (add == 0x3800 || add == 0x3900) ? data : 0xee32001fu;
  uint16_t wa = ((add >> 8) == 0x38) ? 0x3800 : 0x3900;
  for (uint32_t count = 0; count < 100; ++count) {
    bb_write(0x2dd4, 0x0);
    bb_write(add, data);
    bb_write(add, data);
    bb_write(0x2dd4, 0x0);
    if (afereg_check(wa))
      return;
    bb_write(wa, wd);
    bb_write(wa, wd);
  }
}

void Halrf8822e::dack_soft_rst() {
  write_check_afe(0x3800, 0xee30001fu);
  write_check_afe(0x3800, 0xee32001fu);
  write_check_afe(0x382c, 0xee30001fu);
  write_check_afe(0x382c, 0xee32001fu);
  write_check_afe(0x3900, 0xee30001fu);
  write_check_afe(0x3900, 0xee32001fu);
  write_check_afe(0x392c, 0xee30001fu);
  write_check_afe(0x392c, 0xee32001fu);
}

void Halrf8822e::dack_reset() {
  bb_set(0x1818, 1u << 25, 0);
  bb_set(0x1818, 1u << 25, 1);
  delay_ms(1);
  bb_set(0x4118, 1u << 25, 0);
  bb_set(0x4118, 1u << 25, 1);
  delay_ms(1);
}

void Halrf8822e::dac_fifo_reset() { /* needs DAC clock */
  bb_set(0x3800, 1u << 21, 0);
  bb_set(0x382c, 1u << 21, 0);
  bb_set(0x3800, 1u << 21, 1);
  bb_set(0x382c, 1u << 21, 1);
  bb_set(0x3900, 1u << 21, 0);
  bb_set(0x392c, 1u << 21, 0);
  bb_set(0x3900, 1u << 21, 1);
  bb_set(0x392c, 1u << 21, 1);
}

/* halrf_check_addc_8822e: SAR-ADC averaged DC sample for the path's I/Q. */
void Halrf8822e::check_addc(uint8_t path) {
  uint16_t off = (path == 0) ? 0 : 0x100;
  wdack(0x381c + off, 0x60000, 0x3);   /* AVG sample */
  wdack(0x381c + off, 1u << 16, 0x0);  /* AVG enable (even-count write) */
  wdack(0x381c + off, 1u << 16, 0x1);
  wdack(0x381c + off, 1u << 16, 0x1);
  uint32_t c = 0;
  while (bb_get(0x3878 + off, 1u << 12) == 0 || bb_get(0x38a8 + off, 1u << 12) == 0) {
    if (++c > 10000) {
      _dack.addck_timeout[path] = true;
      break;
    }
    delay_us(1);
  }
  _dack.addc[path][0] = static_cast<uint16_t>(bb_get(0x3878 + off, 0xfff));
  _dack.addc[path][1] = static_cast<uint16_t>(bb_get(0x38a8 + off, 0xfff));
}

void Halrf8822e::addck_s0() {
  bb_set(0x1830, 1u << 30, 0x0);
  bb_set(0x1860, 0xf0000000, 0xf);
  bb_set(0x1860, 1u << 26, 0x0);
  bb_set(0x1860, 1u << 12, 0x0); /* ADC input short */
  bb_set(0x1810, 1u << 19, 0x1);
  check_addc(0);
  uint32_t ic = 0x800 - _dack.addc[0][0];
  uint32_t qc = 0x800 - _dack.addc[0][1];
  _dack.addck_d[0][0] = static_cast<uint16_t>(ic);
  _dack.addck_d[0][1] = static_cast<uint16_t>(qc);
  bb_write(0x1868, (ic & 0x3ff) | ((qc & 0x3ff) << 10));
  bb_set(0x1810, 1u << 19, 0x0);
  bb_set(0x1860, 1u << 12, 0x1);
  bb_set(0x1830, 1u << 30, 0x1);
}

void Halrf8822e::addck_s1() {
  bb_set(0x4130, 1u << 30, 0x0);
  bb_set(0x4160, 0xf0000000, 0xf);
  bb_set(0x4160, 1u << 26, 0x0);
  bb_set(0x4160, 1u << 12, 0x0);
  bb_set(0x4110, 1u << 19, 0x1);
  check_addc(1);
  uint32_t ic = 0x800 - _dack.addc[1][0];
  uint32_t qc = 0x800 - _dack.addc[1][1];
  _dack.addck_d[1][0] = static_cast<uint16_t>(ic);
  _dack.addck_d[1][1] = static_cast<uint16_t>(qc);
  bb_write(0x4168, (ic & 0x3ff) | ((qc & 0x3ff) << 10));
  bb_set(0x4110, 1u << 19, 0x0);
  bb_set(0x4160, 1u << 12, 0x1);
  bb_set(0x4130, 1u << 30, 0x1);
}

void Halrf8822e::dack_backup_s0() {
  for (uint8_t i = 0; i < 0x10; ++i) {
    wdack(0x3800, 0x1e, i);
    _dack.new_msbk_d[0][0][i] = static_cast<uint8_t>(bb_get(0x3870, 0xff000000));
    wdack(0x382c, 0x1e, i);
    _dack.new_msbk_d[0][1][i] = static_cast<uint8_t>(bb_get(0x38a0, 0xff000000));
  }
  _dack.new_biask_d[0] = static_cast<uint16_t>(bb_get(0x3878, 0xffc00000));
  _dack.dadck_d[0][0] = static_cast<uint8_t>(bb_get(0x3874, 0xff000000));
  _dack.dadck_d[0][1] = static_cast<uint8_t>(bb_get(0x38a4, 0xff000000));
}

void Halrf8822e::dack_backup_s1() {
  for (uint8_t i = 0; i < 0x10; ++i) {
    wdack(0x3900, 0x1e, i);
    _dack.new_msbk_d[1][0][i] = static_cast<uint8_t>(bb_get(0x3970, 0xff000000));
    wdack(0x392c, 0x1e, i);
    _dack.new_msbk_d[1][1][i] = static_cast<uint8_t>(bb_get(0x39a0, 0xff000000));
  }
  _dack.new_biask_d[1] = static_cast<uint16_t>(bb_get(0x3978, 0xffc00000));
  _dack.dadck_d[1][0] = static_cast<uint8_t>(bb_get(0x3974, 0xff000000));
  _dack.dadck_d[1][1] = static_cast<uint8_t>(bb_get(0x39a4, 0xff000000));
}

/* Reload the calibrated MSBK/biasK/DADCK coefficients into the AFE coefficient
 * registers (0x38c0.. per path/index). */
void Halrf8822e::dack_reload_by_path(uint8_t path, uint8_t index) {
  uint16_t off = static_cast<uint16_t>((index == 0 ? 0 : 0x14) + (path == 0 ? 0 : 0x100));
  auto pack4 = [&](int base) {
    uint32_t t = 0;
    for (int i = 0; i < 4; ++i)
      t |= static_cast<uint32_t>(_dack.new_msbk_d[path][index][base + i]) << (i * 8);
    return t;
  };
  wdack(0x38c0 + off, 0xFFFFFFFFu, pack4(12));
  wdack(0x38c4 + off, 0xFFFFFFFFu, pack4(8));
  wdack(0x38c8 + off, 0xFFFFFFFFu, pack4(4));
  wdack(0x38cc + off, 0xFFFFFFFFu, pack4(0));
  uint32_t t = (static_cast<uint32_t>(_dack.new_biask_d[path]) << 16) |
               (static_cast<uint32_t>(_dack.dadck_d[path][index]) << 8);
  wdack(0x38d0 + off, 0xFFFFFFFFu, t);
}

void Halrf8822e::dack_reload(uint8_t path) {
  for (uint8_t i = 0; i < 2; ++i)
    dack_reload_by_path(path, i);
}

void Halrf8822e::dack_s0() {
  uint32_t temp = bb_get(0x9b4, 0xFFFFFFFFu);
  bb_set(0x9b4, 0x1ff00, 0xdb); /* clk 160MHz for cal */
  bb_set(0x1830, 1u << 30, 0x0);
  bb_set(0x1860, 1u << 30, 0x1);
  bb_set(0x1860, 1u << 27, 0x0);
  bb_set(0x1810, 1u << 15, 0x1); /* enable comparator */
  bb_set(0x1818, 0x0c000000, 0x3); /* DAC gain cal mode */
  wdack(0x3804, 0x3ff00000, 0x58); /* MSB bias init */
  wdack(0x3830, 0x3ff00000, 0x58);
  dac_fifo_reset();
  wdack(0x380c, 1u << 1, 0x0);
  wdack(0x3804, 1u << 0, 0x1); /* auto mode */
  delay_us(1);
  uint32_t c = 0;
  while (bb_get(0x385c, 1u << 1) == 0 || bb_get(0x388c, 1u << 1) == 0) {
    if (++c > 10000) { _dack.msbk_timeout[0] = true; break; }
    delay_us(1);
  }
  bb_set(0x1818, 0x0c000000, 0x0); /* DAC gain normal */
  wdack(0x380c, 1u << 1, 0x1);     /* enable DC offset cal */
  delay_us(1);
  c = 0;
  while (bb_get(0x3870, 1u << 2) == 0 || bb_get(0x38a0, 1u << 2) == 0) {
    if (++c > 10000) { _dack.dadck_timeout[0] = true; break; }
    delay_us(1);
  }
  wdack(0x3804, 1u << 0, 0x0);   /* disable auto */
  bb_set(0x1810, 1u << 15, 0x0); /* disable comparator */
  dack_backup_s0();
  dack_reload(0);
  bb_set(0x9b4, 0xFFFFFFFFu, temp); /* clk back to normal */
  dac_fifo_reset();
  bb_set(0x1830, 1u << 30, 0x1);
}

void Halrf8822e::dack_s1() {
  uint32_t temp = bb_get(0x9b4, 0xFFFFFFFFu);
  bb_set(0x9b4, 0x1ff00, 0xdb);
  bb_set(0x4130, 1u << 30, 0x0);
  bb_set(0x4160, 1u << 30, 0x1);
  bb_set(0x4160, 1u << 27, 0x0);
  bb_set(0x4110, 1u << 15, 0x1);
  bb_set(0x4118, 0x0c000000, 0x3);
  wdack(0x3904, 0x3ff00000, 0x58);
  wdack(0x3930, 0x3ff00000, 0x58);
  dac_fifo_reset();
  wdack(0x390c, 1u << 1, 0x0);
  wdack(0x3904, 1u << 0, 0x1);
  delay_us(1);
  uint32_t c = 0;
  while (bb_get(0x395c, 1u << 1) == 0 || bb_get(0x398c, 1u << 1) == 0) {
    if (++c > 10000) { _dack.msbk_timeout[1] = true; break; }
    delay_us(1);
  }
  bb_set(0x4118, 0x0c000000, 0x0);
  wdack(0x390c, 1u << 1, 0x1);
  delay_us(1);
  c = 0;
  while (bb_get(0x3970, 1u << 2) == 0 || bb_get(0x39a0, 1u << 2) == 0) {
    if (++c > 10000) { _dack.dadck_timeout[1] = true; break; }
    delay_us(1);
  }
  wdack(0x3904, 1u << 0, 0x0);
  bb_set(0x4110, 1u << 15, 0x0);
  dack_backup_s1();
  dack_reload(1);
  bb_set(0x9b4, 0xFFFFFFFFu, temp);
  dac_fifo_reset();
  bb_set(0x4130, 1u << 30, 0x1);
}

void Halrf8822e::dack_val(bool from_reg) {
  uint32_t kv = from_reg ? 0x1 : 0x0;
  wdack(0x38d0, 1u << 0, kv);
  wdack(0x38e4, 1u << 0, kv);
  wdack(0x39d0, 1u << 0, kv);
  wdack(0x39e4, 1u << 0, kv);
}

bool Halrf8822e::dack_checkfail() {
  return bb_get(0x3800, 1u << 17) == 0 || bb_get(0x3900, 1u << 17) == 0;
}

/* halrf_dac_cal_8822e entry: ADDCK, then DACK (S0+S1) with a checkfail retry. */
void Halrf8822e::dac_calibrate() {
  addck_s0();
  addck_s1();
  dack_reset();
  dack_val(false);
  dack_s0();
  dack_s1();
  int i = 0;
  for (; i < 10; ++i) {
    if (!dack_checkfail())
      break;
    dack_reset();
    dack_val(false);
    dack_s0();
    dack_s1();
  }
  dack_val(true);
  _logger->info("Jaguar3(8822e): DACK complete (retries={}, msbk_to={}/{}, "
                "dadck_to={}/{}, addck_to={}/{})",
                i, _dack.msbk_timeout[0], _dack.msbk_timeout[1],
                _dack.dadck_timeout[0], _dack.dadck_timeout[1],
                _dack.addck_timeout[0], _dack.addck_timeout[1]);
}

/* --- IQK support layer (port of halrf_iqk_8822e.c, in progress) --- */

/* _iqk_check_cal_8822e: poll the NCTL cal-ready handshake. */
bool Halrf8822e::iqk_check_cal(uint8_t /*path*/, uint8_t cmd) {
  bool fail = true;
  uint32_t cnt = 0;
  delay_us(1);
  while (true) {
    if (bb_get(0x2d9c, 0x000000ff) == 0x55) {
      fail = (cmd == IQK_LOK1 || cmd == IQK_LOK2) ? false
                                                  : (bb_get(0x1b08, 1u << 26) != 0);
      break;
    }
    delay_us(1);
    if (++cnt >= 3000) { fail = true; break; }
  }
  cnt = 0;
  delay_us(1);
  while (true) {
    if (bb_get(0x1bfc, 0x0000ffff) == 0x8000)
      break;
    delay_us(1);
    if (++cnt >= 500) { fail = true; break; }
  }
  delay_us(50);
  bb_set(0x1b10, 0x000000ff, 0x0);
  bb_set(0x1b08, 1u << 26, 0x0);
  return fail;
}

/* LTE/BTC indirect register window (0x1700/0x1704/0x1708). */
uint32_t Halrf8822e::btc_wait_indirect_ready() {
  uint32_t delay_count = 0;
  while (true) {
    if ((bb_get(0x1700, 0xff000000) & (1u << 5)) == 0) {
      delay_us(100);
      if (++delay_count >= 10)
        break;
    } else {
      break;
    }
  }
  return delay_count;
}
uint32_t Halrf8822e::btc_read_indirect(uint16_t reg) {
  btc_wait_indirect_ready();
  bb_write(0x1700, 0x800F0000u | reg);
  return bb_read(0x1708);
}
void Halrf8822e::btc_write_indirect(uint16_t reg, uint32_t mask, uint32_t val) {
  if (mask == 0x0)
    return;
  if (mask == 0xffffffffu) {
    btc_wait_indirect_ready();
    bb_write(0x1704, val);
    bb_write(0x1700, 0xc00F0000u | reg);
  } else {
    uint32_t bitpos = 0;
    for (uint32_t i = 0; i <= 31; ++i)
      if (((mask >> i) & 0x1) == 0x1) { bitpos = i; break; }
    uint32_t v = btc_read_indirect(reg);
    v = (v & ~mask) | (val << bitpos);
    btc_wait_indirect_ready();
    bb_write(0x1704, v);
    bb_write(0x1700, 0xc00F0000u | reg);
  }
}
void Halrf8822e::iqk_set_gnt_wl_high() {
  btc_write_indirect(0x38, 0xff00, 0x77); /* GNT_WL high: 0x38[15:8]=0x77 */
}
void Halrf8822e::iqk_set_gnt_wl_gnt_bt(bool before_k) {
  if (before_k)
    iqk_set_gnt_wl_high();
  else
    btc_write_indirect(0x38, 0xffffffffu, _iqk.tmp_gntwl);
}

/* CFIR (calibration FIR) coefficient backup/reload — read the calibrated TX/RX
 * filter taps out of the NCTL window (0x1bd8/0x1bfc) into IqkInfo, and write
 * them back on a channel revisit. */
void Halrf8822e::iqk_backup_txcfir(uint8_t path) {
  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1b20, 0xc0000000, 0x3);
  bb_set(0x1bd8, 0xFFFFFFFFu, 0x0000071);
  bb_set(0x1bd4, 0x00200000, 0x1);
  bb_set(0x1bd4, 0x001f0000, 0x10);
  for (uint8_t i = 0; i < 17; ++i) {
    bb_set(0x1bd8, 0x01f00000, i);
    uint32_t tmp = bb_get(0x1bfc, 0xFFFFFFFFu);
    _iqk.iqk_cfir_real[0][path][0][i] = static_cast<uint16_t>((tmp & 0x0fff0000) >> 16);
    _iqk.iqk_cfir_imag[0][path][0][i] = static_cast<uint16_t>(tmp & 0x0fff);
  }
  bb_set(0x1b20, 0xc0000000, 0x0);
  bb_set(0x1bd8, 0xFFFFFFFFu, 0x0000070);
}
void Halrf8822e::iqk_backup_rxcfir(uint8_t path) {
  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1b20, 0xc0000000, 0x1);
  bb_set(0x1bd8, 0xFFFFFFFFu, 0x0000071);
  bb_set(0x1bd4, 0x00200000, 0x1);
  bb_set(0x1bd4, 0x001f0000, 0x10);
  for (uint8_t i = 0; i < 17; ++i) {
    bb_set(0x1bd8, 0x01f00000, i);
    uint32_t tmp = bb_get(0x1bfc, 0xFFFFFFFFu);
    _iqk.iqk_cfir_real[0][path][1][i] = static_cast<uint16_t>((tmp & 0x0fff0000) >> 16);
    _iqk.iqk_cfir_imag[0][path][1][i] = static_cast<uint16_t>(tmp & 0x0fff);
  }
  bb_set(0x1b20, 0xc0000000, 0x0);
  bb_set(0x1bd8, 0xFFFFFFFFu, 0x0000070);
}
void Halrf8822e::iqk_reload_txcfir(uint8_t ch, uint8_t path) {
  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1bd8, 0x000000ff, 0x63);
  for (uint8_t i = 0; i < 17; ++i) {
    bb_set(0x1bd8, 0x01f00000, i);
    bb_set(0x1bd8, 0x000fff00, _iqk.iqk_cfir_real[ch][path][0][i]);
  }
  bb_set(0x1bd8, 0x000000ff, 0x61);
  for (uint8_t i = 0; i < 17; ++i) {
    bb_set(0x1bd8, 0x01f00000, i);
    bb_set(0x1bd8, 0x000fff00, _iqk.iqk_cfir_imag[ch][path][0][i]);
  }
  bb_set(0x1b20, 0xc0000000, 0x0);
  bb_set(0x1bd8, 0xFFFFFFFFu, 0x0000070);
}
void Halrf8822e::iqk_reload_rxcfir(uint8_t ch, uint8_t path) {
  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1bd8, 0x000000ff, 0x33);
  for (uint8_t i = 0; i < 17; ++i) {
    bb_set(0x1bd8, 0x01f00000, i);
    bb_set(0x1bd8, 0x000fff00, _iqk.iqk_cfir_real[ch][path][1][i]);
  }
  bb_set(0x1bd8, 0x000000ff, 0x31);
  for (uint8_t i = 0; i < 17; ++i) {
    bb_set(0x1bd8, 0x01f00000, i);
    bb_set(0x1bd8, 0x000fff00, _iqk.iqk_cfir_imag[ch][path][1][i]);
  }
  bb_set(0x1b20, 0xc0000000, 0x0);
  bb_set(0x1bd8, 0xFFFFFFFFu, 0x0000070);
}
void Halrf8822e::iqk_backup_cfir(uint8_t path, uint8_t idx) {
  bb_set(0x1b00, 0x00000006, path);
  if (idx == IQK_TXIQK || idx == IQK_NBTXK) {
    _iqk.txxy[0][path] = bb_get(0x1b38, 0xffffffff);
    iqk_backup_txcfir(path);
  } else if (idx == IQK_RXIQK || idx == IQK_RXIQK2 || idx == IQK_NBRXK) {
    _iqk.rxxy[0][path] = bb_get(0x1b3c, 0xffffffff);
    iqk_backup_rxcfir(path);
  }
  bb_set(0x1b00, 0x00000006, path);
  _iqk.cfir_en[0][path] = bb_get(0x1b70, 0xffffffff);
  _iqk.iqk_tab[0] = _iqk.rf_reg18;
}
void Halrf8822e::iqk_reload_cfir(uint8_t ch, uint8_t path, uint8_t idx) {
  bb_set(0x1b00, 0x00000006, path);
  if (idx == IQK_TXIQK || idx == IQK_NBTXK) {
    iqk_reload_txcfir(ch, path);
    bb_set(0x1b38, 0xffffffff, _iqk.txxy[ch][path]);
  } else if (idx == IQK_RXIQK || idx == IQK_NBRXK) {
    iqk_reload_rxcfir(ch, path);
    bb_set(0x1b3c, 0xffffffff, _iqk.rxxy[ch][path]);
  }
  bb_set(0x1b70, 0xffffffff, _iqk.cfir_en[ch][path]);
}

/* AFE setup/restore for IQK (the long 0x1830/0x4130 AFEDIG sequences). */
void Halrf8822e::iqk_afe_setting() {
  bb_write(0x1c38, 0x00000000);
  bb_set(0x1830, 0x40000000, 0x0);
  bb_set(0x1860, 0xfffff000, 0xf0001);
  bb_set(0x4130, 0x40000000, 0x0);
  bb_set(0x4160, 0xfffff000, 0xf0001);
  static const uint32_t seq[] = {
      0x700f0001, 0x700f0001, 0x701f0001, 0x702f0001, 0x703f0001, 0x704f0001,
      0x705f0001, 0x706f0001, 0x707f0001, 0x708f0001, 0x709f0001, 0x70af0001,
      0x70bf0001, 0x70cf0001, 0x70df0001, 0x70ef0001, 0x70ff0001, 0x70ff0001};
  for (uint32_t v : seq)
    bb_write(0x1830, v);
  for (uint32_t v : seq)
    bb_write(0x4130, v);
  bb_write(0x1c38, 0xffffffff);
  iqk_ex_dac_fifo_rst();
}
void Halrf8822e::iqk_afe_restore() {
  bb_write(0x1c38, 0x0);
  bb_set(0x1830, 0x40000000, 0x1);
  bb_set(0x4130, 0x40000000, 0x1);
  static const uint32_t seq[] = {
      0x700f0001, 0x700f0001, 0x700f0001, 0x70144001, 0x70244001, 0x70344001,
      0x70444001, 0x705b0001, 0x70644001, 0x707b0001, 0x708f0001, 0x709f0001,
      0x70af0001, 0x70bf0001, 0x70cb0001, 0x70db0001, 0x70eb0001, 0x70fb0001,
      0x70fb0001};
  for (uint32_t v : seq)
    bb_write(0x1830, v);
  static const uint32_t seq_b[] = {
      0x700f0001, 0x700f0001, 0x70144001, 0x70244001, 0x70344001, 0x70444001,
      0x705b0001, 0x70644001, 0x707b0001, 0x708f0001, 0x709f0001, 0x70af0001,
      0x70bf0001, 0x70cb0001, 0x70db0001, 0x70eb0001, 0x70fb0001, 0x70fb0001};
  for (uint32_t v : seq_b)
    bb_write(0x4130, v);
  bb_write(0x1c38, 0xffa1005e);
  iqk_ex_dac_fifo_rst();
}

/* MAC/BB/RF backup-restore over the fixed tables. */
void Halrf8822e::iqk_backup_mac_bb() {
  for (int i = 0; i < 4; ++i)
    _mac_backup[i] = _device.rtw_read32(kBackupMacReg[i]);
  for (int i = 0; i < 21; ++i)
    _bb_backup[i] = bb_read(kBackupBbReg[i]);
}
void Halrf8822e::iqk_backup_rf() {
  for (int i = 0; i < 3; ++i) {
    _rf_backup[i][0] = rf_read(0, kBackupRfReg[i], 0xfffff);
    _rf_backup[i][1] = rf_read(1, kBackupRfReg[i], 0xfffff);
  }
}
void Halrf8822e::iqk_restore_mac_bb() {
  bb_write(0x1d70, 0x50505050); /* toggle IGI */
  for (int i = 0; i < 4; ++i)
    _device.rtw_write32(kBackupMacReg[i], _mac_backup[i]);
  for (int i = 0; i < 21; ++i)
    bb_write(kBackupBbReg[i], _bb_backup[i]);
  /* RX-through-IQK enable per path's RXIQK result. */
  bb_set(0x180c, 1u << 31, _iqk.iqk_fail_report[0][0][IQK_RXIQK] ? 0 : 1);
  bb_set(0x410c, 1u << 31, _iqk.iqk_fail_report[0][1][IQK_RXIQK] ? 0 : 1);
}
void Halrf8822e::iqk_restore_rf() {
  rf_write(0, 0xef, 0xfffff, 0x0);
  rf_write(1, 0xef, 0xfffff, 0x0);
  rf_write(0, 0xdf, 0x00010, 0x0);
  rf_write(1, 0xdf, 0x00010, 0x0);
  for (int i = 0; i < 3; ++i) {
    rf_write(0, kBackupRfReg[i], 0xfffff, _rf_backup[i][0]);
    rf_write(1, kBackupRfReg[i], 0xfffff, _rf_backup[i][1]);
  }
  rf_write(0, 0xde, 1u << 16, 0x0);
  rf_write(1, 0xde, 1u << 16, 0x0);
}

void Halrf8822e::iqk_switch_table() {
  _iqk.iqk_tab[1] = _iqk.iqk_tab[0];
  for (int i = 0; i < SS_8822E; ++i) {
    _iqk.lok_idac[1][i] = _iqk.lok_idac[0][i];
    _iqk.txxy[1][i] = _iqk.txxy[0][i];
    _iqk.rxxy[1][i] = _iqk.rxxy[0][i];
    _iqk.cfir_en[1][i] = _iqk.cfir_en[0][i];
    for (int j = 0; j < 2; ++j) {
      _iqk.iqk_fail_report[1][i][j] = _iqk.iqk_fail_report[0][i][j];
      for (int k = 0; k < 17; ++k) {
        _iqk.iqk_cfir_real[1][i][j][k] = _iqk.iqk_cfir_real[0][i][j][k];
        _iqk.iqk_cfir_imag[1][i][j][k] = _iqk.iqk_cfir_imag[0][i][j][k];
      }
    }
  }
}

void Halrf8822e::iqk_tx_pause() {
  mac_write8(0x522, 0xff);
  bb_set(0x1e70, 0x0000000f, 0x2); /* hw tx stop */
  uint8_t a = static_cast<uint8_t>(rf_read(0, 0x00, 0xF0000));
  uint8_t b = static_cast<uint8_t>(rf_read(1, 0x00, 0xF0000));
  uint16_t count = 0;
  while (((a != 3) && (b != 3)) && count < 2500) {
    a = static_cast<uint8_t>(rf_read(0, 0x00, 0xF0000));
    b = static_cast<uint8_t>(rf_read(1, 0x00, 0xF0000));
    delay_us(2);
    ++count;
  }
}

void Halrf8822e::iqk_macbb_setting() {
  bb_set(0x0824, 0x00030000, 0x3); /* rx path on */
  iqk_tx_pause();
  bb_set(0x70, 0xff000000, 0x06); /* PTA control path at WLAN */
  bb_set(0x1e24, 0x00020000, 0x1);
  bb_set(0x1cd0, 0x10000000, 0x1);
  bb_set(0x1cd0, 0x20000000, 0x1);
  bb_set(0x1cd0, 0x40000000, 0x1);
  bb_set(0x1cd0, 0x80000000, 0x0);
  bb_set(0x1864, 0x80000000, 0x1);
  bb_set(0x4164, 0x80000000, 0x1);
  bb_set(0x180c, 0x08000000, 0x1);
  bb_set(0x410c, 0x08000000, 0x1);
  bb_set(0x186c, 0x00000080, 0x1);
  bb_set(0x416c, 0x00000080, 0x1);
  bb_set(0x180c, 0x00000003, 0x0);
  bb_set(0x410c, 0x00000003, 0x0);
  bb_set(0x1a00, 0x00000003, 0x2);
  bb_write(0x1b08, 0x00000080);
}

void Halrf8822e::iqk_macbb_restore() {
  rf_write(0, 0xde, 0x10000, 0x0);
  rf_write(1, 0xde, 0x10000, 0x0);
  bb_set(0x1b00, 0x00000006, 0x0);
  bb_set(0x1bcc, 0x0000003f, 0x00);
  bb_set(0x1b20, 1u << 25, 0x00);
  bb_set(0x1b00, 0x00000006, 0x1);
  bb_set(0x1bcc, 0x0000003f, 0x00);
  bb_set(0x1b20, 1u << 25, 0x00);
  bb_set(0x1b00, 0x00000006, 0x0);
  bb_write(0x1b08, 0x00000000);
  bb_set(0x1d0c, 0x00010000, 0x1);
  bb_set(0x1d0c, 0x00010000, 0x0);
  bb_set(0x1d0c, 0x00010000, 0x1);
  bb_set(0x1864, 0x80000000, 0x0);
  bb_set(0x4164, 0x80000000, 0x0);
  bb_set(0x180c, 0x08000000, 0x0);
  bb_set(0x410c, 0x08000000, 0x0);
  bb_set(0x186c, 0x00000080, 0x0);
  bb_set(0x416c, 0x00000080, 0x0);
  bb_set(0x180c, 0x00000003, 0x3);
  bb_set(0x410c, 0x00000003, 0x3);
  bb_set(0x1a00, 0x00000003, 0x0);
}

void Halrf8822e::iqk_reload_lok_setting(uint8_t path) {
  uint8_t idac_i = static_cast<uint8_t>((_iqk.rf_reg58 & 0xfc000) >> 14);
  uint8_t idac_q = static_cast<uint8_t>((_iqk.rf_reg58 & 0x3f00) >> 8);
  rf_write(path, 0xdf, 1u << 4, 0x0); /* W LOK table */
  rf_write(path, 0xef, 1u << 4, 0x1);
  rf_write(path, 0x33, 0x7f, _iqk.iqk_band == IQK_BAND_2G ? 0x00 : 0x20);
  rf_write(path, 0x08, 0xfc000, idac_i);
  rf_write(path, 0x08, 0x003f0, idac_q);
  rf_write(path, 0xef, 1u << 4, 0x0); /* stop write */
}

bool Halrf8822e::iqk_lok1_check(uint8_t path) {
  uint32_t temp = rf_read(path, 0x58, 0xfffff);
  uint8_t idac_i = static_cast<uint8_t>((temp & 0xf8000) >> 15);
  uint8_t idac_q = static_cast<uint8_t>((temp & 0x07c00) >> 10);
  return idac_i <= 0x3 || idac_i >= 0x1c || idac_q <= 0x3 || idac_q >= 0x1c;
}

/* Trigger one calibration step (LOK/TXK/RXK) and poll for completion. */
bool Halrf8822e::iqk_one_shot(uint8_t path, uint8_t idx) {
  uint32_t cmd = 0x0;
  switch (idx) {
  case IQK_NBTXK:
    bb_set(0x1b2c, 0x00000fff, 0x0c);
    cmd = 0x200 | (1u << (4 + path)) | 0x8;
    break;
  case IQK_NBRXK:
    bb_set(0x1b2c, 0x0fff0000, 0x18);
    cmd = 0x300 | (1u << (4 + path)) | 0x8;
    break;
  case IQK_TXIQK:
    cmd = ((static_cast<uint32_t>(_iqk.iqk_bw) + 4) << 8) | (1u << (path + 4)) | 0x8;
    break;
  case IQK_RXIQK1:
    cmd = 0xf00 | (1u << (4 + path)) | 0x8;
    break;
  case IQK_RXIQK2:
    cmd = ((static_cast<uint32_t>(_iqk.iqk_bw) + 0xa) << 8) | (1u << (path + 4)) | 0x8;
    break;
  case IQK_LOK1:
    cmd = (1u << (4 + path)) | 0x8;
    break;
  case IQK_LOK2:
    cmd = 0x100 | (1u << (4 + path)) | 0x8;
    break;
  }
  bb_write(0x1b00, cmd);
  bb_write(0x1b00, cmd + 1);
  return iqk_check_cal(path, 0x1);
}

/* --- per-path LOK/TXK/RXK cores (port of halrf_iqk_8822e.c) ---
 * LOK1 + LOK2 + TXK with the band-specific RF preset, then a LOK1 sanity check.
 * Returns the TX kfail flag. The MAC 0x1c/0xec top-2-bit field is saved/cleared
 * over LOK and restored after (odm_get/set_mac_reg in the vendor source). */
bool Halrf8822e::iqk_5g_txk(uint8_t path) {
  bool kfail = false;
  rf_write(path, 0xdf, 0x00010, 0x0);
  rf_write(path, 0xde, 0x10000, 0x1);
  rf_write(path, 0x56, 0x00c00, 0x0);
  rf_write(path, 0x56, 0x003e0, 0x07);
  rf_write(path, 0x56, 0x0001f, 0x0c);
  rf_write(path, 0x57, 0x08000, 0x1);
  rf_write(path, 0x64, 0x07000, 0x0);
  rf_write(path, 0xef, 0x00010, 0x1);
  rf_write(path, 0x33, 0x0007f, 0x20);

  uint16_t mreg = (path == 0) ? 0x1c : 0xec;
  uint32_t reg_tmp = _device.rtw_read32(mreg);
  _device.rtw_write32(mreg, reg_tmp & ~0xC0000000u);

  /* LOK1 */
  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1b10, 0x000000ff, 0x00);
  bb_set(0x1bcc, 0x0000003f, 0x00);
  bb_set(0x1bcc, 0x00000fc0, 0x09);
  bb_set(0x1b2c, 0x00000fff, 0x038);
  kfail = iqk_one_shot(path, IQK_LOK1);
  if (kfail)
    _iqk.fail_step |= (1u << 0);
  /* LOK2 */
  bb_set(0x1b10, 0x000000ff, 0x00);
  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1bcc, 0x0000003f, 0x00);
  bb_set(0x1bcc, 0x00000fc0, 0x09);
  bb_set(0x1b2c, 0x00000fff, 0x038);
  kfail = iqk_one_shot(path, IQK_LOK2);

  _device.rtw_write32(mreg, reg_tmp);
  rf_write(path, 0xef, 0x00010, 0x0);
  _iqk.lok_fail[path] = iqk_lok1_check(path);

  /* TXK */
  rf_write(path, 0x56, 0x0001f, 0x13);
  rf_write(path, 0x64, 0x07000, 0x01);
  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1bcc, 0x0000003f, 0x00);
  bb_set(0x1bcc, 0x00000fc0, 0x12);
  kfail = iqk_one_shot(path, _iqk.is_nbiqk ? IQK_NBTXK : IQK_TXIQK);
  _iqk.iqk_fail_report[0][path][0] = kfail;
  if (kfail) {
    _iqk.fail_step |= (1u << 1);
    bb_set(0x1b00, 0x00000006, path);
    bb_write(0x1b38, 0x40000000);
    bb_set(0x1b70, 1u << 8, 0x0);
  }
  return kfail;
}

bool Halrf8822e::iqk_2g_txk(uint8_t path) {
  bool kfail = false;
  rf_write(path, 0xdf, 0x00010, 0x0);
  rf_write(path, 0xde, 0x10000, 0x1);
  rf_write(path, 0x56, 0x00c00, 0x0);
  rf_write(path, 0x56, 0x003e0, 0x0f);
  rf_write(path, 0x56, 0x0001f, 0x05);
  rf_write(path, 0x57, 0x08000, 0x1);
  rf_write(path, 0x53, 0x000e0, 0x0);
  rf_write(path, 0xef, 0x00010, 0x1);
  rf_write(path, 0x33, 0x0007f, 0x00);

  uint16_t mreg = (path == 0) ? 0x1c : 0xec;
  uint32_t reg_tmp = _device.rtw_read32(mreg);
  _device.rtw_write32(mreg, reg_tmp & ~0xC0000000u);

  /* LOK1 */
  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1b10, 0x000000ff, 0x00);
  bb_set(0x1bcc, 0x0000003f, 0x00);
  bb_set(0x1bcc, 0x00000fc0, 0x09);
  bb_set(0x1b2c, 0x00000fff, 0x038);
  kfail = iqk_one_shot(path, IQK_LOK1);
  if (kfail)
    _iqk.fail_step |= (1u << 0);
  /* LOK2 */
  bb_set(0x1b10, 0x000000ff, 0x00);
  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1bcc, 0x0000003f, 0x00);
  bb_set(0x1bcc, 0x00000fc0, 0x09);
  bb_set(0x1b2c, 0x00000fff, 0x038);
  kfail = iqk_one_shot(path, IQK_LOK2);

  _device.rtw_write32(mreg, reg_tmp);
  rf_write(path, 0xef, 0x00010, 0x0);
  _iqk.lok_fail[path] = iqk_lok1_check(path);

  /* TXK */
  rf_write(path, 0x56, 0x003e0, 0x07);
  rf_write(path, 0x56, 0x0001f, 0x0c);
  rf_write(path, 0x53, 0x000e0, 0x01);
  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1bcc, 0x0000003f, 0x00);
  bb_set(0x1bcc, 0x00000fc0, 0x12);
  kfail = iqk_one_shot(path, _iqk.is_nbiqk ? IQK_NBTXK : IQK_TXIQK);
  _iqk.iqk_fail_report[0][path][0] = kfail;
  if (kfail) {
    bb_write(0x1b38, 0x40000000);
    _iqk.fail_step |= (1u << 1);
    bb_set(0x1b70, 1u << 8, 0x0);
  }
  return kfail;
}

/* RX gain search: iterate LNA/RXBB until the IQK report settles in range, then
 * lock the gain. Returns fail (timeout or check_cal fail). 5G/2G differ only in
 * the 0x1bcc tone index and the iteration ceiling. */
bool Halrf8822e::iqk_5g_rx_gain_search1(uint8_t path, bool force) {
  bool fail = true, isbnd = true;
  uint8_t kcount = 0;
  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1b18, 0x00000002, 0x1);
  bb_set(0x1b24, 0x000fffff, 0x70408); /* LNA=1 */
  if (force)
    return false;
  while (isbnd) {
    ++kcount;
    bb_set(0x1b00, 0x00000006, path);
    bb_set(0x1bcc, 0x0000003f, 0x1b);
    bb_set(0x1b2c, 0x0fff0000, 0x018);
    uint32_t cmd = (0xf << 8) | (1u << (path + 4)) | 0x8;
    bb_write(0x1b00, cmd);
    bb_write(0x1b00, cmd + 1);
    delay_us(20);
    fail = iqk_check_cal(path, 0x1);
    bb_set(0x1b00, 0x00000006, path);
    uint32_t rf0 = rf_read(path, 0x0, 0xfffff);
    uint32_t lna = (rf0 & 0x01c00) >> 10;
    uint32_t rxbb = (rf0 & 0x003e0) >> 5;
    if (rxbb > 0x9) { lna++; isbnd = true; }
    else if (rxbb > 0x1) { isbnd = false; }
    else { lna--; isbnd = true; }
    if (lna < 1) lna = 0;
    if (lna >= 7) lna = 7;
    if (isbnd) delay_us(10);
    bb_set(0x1b00, 0x00000006, path);
    bb_set(0x1b24, 0x00001c00, lna);
    bb_set(0x1b24, 0x000003e0, rxbb);
    if (kcount >= 4) { fail = true; isbnd = false; }
  }
  return fail;
}

bool Halrf8822e::iqk_2g_rx_gain_search1(uint8_t path, bool force) {
  bool fail = true, isbnd = true;
  uint8_t kcount = 0;
  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1b18, 0x00000002, 0x1);
  bb_set(0x1b24, 0x000fffff, 0x70408); /* LNA=1 */
  if (force)
    return false;
  while (isbnd) {
    ++kcount;
    bb_set(0x1b00, 0x00000006, path);
    bb_set(0x1bcc, 0x0000003f, 0x12);
    bb_set(0x1b2c, 0x0fff0000, 0x018);
    uint32_t cmd = (0xf << 8) | (1u << (path + 4)) | 0x8;
    bb_write(0x1b00, cmd);
    bb_write(0x1b00, cmd + 1);
    delay_us(20);
    fail = iqk_check_cal(path, 0x1);
    bb_set(0x1b00, 0x00000006, path);
    uint32_t rf0 = rf_read(path, 0x0, 0xfffff);
    uint32_t lna = (rf0 & 0x01c00) >> 10;
    uint32_t rxbb = (rf0 & 0x003e0) >> 5;
    if (rxbb > 0x9) { lna++; isbnd = true; }
    else if (rxbb > 0x1) { isbnd = false; }
    else { lna--; isbnd = true; }
    if (lna < 1) lna = 0;
    if (lna >= 7) lna = 7;
    if (isbnd) delay_us(10);
    bb_set(0x1b00, 0x00000006, path);
    bb_set(0x1b24, 0x00001c00, lna);
    bb_set(0x1b24, 0x000003e0, rxbb);
    if (kcount >= 5) { fail = true; isbnd = false; }
  }
  return fail;
}

void Halrf8822e::iqk_5g_rxk(uint8_t path) {
  bool kfail = false;
  rf_write(path, 0x9e, 0x00020, 0x0);
  rf_write(path, 0x9e, 0x00400, 0x0);
  rf_write(path, 0x56, 0x003e0, 0x00);
  kfail = iqk_5g_rx_gain_search1(path, false);
  if (kfail) {
    _iqk.fail_step |= (1u << 2);
  } else {
    bb_set(0x1b00, 0x00000006, path);
    bb_set(0x1bcc, 0x0000003f, 0x1b);
    kfail = iqk_one_shot(path, _iqk.is_nbiqk ? IQK_NBRXK : IQK_RXIQK2);
  }
  _iqk.iqk_fail_report[0][path][1] = kfail;
  if (kfail) {
    bb_write(0x1b3c, 0x40000000);
    _iqk.fail_step |= (1u << 3);
    bb_set(0x1b70, 1u << 0, 0x0);
  }
}

void Halrf8822e::iqk_2g_rxk(uint8_t path) {
  bool kfail = false;
  rf_write(path, 0x9e, 0x00020, 0x0);
  rf_write(path, 0x9e, 0x00400, 0x0);
  rf_write(path, 0x56, 0x003e0, 0x04);
  kfail = iqk_2g_rx_gain_search1(path, false);
  if (kfail) {
    bb_set(0x1b24, 0x000fffff, 0x70108); /* LNA=0, RXBB=0x8 */
    _iqk.fail_step |= (1u << 2);
  }
  /* NB: 2G RXK runs the RXK2 one-shot even after a gain-search fail (faithful
   * to the vendor source — there is no else branch here). */
  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1bcc, 0x0000003f, 0x12);
  kfail = iqk_one_shot(path, _iqk.is_nbiqk ? IQK_NBRXK : IQK_RXIQK2);
  _iqk.iqk_fail_report[0][path][1] = kfail;
  if (kfail) {
    bb_write(0x1b3c, 0x40000000);
    _iqk.fail_step |= (1u << 3);
    bb_set(0x1b70, 1u << 0, 0x0);
  }
}

void Halrf8822e::iqk_by_path() {
  for (uint8_t path = 0; path < SS_8822E; ++path) {
    /* park the idle path's RF in standby */
    rf_write(path == 0 ? 1 : 0, 0x0, 0xf0000, 0x1);
    bool txkfail = false;
    if (_iqk.iqk_band == IQK_BAND_2G) {
      _iqk.is_nbiqk = true;
      txkfail = iqk_2g_txk(path);
      if (!txkfail)
        iqk_2g_rxk(path);
    } else {
      _iqk.is_nbiqk = false;
      txkfail = iqk_5g_txk(path);
      if (!txkfail)
        iqk_5g_rxk(path);
    }
  }
  for (uint8_t path = 0; path < SS_8822E; ++path) {
    if (!_iqk.lok_fail[path])
      _iqk.lok_idac[0][path] = rf_read(path, 0x58, 0xfffff);
    else
      _iqk.lok_idac[0][path] = 0x84220;

    if (!_iqk.iqk_fail_report[0][path][0]) {
      iqk_backup_cfir(path, IQK_TXIQK);
    } else {
      bb_set(0x1b00, 0x00000006, path);
      bb_write(0x1b38, 0x40000000);
      bb_set(0x1b70, 1u << 8, 0x0);
      bb_write(0x1b3c, 0x40000000);
      bb_set(0x1b70, 1u << 0, 0x0);
    }
    if (!_iqk.iqk_fail_report[0][path][1]) {
      iqk_backup_cfir(path, IQK_RXIQK2);
    } else {
      bb_set(0x1b00, 0x00000006, path);
      bb_write(0x1b3c, 0x40000000);
      bb_set(0x1b70, 1u << 0, 0x0);
    }
  }
}

void Halrf8822e::iqk_start_iqk() {
  iqk_set_gnt_wl_gnt_bt(true);
  iqk_by_path();
  iqk_set_gnt_wl_gnt_bt(false);
}

void Halrf8822e::iqk_information(ChannelWidth_t bw) {
  _iqk.rf_reg18 = rf_read(0, 0x18, 0xfffff);
  _iqk.iqk_band = static_cast<uint8_t>((_iqk.rf_reg18 & (1u << 16)) >> 16); /* 0/1 = G/A */
  _iqk.iqk_ch = static_cast<uint8_t>(_iqk.rf_reg18 & 0xff);
  switch ((_iqk.rf_reg18 & 0x3000) >> 12) {
  case 0x3: _iqk.iqk_bw = 0x0; break;            /* 20M */
  case 0x2: _iqk.iqk_bw = 0x1; break;            /* 40M */
  case 0x1: _iqk.iqk_bw = 0x2; break;            /* 80M */
  default:  _iqk.iqk_bw = 0x0; _iqk.is_nbiqk = true; break;
  }
  if (bw == CHANNEL_WIDTH_5 || bw == CHANNEL_WIDTH_10)
    _iqk.is_nbiqk = true;
}

void Halrf8822e::iqk_init() {
  _iqk.is_nbiqk = false;
  _iqk.fail_step = 0;
  _iqk.iqk_times = 0;
  _iqk.kcount = 0;
  _iqk.fail_count = 0;
  if (_iqk_inited)
    return;
  _iqk_inited = true;
  for (int i = 0; i < SS_8822E; ++i) {
    _iqk.lok_fail[i] = true;
    for (int j = 0; j < 2; ++j) {
      _iqk.iqk_fail[j][i] = true;
      _iqk.iqc_matrix[j][i] = 0x20000000;
    }
  }
  for (int i = 0; i < 2; ++i) {
    _iqk.iqk_tab[i] = 0x0;
    for (int j = 0; j < SS_8822E; ++j) {
      _iqk.lok_idac[i][j] = 0x84220;
      for (int k = 0; k < 2; ++k) {
        _iqk.iqk_fail_report[i][j][k] = true;
        for (int m = 0; m < 17; ++m) {
          _iqk.iqk_cfir_real[i][j][k][m] = 0x0;
          _iqk.iqk_cfir_imag[i][j][k][m] = 0x0;
        }
      }
    }
  }
}

void Halrf8822e::phy_iq_calibrate(ChannelWidth_t bw, uint8_t channel) {
  iqk_init();
  iqk_information(bw);

  _iqk.iqk_step = 0;
  _iqk.fail_step = 0;
  _iqk.iqk_times++;
  _iqk.kcount++;
  _iqk.tmp_gntwl = btc_read_indirect(0x38);

  iqk_backup_mac_bb();
  iqk_backup_rf();
  iqk_switch_table();

  for (int i = 0; i < 2; ++i) {
    iqk_preset();
    iqk_macbb_setting();
    iqk_afe_setting();
    iqk_start_iqk();
    iqk_afe_restore();
    iqk_macbb_restore();
    if (!_iqk.iqk_fail_report[0][0][0] && !_iqk.iqk_fail_report[0][1][0])
      break;
  }
  iqk_restore_rf();
  iqk_restore_mac_bb();
  if (_iqk.fail_step != 0x0)
    _iqk.fail_count++;
  /* fail_step bits: 0=LOK1 1=TXK 2=RX-gain-search 3=RXK (per path, OR'd). */
  char buf[8];
  std::snprintf(buf, sizeof(buf), "0x%02x", _iqk.fail_step);
  _logger->info("Jaguar3(8822e): IQK done (times=" +
                std::to_string(_iqk.iqk_times) + " fail_step=" + buf + ")");

  /* TX gain calibration — sets the 8822E TX gain table (the gross 5 GHz gain).
   * Runs after IQK on the tuned channel (halrf_init order: IQK then TXGAPK). */
  if (_skip_txgapk)
    _logger->info("Jaguar3(8822e): TXGAPK SKIPPED (debug)");
  else
    do_txgapk(channel);
}

/* 8822e thermal delta-swing tables (halhwimg8822e_rf.c _txpowertrack), 5 GHz
 * only: [band][delta], band 0 = ch36-64, 1 = ch100-144, 2 = ch149-177; delta
 * 0..29. _p = temperature-up (add power), _n = temperature-down (subtract). */
namespace {
constexpr int D_S = 30;
const uint8_t k5ga_p[3][D_S] = {
  {0,1,1,2,2,3,3,3,4,4,5,5,6,6,7,7,8,8,8,9,9,10,10,11,11,12,12,13,13,13},
  {0,1,2,2,3,4,4,5,6,7,7,8,9,9,10,11,12,12,13,14,14,15,16,17,17,18,19,19,20,21},
  {0,1,2,3,3,4,5,6,6,7,8,9,9,10,11,12,12,13,14,15,15,16,17,18,18,19,20,21,22,22}};
const uint8_t k5ga_n[3][D_S] = {
  {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
  {0,1,1,2,2,2,3,3,4,4,5,5,5,6,6,7,7,8,8,9,9,9,10,10,11,11,12,12,12,13},
  {0,1,1,1,2,2,2,3,3,4,4,4,5,5,5,6,6,6,7,7,8,8,8,9,9,9,10,10,10,11}};
const uint8_t k5gb_p[3][D_S] = {
  {0,1,1,2,3,3,4,4,5,5,6,7,7,8,8,9,9,10,11,11,12,12,13,14,14,15,15,16,16,17},
  {0,1,2,3,3,4,5,6,7,8,8,9,10,11,12,12,13,14,15,16,16,17,18,19,20,21,21,22,23,24},
  {0,1,2,2,3,4,4,5,6,6,7,8,8,9,10,11,11,12,13,13,14,15,15,16,17,17,18,19,19,20}};
const uint8_t k5gb_n[3][D_S] = {
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,1,1,2,2,3,3,4,4,5,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,14},
  {0,1,1,1,2,2,2,3,3,4,4,4,5,5,5,6,6,7,7,7,8,8,8,9,9,10,10,10,11,11}};
} /* namespace */

void Halrf8822e::set_pwr_track_ctx(uint8_t thermal_base_a, uint8_t thermal_base_b,
                                   uint8_t channel) {
  _therm_base[0] = thermal_base_a;
  _therm_base[1] = thermal_base_b;
  _track_channel = channel;
  _tm_triggered = false;
  _therm_avg_cnt[0] = _therm_avg_cnt[1] = 0;
  _therm_avg_idx[0] = _therm_avg_idx[1] = 0;
}

void Halrf8822e::pwr_track() { thermal_track_8822e(); }

bool Halrf8822e::read_thermal(uint8_t &raw, uint8_t &baseline) {
  /* Same one-time meter enable the tracker uses (RF 0x42[19] 1->0->1 per
   * path, then the meter free-runs). Caller serializes against pwr_track. */
  if (!_tm_triggered) {
    for (uint8_t p = 0; p < 2; ++p) {
      rf_write(p, 0x42, 1u << 19, 1);
      rf_write(p, 0x42, 1u << 19, 0);
      rf_write(p, 0x42, 1u << 19, 1);
    }
    _tm_triggered = true;
    delay_us(300);
  }
  raw = static_cast<uint8_t>(rf_read(0, 0x42, 0x7e)); /* RF 0x42[6:1] */
  baseline = _therm_base[0];                          /* efuse 0xd0 */
  return raw != 0; /* 0 = meter not ready this cycle */
}

void Halrf8822e::thermal_track_8822e() {
  /* 5 GHz only (2.4 G swing tables not wired); need a valid efuse baseline. */
  const uint8_t ch = _track_channel;
  if (ch <= 14)
    return;
  if (_therm_base[0] == 0xFF && _therm_base[1] == 0xFF)
    return;
  const int band = ch <= 64 ? 0 : ch <= 144 ? 1 : 2;

  /* One-time enable/trigger of the thermal meter (RF 0x42[19] 1->0->1 per path),
   * then the meter free-runs (port of the tm_trigger block). */
  if (!_tm_triggered) {
    for (uint8_t p = 0; p < 2; ++p) {
      rf_write(p, 0x42, 1u << 19, 1);
      rf_write(p, 0x42, 1u << 19, 0);
      rf_write(p, 0x42, 1u << 19, 1);
    }
    _tm_triggered = true;
    delay_us(300);
  }

  for (uint8_t p = 0; p < 2; ++p) {
    const uint8_t base = _therm_base[p];
    if (base == 0xFF)
      continue;
    uint8_t cur = static_cast<uint8_t>(rf_read(p, 0x42, 0x7e)); /* RF 0x42[6:1] */
    if (cur == 0)
      continue; /* meter not ready this cycle */
    /* 4-sample rolling average (AVG_THERMAL_NUM_8822E). */
    _therm_avg[p][_therm_avg_idx[p]] = cur;
    _therm_avg_idx[p] = static_cast<uint8_t>((_therm_avg_idx[p] + 1) & 3);
    if (_therm_avg_cnt[p] < 4)
      ++_therm_avg_cnt[p];
    uint32_t sum = 0;
    for (uint8_t i = 0; i < _therm_avg_cnt[p]; ++i)
      sum += _therm_avg[p][i];
    uint8_t avg = static_cast<uint8_t>(sum / _therm_avg_cnt[p]);

    int delta = avg > base ? (avg - base) : (base - avg);
    if (delta >= D_S)
      delta = D_S - 1;
    const uint8_t(*up)[D_S] = p == 0 ? k5ga_p : k5gb_p;
    const uint8_t(*dn)[D_S] = p == 0 ? k5ga_n : k5gb_n;
    int swing = avg > base ? static_cast<int>(up[band][delta])
                           : -static_cast<int>(dn[band][delta]);
    /* absolute_ofdm_swing_idx -> R_0x18a0/0x41a0[7:0] (signed 8-bit). */
    bb_set(p == 0 ? 0x18a0 : 0x41a0, 0x000000ff,
           static_cast<uint32_t>(swing) & 0xff);
    if (swing != _last_swing[p]) { /* log only on change (2 s cadence otherwise spams) */
      _last_swing[p] = swing;
      _logger->info("Jaguar3(8822e): pwr_track path{} thermal={} base={} "
                    "delta={} swing={}",
                    p, avg, base, delta, swing);
    }
  }
}

/* Coex / antenna control — port of the 8822c WiFi-only coex (Halrf8822c). Even
 * though the EU parts have no BT combo, the chip still powers up with the
 * LTE/BTC coex arbitration ENABLED, which gates the WL TRX path: with it left at
 * defaults the WL RX (and TX) is masked, so the receiver delivers zero frames.
 * Disabling the arbitration (LTE_COEX_EN=0, WL/BT TRX-CTRL all-pass) and forcing
 * the antenna to WL is what connects the RX path. Same MAC/BTC registers as the
 * 8822c (BTC indirect window 0x38/0xa0/0xa4, antenna owner 0x73, scoreboard
 * 0xAA) and the same 8822-BB gnt-fix addresses. */
void Halrf8822e::force_wl_antenna() {
  _device.rtw_write<uint16_t>(0xAA, 0x8003); /* WL scoreboard active|on|BT_INT_EN */
  iqk_set_gnt_wl_high();                     /* GNT_WL SW-high (0x38[15:8]=0x77) */
  mac_write8(0x73, static_cast<uint8_t>(mac_read8(0x73) | 0x04)); /* owner = WL */
}

void Halrf8822e::coex_wlan_only_init() {
  auto w8m = [&](uint16_t reg, uint8_t mask, uint8_t val) {
    uint8_t o = mac_read8(reg);
    uint8_t sh = 0;
    for (uint8_t m = mask; m && !(m & 1); m >>= 1)
      ++sh;
    o = static_cast<uint8_t>((o & ~mask) | ((val << sh) & mask));
    mac_write8(reg, o);
  };
  auto set32 = [&](uint16_t reg, uint32_t bits) {
    _device.rtw_write<uint32_t>(reg, _device.rtw_read<uint32_t>(reg) | bits);
  };

  /* rtw8822c_coex_cfg_init: base PTA hardware config */
  w8m(0x550, 0x08, 1);
  w8m(0x790, 0x3f, 0x5);
  mac_write8(0x778, 0x1);
  set32(0x40, (1u << 5) | (1u << 9));
  w8m(0x4c6, 0x10, 1);
  w8m(0x4c6, 0x20, 0);
  _device.rtw_write<uint16_t>(
      0x762, static_cast<uint16_t>(_device.rtw_read<uint16_t>(0x762) | (1u << 12)));
  w8m(0x4fc, 0x03, 0);

  /* cfg_rfe_type: disable LTE/BT coex arbitration, WL/BT TRX all-pass */
  btc_write_indirect(0x38, 0x80, 0x0);
  btc_write_indirect(0xa0, 0xffff, 0xffff);
  btc_write_indirect(0xa4, 0xffff, 0xffff);

  /* cfg_gnt_fix: WL TX/RX must not be masked by GNT_BT */
  rf_write(1, 0x01, RFREG_MASK, 0x42000);
  w8m(0x1c32, 0x40, 1);
  w8m(0x1c39, 0x10, 0);
  w8m(0x1c3b, 0x10, 1);
  w8m(0x4160, 0x08, 1);
  w8m(0x1860, 0x08, 0);
  w8m(0x1ca7, 0x08, 1);

  force_wl_antenna(); /* COEX_SET_ANT_WONLY */
  _logger->info("Jaguar3(8822e): coex WiFi-only init applied (antenna -> WL)");
}

void Halrf8822e::coex_run_5g() {
  btc_write_indirect(0x38, 0xc000, 0x0);
  btc_write_indirect(0x38, 0x0c00, 0x0);
  btc_write_indirect(0x38, 0x3000, 0x3);
  btc_write_indirect(0x38, 0x0300, 0x3);
  mac_write8(0x73, static_cast<uint8_t>(mac_read8(0x73) | 0x04));
  _device.rtw_write<uint32_t>(0x6c0, 0xffffffffu);
  _device.rtw_write<uint32_t>(0x6c4, 0xffffffffu);
  _device.rtw_write<uint32_t>(0x6c8, 0xf0ffffffu);
  _device.rtw_write<uint16_t>(0xAA, 0x8003);
}

void Halrf8822e::coex_keepalive() {
  btc_write_indirect(0x38, 0x80, 0x0);
  btc_write_indirect(0xa0, 0xffff, 0xffff);
  btc_write_indirect(0xa4, 0xffff, 0xffff);
  force_wl_antenna();
}

/* ===== TXGAPK (TX gain calibration) — port of halrf_txgapk_8822e.c =====
 * Per-path BB/AFE-IQK gain calibration that sets the 8822E TX gain table; the
 * gross 5 GHz TX gain depends on it. Chunk 1: register-sequence backup/restore +
 * BB/AFE IQK setup (faithful 1:1; the offset-calc + gain-table write follow). */

void Halrf8822e::txgapk_backup_bb(const uint16_t *reg, uint32_t *backup,
                                  uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    backup[i] = bb_read(reg[i]);
}
void Halrf8822e::txgapk_reload_bb(const uint16_t *reg, const uint32_t *backup,
                                  uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    bb_write(reg[i], backup[i]);
}
void Halrf8822e::txgapk_backup_kip(const uint16_t *reg, uint32_t backup[][2],
                                   uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    for (uint32_t j = 0; j < SS_8822E; ++j) {
      bb_set(0x1b00, (1u << 2) | (1u << 1), j);
      backup[i][j] = bb_read(reg[i]);
    }
}
void Halrf8822e::txgapk_reload_kip(const uint16_t *reg,
                                   const uint32_t backup[][2], uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    for (uint32_t j = 0; j < SS_8822E; ++j) {
      bb_set(0x1b00, (1u << 2) | (1u << 1), j);
      bb_write(reg[i], backup[i][j]);
    }
}

void Halrf8822e::txgapk_tx_pause() {
  mac_write8(0x522, 0xff);
  bb_set(0x1e70, 0x0000000f, 0x2); /* hw tx stop */
  uint8_t a = static_cast<uint8_t>(rf_read(0, 0x00, 0xF0000));
  uint8_t b = static_cast<uint8_t>(rf_read(1, 0x00, 0xF0000));
  uint16_t count = 0;
  while (((a == 2) || (b == 2)) && count < 2500) {
    a = static_cast<uint8_t>(rf_read(0, 0x00, 0xF0000));
    b = static_cast<uint8_t>(rf_read(1, 0x00, 0xF0000));
    delay_us(2);
    ++count;
  }
}

void Halrf8822e::txgapk_bb_iqk(uint8_t path) {
  bb_set(0x1e24, 0x00020000, 0x1);
  bb_set(0x1cd0, 0x10000000, 0x1);
  bb_set(0x1cd0, 0x20000000, 0x1);
  bb_set(0x1cd0, 0x40000000, 0x1);
  bb_set(0x1cd0, 0x80000000, 0x0);
  if (path == 0) {
    bb_set(0x1864, 0x80000000, 0x1);
    bb_set(0x180c, 0x08000000, 0x1);
    bb_set(0x186c, 0x00000080, 0x1);
    bb_set(0x180c, 0x00000003, 0x0);
  } else {
    bb_set(0x4164, 0x80000000, 0x1);
    bb_set(0x410c, 0x08000000, 0x1);
    bb_set(0x416c, 0x00000080, 0x1);
    bb_set(0x410c, 0x00000003, 0x0);
  }
  bb_set(0x1a00, 0x00000003, 0x2);
  bb_write(0x1b08, 0x00000080);
}

void Halrf8822e::txgapk_afe_iqk(uint8_t path) {
  const uint16_t reg = (path == 0) ? 0x1830 : 0x4130;
  bb_write(0x1c38, 0xffffffff);
  bb_write(reg, 0x700f0001);
  for (uint32_t v = 0x0; v <= 0xf; ++v)
    bb_write(reg, 0x700f0001 | (v << 20));
  bb_write(reg, 0x70ff0001);
  iqk_ex_dac_fifo_rst();
}

void Halrf8822e::txgapk_afe_iqk_restore(uint8_t path) {
  const uint16_t reg = (path == 0) ? 0x1830 : 0x4130;
  static const uint32_t vals[16] = {
      0x700b8041, 0x70144041, 0x70244041, 0x70344041, 0x70444041, 0x705b8041,
      0x70644041, 0x707b8041, 0x708b8041, 0x709b8041, 0x70ab8041, 0x70bb8041,
      0x70cb8041, 0x70db8041, 0x70eb8041, 0x70fb8041};
  bb_write(0x1c38, 0xffa1005e);
  for (uint32_t v : vals)
    bb_write(reg, v);
  iqk_ex_dac_fifo_rst();
}

void Halrf8822e::txgapk_bb_iqk_restore(uint8_t path) {
  rf_write(path, 0xde, 0x10000, 0x0);
  bb_set(0x1b00, 0x00000006, 0x0);
  bb_write(0x1b08, 0x00000000);
  bb_set(0x1d0c, 0x00010000, 0x1);
  bb_set(0x1d0c, 0x00010000, 0x0);
  bb_set(0x1d0c, 0x00010000, 0x1);
  if (path == 0) {
    bb_set(0x1864, 0x80000000, 0x0);
    bb_set(0x180c, 0x08000000, 0x0);
    bb_set(0x186c, 0x00000080, 0x0);
    bb_set(0x180c, 0x00000003, 0x3);
  } else {
    bb_set(0x4164, 0x80000000, 0x0);
    bb_set(0x410c, 0x08000000, 0x0);
    bb_set(0x416c, 0x00000080, 0x0);
    bb_set(0x410c, 0x00000003, 0x3);
  }
  bb_set(0x1a00, 0x00000003, 0x0);
}

/* Measure the per-gain-index TX offsets for one path (port of
 * _halrf_txgapk_calculate_offset_8822e). Runs the gain-K one-shot and reads the
 * 10 4-bit offsets (sign-extended) from 0x1bfc into _txgapk.offset[0..9][path].
 * 2G and 5G differ only in a few RF presets + the 0x1b98 band value. */
void Halrf8822e::txgapk_calculate_offset(uint8_t path, uint8_t channel) {
  const uint16_t set_pi[2] = {0x1c, 0xec};       /* MAC PI-disable field [31:30] */
  const uint32_t set_1b00_cfg1[2] = {0x00000d19, 0x00000d29};
  const bool is_2g = channel <= 14;

  if (is_2g)
    iqk_set_gnt_wl_gnt_bt(true);

  bb_set(0x1b00, 0x00000006, path);
  rf_write(path, 0xde, 0x10000, 0x1);
  rf_write(path, 0x00, 0xf0000, 0x5);
  if (is_2g) {
    rf_write(path, 0x88, 0x00070, 0x1);
    rf_write(path, 0x88, 0x0000f, 0x1);
    rf_write(path, 0xdf, 0x10000, 0x1);
    rf_write(path, 0x87, 0xc0000, 0x3);
    rf_write(path, 0x00, 0x003e0, 0x0f);
    bb_set(0x1b98, 0x00007000, 0x0);
  } else {
    rf_write(path, 0x8b, 0x00700, 0x0);
    rf_write(path, 0xdf, 0x20000, 0x1);
    rf_write(path, 0x89, 0x00003, 0x3);
    rf_write(path, 0x00, 0x003e0, 0x0f);
    /* Extended-range clamps: below-band 15..35 rides the low bucket, >177
     * keeps the top bucket (the synth tunes past the vendor tables). */
    if (channel <= 64)
      bb_set(0x1b98, 0x00007000, 0x2);
    else if (channel >= 100 && channel <= 144)
      bb_set(0x1b98, 0x00007000, 0x3);
    else if (channel >= 149)
      bb_set(0x1b98, 0x00007000, 0x4);
  }

  uint32_t backup_pi = _device.rtw_read32(set_pi[path]) & 0xc0000000;
  _device.rtw_write32(set_pi[path],
                      _device.rtw_read32(set_pi[path]) & ~0xc0000000u);
  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1bcc, 0x0000003f, 0x12);
  bb_set(0x1b2c, 0x00000fff, 0x038);
  bb_write(0x1b00, set_1b00_cfg1[path]);
  delay_us(10000);

  for (int i = 0; i < 30; ++i) {
    delay_us(100);
    if (bb_get(0x2d9c, 0x000000ff) == 0x55) break;
  }
  for (int i = 0; i < 30; ++i) {
    delay_us(100);
    if (bb_get(0x1bfc, 0x0000ffff) == 0x8000) break;
  }

  bb_set(0x1b10, 0x000000ff, 0x00);
  delay_us(100);
  _device.rtw_write32(set_pi[path],
                      (_device.rtw_read32(set_pi[path]) & ~0xc0000000u) | backup_pi);

  if (is_2g)
    iqk_set_gnt_wl_gnt_bt(false);

  bb_set(0x1b00, 0x00000006, path);
  bb_set(0x1bd4, 0x00200000, 0x1);
  bb_set(0x1bd4, 0x001f0000, 0x12);

  bb_set(0x1b9c, 0x00000f00, 0x3);
  uint32_t tmp = bb_get(0x1bfc, 0xffffffff);
  for (int i = 0; i < 8; ++i)
    _txgapk.offset[i][path] = static_cast<int8_t>((tmp >> (i * 4)) & 0xf);
  bb_set(0x1b9c, 0x00000f00, 0x4);
  tmp = bb_get(0x1bfc, 0x000000ff);
  _txgapk.offset[8][path] = static_cast<int8_t>(tmp & 0xf);
  _txgapk.offset[9][path] = static_cast<int8_t>((tmp & 0xf0) >> 4);
  for (int i = 0; i < 10; ++i)
    if (_txgapk.offset[i][path] & 0x8) /* sign-extend 4-bit */
      _txgapk.offset[i][path] = static_cast<int8_t>(_txgapk.offset[i][path] | 0xf0);
}

void Halrf8822e::txgapk_rf_restore(uint8_t path) {
  rf_write(path, 0x00, 0xf0000, 0x3);
  rf_write(path, 0xde, 0x10000, 0x0);
  rf_write(path, 0xdf, 0x30000, 0x0);
}

/* Apply a half-dB TX offset to an RF gain codeword (port of
 * _halrf_txgapk_calculat_tx_gain_8822e). offset is in half-steps; the +0x1000
 * carry handles the gain-table's split integer/fraction encoding. */
uint32_t Halrf8822e::txgapk_calc_tx_gain(uint32_t original, int8_t offset) {
  uint32_t g = original;
  if (offset < 0) {
    if ((offset % 2) == 0)
      g = g + (offset / 2);
    else
      g = g + 0x1000 + (offset / 2) - 1;
  } else {
    if ((offset % 2) == 0)
      g = g + (offset / 2);
    else
      g = g + 0x1000 + (offset / 2);
  }
  return g;
}

/* Write the saved per-band gain table into the BB gain-table SRAM via 0x1b98/
 * 0x1b9c (port of _halrf_txgapk_write_gain_bb_table_8822e, active #else branch). */
void Halrf8822e::txgapk_write_gain_bb_table() {
  for (int band = 0; band < 5; ++band) {
    for (uint8_t path = 0; path < SS_8822E; ++path) {
      bb_set(0x1b00, 0x00000006, path);
      if (band == 1)
        bb_set(0x1b98, 0x00007000, 0x0);
      else if (band == 2)
        bb_set(0x1b98, 0x00007000, 0x2);
      else if (band == 3)
        bb_set(0x1b98, 0x00007000, 0x3);
      else if (band == 4)
        bb_set(0x1b98, 0x00007000, 0x4);
      bb_set(0x1b9c, 0x000000ff, 0x88);
      for (int gain = 0; gain < 11; ++gain) {
        uint32_t tmp_3f = _txgapk.rf3f_bp[band][gain][path] & 0xfff;
        bb_set(0x1b98, 0x00000fff, tmp_3f);
        bb_set(0x1b98, 0x000f0000, gain);
        bb_set(0x1b98, 0x00008000, 0x1);
        bb_set(0x1b98, 0x00008000, 0x0);
      }
    }
  }
}

/* Read every band's RF TX gain table into _txgapk.rf3f_bp and derive rf3f_same
 * (consecutive equal [11:5] gains), then prime the BB gain table. Port of
 * halrf_txgapk_save_all_tx_gain_table_8822e. Runs once (read_txgain latch). */
void Halrf8822e::txgapk_save_all() {
  const uint16_t three_wire[2] = {0x180c, 0x410c};
  const uint8_t ch_num[5] = {1, 1, 36, 100, 149};
  const uint8_t ch_setting[5] = {0, 0, 1, 1, 1};
  const uint8_t band_num[5] = {0x0, 0x0, 0x1, 0x3, 0x5};
  const uint8_t cck[5] = {0x1, 0x0, 0x0, 0x0, 0x0};

  if (_txgapk.read_txgain == 1) {
    txgapk_write_gain_bb_table();
    return;
  }

  for (int band = 0; band < 5; ++band) {
    for (uint8_t path = 0; path < SS_8822E; ++path) {
      uint32_t rf18 = rf_read(path, 0x18, 0xfffff);
      bb_set(three_wire[path], 0x00000003, 0x0);
      rf_write(path, 0x18, 0x000ff, ch_num[band]);
      rf_write(path, 0x18, 0x70000, band_num[band]);
      rf_write(path, 0x18, 0x00100, ch_setting[band]);
      rf_write(path, 0x1a, 0x00001, cck[band]);
      rf_write(path, 0x1a, 0x10000, cck[band]);
      uint8_t gain = 0;
      for (uint32_t rf0 = 1; rf0 < 32; rf0 += 3) {
        rf_write(path, 0x0, 0x000ff, rf0);
        _txgapk.rf3f_bp[band][gain][path] = rf_read(path, 0x5f, 0xfffff);
        ++gain;
      }
      rf_write(path, 0x18, 0xfffff, rf18);
      bb_set(three_wire[path], 0x00000003, 0x3);
      if (_gaintab_dbg && path == 0)
        _logger->info("Jaguar3(8822e): gaintab band{} p0 = {:03x} {:03x} {:03x} "
                      "{:03x} {:03x} {:03x} {:03x} {:03x} {:03x} {:03x} {:03x}",
                      band, _txgapk.rf3f_bp[band][0][0] & 0xfff,
                      _txgapk.rf3f_bp[band][1][0] & 0xfff,
                      _txgapk.rf3f_bp[band][2][0] & 0xfff,
                      _txgapk.rf3f_bp[band][3][0] & 0xfff,
                      _txgapk.rf3f_bp[band][4][0] & 0xfff,
                      _txgapk.rf3f_bp[band][5][0] & 0xfff,
                      _txgapk.rf3f_bp[band][6][0] & 0xfff,
                      _txgapk.rf3f_bp[band][7][0] & 0xfff,
                      _txgapk.rf3f_bp[band][8][0] & 0xfff,
                      _txgapk.rf3f_bp[band][9][0] & 0xfff,
                      _txgapk.rf3f_bp[band][10][0] & 0xfff);
    }
  }

  txgapk_write_gain_bb_table();

  for (int band = 0; band < 5; ++band)
    for (uint8_t path = 0; path < SS_8822E; ++path)
      for (int gain = 0; gain < TXGAPK_GAIN_NUM - 1; ++gain)
        _txgapk.rf3f_same[band][gain][path] =
            ((_txgapk.rf3f_bp[band][gain][path] & 0xfe0) ==
             (_txgapk.rf3f_bp[band][gain + 1][path] & 0xfe0))
                ? 1
                : 0;

  _txgapk.read_txgain = 1;
}

/* Apply the measured offsets to the RF TX gain table for the active band (port
 * of _halrf_txgapk_write_tx_gain_8822e). */
void Halrf8822e::txgapk_write_tx_gain(uint8_t channel) {
  uint32_t base = 0x20;
  int band = 1;
  /* Extended-range clamps: 15..35 → low 5G band, >177 → top band. */
  if (channel >= 1 && channel <= 14) { base = 0x20; band = 1; }
  else if (channel >= 15 && channel <= 64) { base = 0x200; band = 2; }
  else if (channel >= 100 && channel <= 144) { base = 0x280; band = 3; }
  else if (channel >= 149) { base = 0x300; band = 4; }

  for (uint8_t path = 0; path < SS_8822E; ++path) {
    /* Guard: if the saved RF gain table for this band read back all-zero, the
     * gain-table readback failed — writing calc_tx_gain(0,offset) back would
     * ZERO the live RF gain table and kill TX for the band. Skip the write-back
     * and leave the radioa-loaded gains intact. (Root-caused on EU 5 GHz: the
     * 5 GHz save_all readback returns 0 while 2 GHz reads non-zero.) */
    bool all_zero = true;
    for (int g = 0; g < 11 && all_zero; ++g)
      if ((_txgapk.rf3f_bp[band][g][path] & 0xfff) != 0)
        all_zero = false;
    if (all_zero) {
      _logger->info("Jaguar3(8822e): TXGAPK skip write-back band{} path{} "
                    "(gain readback all-zero — keeping radioa gains)", band, path);
      continue;
    }
    int8_t off_tmp[11] = {};
    for (int i = 0; i < 10; ++i) {
      off_tmp[i] = 0;
      for (int j = i; j < 10; ++j) {
        if (_txgapk.rf3f_same[band][j][path])
          continue;
        off_tmp[i] = static_cast<int8_t>(off_tmp[i] + _txgapk.offset[j][path]);
        _txgapk.final_offset[i][path] = off_tmp[i];
      }
    }
    rf_write(path, 0xee, 0xfffff, 0x10000);
    uint32_t j = 0;
    for (uint32_t i = base; i <= base + 10; ++i) {
      rf_write(path, 0x33, 0xfffff, i);
      uint32_t tmp_3f =
          txgapk_calc_tx_gain(_txgapk.rf3f_bp[band][j][path], off_tmp[j]) & 0x01fff;
      rf_write(path, 0x3f, 0x7ffff, tmp_3f << 6);
      ++j;
    }
    rf_write(path, 0xee, 0xfffff, 0x0);
  }
}

/* Orchestrator — port of halrf_txgapk_8822e: prime the gain table once, then for
 * the tuned channel run per-path BB/AFE-IQK gain measurement and write corrected
 * gains. Backs up/restores the BB + KIP registers it perturbs. */
void Halrf8822e::do_txgapk(uint8_t channel) {
  txgapk_save_all(); /* primes rf3f_bp + read_txgain (once) */
  if (!_txgapk.read_txgain)
    return;

  const uint16_t bb_reg[3] = {0x520, 0x1e70, 0x1b00};
  uint32_t bb_bak[3];
  const uint16_t kip_reg[2] = {0x1b38, 0x1b20};
  uint32_t kip_bak[2][2];
  txgapk_backup_bb(bb_reg, bb_bak, 3);
  txgapk_backup_kip(kip_reg, kip_bak, 2);

  txgapk_tx_pause();
  for (uint8_t path = 0; path < SS_8822E; ++path) {
    txgapk_bb_iqk(path);
    txgapk_afe_iqk(path);
    txgapk_calculate_offset(path, channel);
    txgapk_rf_restore(path);
    txgapk_afe_iqk_restore(path);
    txgapk_bb_iqk_restore(path);
  }
  txgapk_write_tx_gain(channel);

  txgapk_reload_kip(kip_reg, kip_bak, 2);
  txgapk_reload_bb(bb_reg, bb_bak, 3);
  _txgapk.is_txgapk_ok = true;
  _logger->info("Jaguar3(8822e): TXGAPK done (ch=" + std::to_string(channel) + ")");
}

/* Maker, dispatched from make_jaguar3_calibration() in Halrf8822c.cpp. */
std::unique_ptr<Jaguar3Calibration>
make_halrf_8822e(RtlAdapter device, Logger_t logger,
                 const devourer::DeviceConfig &cfg) {
  return std::make_unique<Halrf8822e>(device, logger, cfg);
}

} /* namespace jaguar3 */
