#include "Halrf8822b.h"

#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace jaguar2 {

namespace {
constexpr uint32_t MASK20 = 0x000FFFFF; /* MASK20BITS / RFREGOFFSETMASK */
constexpr uint8_t TXIQK = 0, RXIQK1 = 1, RXIQK2 = 2; /* one-shot idx / retry idx */
constexpr uint8_t rxiqk_gs_limit = 6;
void udelay(uint32_t us) {
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}
uint32_t bshift(uint32_t mask) {
  if (mask == 0)
    return 0;
  uint32_t s = 0;
  while (!((mask >> s) & 1u))
    s++;
  return s;
}
} /* namespace */

Halrf8822b::Halrf8822b(RtlAdapter device, Logger_t logger, uint8_t cut,
                       bool is_2t2r)
    : _device{std::move(device)}, _logger{std::move(logger)}, _cut{cut},
      _2t2r{is_2t2r} {}

/* --- BB access (odm_get/set_bb_reg) --- */
uint32_t Halrf8822b::bb_get(uint16_t addr, uint32_t mask) {
  return (_device.rtw_read32(addr) & mask) >> bshift(mask);
}
void Halrf8822b::bb_set(uint16_t addr, uint32_t mask, uint32_t data) {
  _device.phy_set_bb_reg(addr, mask, data);
}

/* --- RF access: read = direct BB window (config_phydm_read_rf_reg_8822b);
 * write = 3-wire LSSI (config_phydm_write_rf_reg_8822b, 0xC90/0xE90). --- */
uint32_t Halrf8822b::rf_get(uint8_t path, uint32_t addr) {
  const uint32_t base = (path == 0) ? 0x2800u : 0x2c00u;
  const uint16_t direct = static_cast<uint16_t>(base + ((addr & 0xff) << 2));
  return _device.rtw_read32(direct) & MASK20;
}
void Halrf8822b::rf_set(uint8_t path, uint32_t addr, uint32_t mask,
                        uint32_t data) {
  if (mask == 0)
    return;
  uint32_t v = data;
  if (mask != MASK20) {
    uint32_t orig = rf_get(path, addr);
    v = (orig & ~mask) | (data << bshift(mask));
  }
  const uint16_t lssi = (path == 0) ? 0x0C90 : 0x0E90;
  uint32_t data_and_addr = (((addr & 0xff) << 20) | (v & MASK20)) & 0x0fffffffu;
  _device.phy_set_bb_reg(lssi, 0xFFFFFFFFu, data_and_addr);
}
/* _iqk_rf_set_check_8822b: write then verify (RF write can be flaky). */
void Halrf8822b::rf_set_check(uint8_t path, uint16_t addr, uint32_t data) {
  rf_set(path, addr, MASK20, data);
  for (int i = 0; i < 100; i++) {
    if (rf_get(path, addr) == data)
      break;
    udelay(10);
    rf_set(path, addr, MASK20, data);
  }
}

/* --- backup / restore (_iqk_backup_*, _iqk_restore_*) --- */
void Halrf8822b::backup_mac_bb(uint32_t *mac_bk, uint32_t *bb_bk,
                               const uint32_t *mac_reg,
                               const uint32_t *bb_reg) {
  for (int i = 0; i < 2; i++)
    mac_bk[i] = r32(static_cast<uint16_t>(mac_reg[i]));
  for (int i = 0; i < 21; i++)
    bb_bk[i] = r32(static_cast<uint16_t>(bb_reg[i]));
}
void Halrf8822b::backup_rf(uint32_t rf_bk[][2], const uint32_t *reg) {
  for (int i = 0; i < 5; i++) {
    rf_bk[i][0] = rf_get(0, reg[i]);
    rf_bk[i][1] = rf_get(1, reg[i]);
  }
}
void Halrf8822b::restore_mac_bb(const uint32_t *mac_bk, const uint32_t *bb_bk,
                                const uint32_t *mac_reg,
                                const uint32_t *bb_reg) {
  for (int i = 0; i < 2; i++)
    w32(static_cast<uint16_t>(mac_reg[i]), mac_bk[i]);
  for (int i = 0; i < 21; i++)
    w32(static_cast<uint16_t>(bb_reg[i]), bb_bk[i]);
}
void Halrf8822b::restore_rf(const uint32_t *reg, uint32_t rf_bk[][2]) {
  rf_set(0, 0xef, MASK20, 0x0);
  rf_set(1, 0xef, MASK20, 0x0);
  rf_set_check(0, 0xdf, rf_bk[0][0] & ~(1u << 4));
  rf_set_check(1, 0xdf, rf_bk[0][1] & ~(1u << 4));
  for (int i = 1; i < 5; i++) {
    rf_set(0, reg[i], MASK20, rf_bk[i][0]);
    rf_set(1, reg[i], MASK20, rf_bk[i][1]);
  }
}

void Halrf8822b::agc_bnd_int() {
  w32(0x1b00, 0xf8000008);
  w32(0x1b00, 0xf80a7008);
  w32(0x1b00, 0xf8015008);
  w32(0x1b00, 0xf8000008);
}

void Halrf8822b::bb_reset() {
  uint32_t count = 0;
  rf_set(0, 0x0, MASK20, 0x10000);
  rf_set(1, 0x0, MASK20, 0x10000);
  bb_set(0x8f8, 0x0ff00000, 0x0);
  while (true) {
    w32(0x8fc, 0x0);
    bb_set(0x198c, 0x7, 0x7);
    bool cca_ing = bb_get(0xfa0, 1u << 3) != 0;
    if (count > 20000)
      cca_ing = false;
    if (cca_ing) {
      udelay(10);
      count++;
    } else {
      w8(0x808, 0x0);
      bb_set(0xa04, (1u << 27) | (1u << 26) | (1u << 25) | (1u << 24), 0x0);
      bb_set(0x0, 1u << 16, 0x0);
      bb_set(0x0, 1u << 16, 0x1);
      if (bb_get(0x660, 1u << 16))
        w32(0x6b4, 0x89000006);
      break;
    }
  }
}

void Halrf8822b::afe_setting(bool do_iqk) {
  if (do_iqk) {
    w32(0xc60, 0x50000000);
    w32(0xc60, 0x70070040);
    w32(0xe60, 0x50000000);
    w32(0xe60, 0x70070040);
    w32(0xc58, 0xd8000402);
    w32(0xc5c, 0xd1000120);
    w32(0xc6c, 0x00000a15);
    w32(0xe58, 0xd8000402);
    w32(0xe5c, 0xd1000120);
    w32(0xe6c, 0x00000a15);
    bb_reset();
  } else {
    w32(0xc60, 0x50000000);
    w32(0xc60, 0x70038040);
    w32(0xe60, 0x50000000);
    w32(0xe60, 0x70038040);
  }
  bb_set(0x9a4, 1u << 31, 0x0);
}

void Halrf8822b::rfe_setting(bool ext_pa_on) {
  if (ext_pa_on) {
    w32(0xcb0, 0x77777777);
    w32(0xcb4, 0x00007777);
    w32(0xcbc, 0x0000083B);
    w32(0xeb0, 0x77777777);
    w32(0xeb4, 0x00007777);
    w32(0xebc, 0x0000083B);
  } else {
    w32(0xcb0, 0x77777777);
    w32(0xcb4, 0x00007777);
    w32(0xcbc, 0x00000100);
    w32(0xeb0, 0x77777777);
    w32(0xeb4, 0x00007777);
    w32(0xebc, 0x00000100);
  }
}

void Halrf8822b::rf_setting() {
  w32(0x1b00, 0xf8000008);
  w32(0x1bb8, 0x00000000);
  for (uint8_t path = 0; path < 2; path++) {
    uint32_t tmp = rf_get(path, 0xdf);
    tmp = (tmp & ~(1u << 4)) | (1u << 1) | (1u << 11);
    rf_set_check(path, 0xdf, tmp);
    rf_set(path, 0x65, MASK20, 0x09000);
    /* 2.4G branch (5G differs only in RF 0x3f = 0xdefce vs 0x5efce) */
    rf_set(path, 0xef, 1u << 19, 0x1);
    rf_set(path, 0x33, MASK20, 0x00026);
    rf_set(path, 0x3e, MASK20, 0x00037);
    rf_set(path, 0x3f, MASK20, _band2g ? 0x5efce : 0xdefce);
    rf_set(path, 0xef, 1u << 19, 0x0);
  }
}

void Halrf8822b::configure_macbb() {
  w8(0x522, 0x7f);
  bb_set(0x550, (1u << 11) | (1u << 3), 0x0);
  bb_set(0x90c, 1u << 15, 0x1);
  bb_set(0xc94, 1u << 0, 0x1);
  bb_set(0xe94, 1u << 0, 0x1);
  bb_set(0xc94, (1u << 11) | (1u << 10), 0x1);
  bb_set(0xe94, (1u << 11) | (1u << 10), 0x1);
  w32(0xc00, 0x00000004);
  w32(0xe00, 0x00000004);
  bb_set(0xb00, 1u << 8, 0x0);
  bb_set(0x808, 1u << 28, 0x0);
  bb_set(0x838, (1u << 3) | (1u << 2) | (1u << 1), 0x7);
}

void Halrf8822b::lok_setting(uint8_t path) {
  w32(0x1b00, 0xf8000008 | (path << 1));
  w32(0x1bcc, 0x9);
  w8(0x1b23, 0x00);
  if (_band2g) {
    w8(0x1b2b, 0x00);
    rf_set(path, 0x56, MASK20, 0x50df2);
    rf_set(path, 0x8f, MASK20, 0xadc00);
    rf_set(path, 0xef, 1u << 4, 0x1);
    rf_set(path, 0x33, (1u << 1) | (1u << 0), 0x0);
  } else {
    w8(0x1b2b, 0x80);
    rf_set(path, 0x56, MASK20, 0x5086c);
    rf_set(path, 0x8f, MASK20, 0xa9c00);
    rf_set(path, 0xef, 1u << 4, 0x1);
    rf_set(path, 0x33, (1u << 1) | (1u << 0), 0x1);
  }
}

void Halrf8822b::txk_setting(uint8_t path) {
  w32(0x1b00, 0xf8000008 | (path << 1));
  w32(0x1bcc, 0x9);
  w32(0x1b20, 0x01440008);
  if (path == 0x0)
    w32(0x1b00, 0xf800000a);
  else
    w32(0x1b00, 0xf8000008);
  w32(0x1bcc, 0x3f);
  if (_band2g) {
    rf_set(path, 0x56, MASK20, 0x50df2);
    rf_set(path, 0x8f, MASK20, 0xadc00);
    w8(0x1b2b, 0x00);
  } else {
    rf_set(path, 0x56, MASK20, 0x500ef);
    rf_set(path, 0x8f, MASK20, 0xa9c00);
    w8(0x1b2b, 0x80);
  }
}

void Halrf8822b::rxk1_setting(uint8_t path) {
  w32(0x1b00, 0xf8000008 | (path << 1));
  if (_band2g) {
    w8(0x1bcc, 0x9);
    w8(0x1b2b, 0x00);
    w32(0x1b20, 0x01450008);
    w32(0x1b24, 0x01460c88);
    rf_set(path, 0x56, MASK20, 0x510e0);
    rf_set(path, 0x8f, MASK20, 0xacc00);
  } else {
    w8(0x1bcc, 0x09);
    w8(0x1b2b, 0x80);
    w32(0x1b20, 0x00850008);
    w32(0x1b24, 0x00460048);
    rf_set(path, 0x56, MASK20, 0x510e0);
    rf_set(path, 0x8f, MASK20, 0xadc00);
  }
}

void Halrf8822b::rxk2_setting(uint8_t path, bool is_gs) {
  w32(0x1b00, 0xf8000008 | (path << 1));
  if (_band2g) {
    if (is_gs)
      _tmp1bcc = 0x12;
    w8(0x1bcc, _tmp1bcc);
    w8(0x1b2b, 0x00);
    w32(0x1b20, 0x01450008);
    w32(0x1b24, 0x01460848);
    rf_set(path, 0x56, MASK20, 0x510e0);
    rf_set(path, 0x8f, MASK20, 0xa9c00);
  } else {
    if (is_gs)
      _tmp1bcc = (path == 0) ? 0x12 : 0x09;
    w8(0x1bcc, _tmp1bcc);
    w8(0x1b2b, 0x80);
    w32(0x1b20, 0x00850008);
    w32(0x1b24, 0x00460848);
    rf_set(path, 0x56, MASK20, 0x51060);
    rf_set(path, 0x8f, MASK20, 0xa9c00);
  }
}

void Halrf8822b::set_rf0x8(uint8_t path) {
  uint16_t c = 0;
  while (c < 30000) {
    rf_set(path, 0xef, MASK20, 0x0);
    rf_set(path, 0x8, MASK20, 0x0);
    if (rf_get(path, 0x8) == 0x0)
      break;
    c++;
  }
}

/* --- nctl LTE-coex gnt access (_iqk_ltec_read/write_8822b) --- */
uint32_t Halrf8822b::ltec_read(uint16_t reg) {
  w32(0x1700, 0x800f0000u | reg);
  uint32_t j = 0;
  while (((_device.rtw_read8(0x1703) & (1u << 5)) == 0) && (j < 30000))
    j++;
  return r32(0x1708);
}
void Halrf8822b::ltec_write(uint16_t reg, uint32_t mask, uint32_t val) {
  if (mask == 0)
    return;
  uint32_t data = val;
  if (mask != 0xffffffffu) {
    uint32_t bitpos = bshift(mask);
    uint32_t cur = ltec_read(reg);
    data = (cur & ~mask) | (val << bitpos);
  }
  w32(0x1704, data);
  uint32_t j = 0;
  while (((_device.rtw_read8(0x1703) & (1u << 5)) == 0) && (j < 30000))
    j++;
  w32(0x1700, 0xc00f0000u | reg);
}

/* _iqk_check_cal_8822b: poll RF0x8==0x12345; LOK ok always, else fail=0x1b08[26] */
bool Halrf8822b::check_cal(uint8_t path, uint8_t cmd) {
  bool notready = true, fail = true;
  uint32_t delay_count = 0;
  while (notready) {
    if (rf_get(path, 0x8) == 0x12345) {
      fail = (cmd == 0x0) ? false : (bb_get(0x1b08, 1u << 26) != 0);
      notready = false;
    } else {
      udelay(10);
      delay_count++;
    }
    if (delay_count >= 20000) {
      fail = true;
      break;
    }
  }
  set_rf0x8(path);
  return fail;
}

/* _iqk_rxk_gsearch_fail_8822b: RXK1/RXK2 gain search (IQMUX/lna_idx machine) */
bool Halrf8822b::rxk_gsearch(uint8_t path, uint8_t step) {
  bool fail = true;
  uint32_t IQK_CMD;
  const uint8_t IQMUX[4] = {0x9, 0x12, 0x1b, 0x24};

  if (step == RXIQK1) {
    IQK_CMD = 0xf8000208u | (1u << (path + 4));
    ltec_write(0x38, 0xffff, 0x7700);
    w32(0x1b00, IQK_CMD);
    w32(0x1b00, IQK_CMD + 0x1);
    udelay(10);
    fail = check_cal(path, 0x1);
    ltec_write(0x38, 0xffffffff, _tmp_gntwl);
  } else { /* RXIQK2 */
    uint8_t idx;
    for (idx = 0; idx < 4; idx++)
      if (_tmp1bcc == IQMUX[idx])
        break;
    if (idx == 4)
      return fail;
    w32(0x1b00, 0xf8000008u | (path << 1));
    w32(0x1bcc, _tmp1bcc);
    IQK_CMD = 0xf8000308u | (1u << (path + 4));
    ltec_write(0x38, 0xffff, 0x7700);
    w32(0x1b00, IQK_CMD);
    w32(0x1b00, IQK_CMD + 0x1);
    udelay(10);
    fail = check_cal(path, 0x1);
    ltec_write(0x38, 0xffffffff, _tmp_gntwl);

    uint32_t rf_reg0 = rf_get(path, 0x0);
    w32(0x1b00, 0xf8000008u | (path << 1));
    uint32_t tmp = (rf_reg0 & 0x1fe0) >> 5;
    _lna_idx = static_cast<uint8_t>(tmp >> 5);
    uint32_t bb_idx = tmp & 0x1f;
    if (bb_idx == 0x1) {
      if (_lna_idx != 0x0)
        _lna_idx--;
      else if (idx != 3)
        idx++;
      else
        _isbnd = true;
      fail = true;
    } else if (bb_idx == 0xa) {
      if (idx != 0)
        idx--;
      else if (_lna_idx != 0x7)
        _lna_idx++;
      else
        _isbnd = true;
      fail = true;
    } else {
      fail = false;
    }
    if (_isbnd)
      fail = false;
    _tmp1bcc = IQMUX[idx];
    if (fail) {
      w32(0x1b00, 0xf8000008u | (path << 1));
      tmp = (r32(0x1b24) & 0xffffe3ffu) | (_lna_idx << 10);
      w32(0x1b24, tmp);
    }
  }
  return fail;
}

/* _lok_one_shot_8822b (backup of lok_idac skipped — no reload) */
bool Halrf8822b::lok_one_shot(uint8_t path) {
  uint32_t IQK_CMD = 0xf8000008u | (1u << (4 + path));
  ltec_write(0x38, 0xffff, 0x7700);
  w32(0x1b00, IQK_CMD);
  w32(0x1b00, IQK_CMD + 1);
  udelay(10);
  bool notready = check_cal(path, 0x0);
  ltec_write(0x38, 0xffffffff, _tmp_gntwl);
  _lok_fail[path] = notready;
  return notready;
}

/* _iqk_one_shot_8822b: TXIQK / RXIQK1 / RXIQK2 (CFIR backup skipped) */
bool Halrf8822b::one_shot(uint8_t path, uint8_t idx) {
  uint32_t IQK_CMD = 0;
  const uint16_t iqk_apply[2] = {0xc94, 0xe94};
  if (idx == TXIQK) {
    IQK_CMD = 0xf8000008u | ((_bw + 4) << 8) | (1u << (path + 4));
  } else if (idx == RXIQK1) {
    IQK_CMD = (_bw == 2) ? (0xf8000808u | (1u << (path + 4)))
                         : (0xf8000708u | (1u << (path + 4)));
  } else { /* RXIQK2 */
    IQK_CMD = 0xf8000008u | ((_bw + 9) << 8) | (1u << (path + 4));
    w32(0x1b00, 0xf8000008u | (path << 1));
    uint32_t tmp = (r32(0x1b24) & 0xffffe3ffu) | ((_lna_idx & 0x7) << 10);
    w32(0x1b24, tmp);
  }
  ltec_write(0x38, 0xffff, 0x7700);
  w32(0x1b00, IQK_CMD);
  w32(0x1b00, IQK_CMD + 0x1);
  udelay(10);
  bool fail = check_cal(path, 0x1);
  ltec_write(0x38, 0xffffffff, _tmp_gntwl);

  w32(0x1b00, 0xf8000008u | (path << 1));
  if (idx == TXIQK) {
    if (fail)
      bb_set(iqk_apply[path], 1u << 0, 0x0); /* disable apply on fail */
    /* success: HW keeps applied result (CFIR backup for reload skipped) */
  }
  if (idx == RXIQK2) {
    w32(0x1b38, 0x20000000);
    if (fail)
      bb_set(iqk_apply[path], (1u << 11) | (1u << 10), 0x0);
  }
  return fail;
}

/* _iqk_rx_iqk_by_path_8822b: RXK step machine (xym debug reads skipped) */
bool Halrf8822b::rx_iqk_by_path(uint8_t path) {
  bool KFAIL = true, gonext;
  switch (_rxiqk_step) {
  case 1: /* gain search RXK1 */
    rxk1_setting(path);
    gonext = false;
    while (true) {
      KFAIL = rxk_gsearch(path, RXIQK1);
      if (KFAIL && _gs_retry[path][0] < 2) {
        _gs_retry[path][0]++;
      } else if (KFAIL) {
        _rxiqk_fail_code[path] = 0;
        _rxiqk_step = 5;
        gonext = true;
      } else {
        _rxiqk_step++;
        gonext = true;
      }
      if (gonext)
        break;
    }
    break;
  case 2: /* gain search RXK2 */
    rxk2_setting(path, true);
    _isbnd = false;
    while (true) {
      KFAIL = rxk_gsearch(path, RXIQK2);
      if (KFAIL && _gs_retry[path][1] < rxiqk_gs_limit)
        _gs_retry[path][1]++;
      else {
        _rxiqk_step++;
        break;
      }
    }
    break;
  case 3: /* RXK1 */
    rxk1_setting(path);
    gonext = false;
    while (true) {
      KFAIL = one_shot(path, RXIQK1);
      if (KFAIL && _retry[path][RXIQK1] < 2) {
        _retry[path][RXIQK1]++;
      } else if (KFAIL) {
        _rxiqk_fail_code[path] = 1;
        _rxiqk_step = 5;
        gonext = true;
      } else {
        _rxiqk_step++;
        gonext = true;
      }
      if (gonext)
        break;
    }
    break;
  case 4: /* RXK2 */
    rxk2_setting(path, false);
    gonext = false;
    while (true) {
      KFAIL = one_shot(path, RXIQK2);
      if (KFAIL && _retry[path][RXIQK2] < 2) {
        _retry[path][RXIQK2]++;
      } else if (KFAIL) {
        _rxiqk_fail_code[path] = 2;
        _rxiqk_step = 5;
        gonext = true;
      } else {
        _rxiqk_step++;
        gonext = true;
      }
      if (gonext)
        break;
    }
    break;
  }
  return KFAIL;
}

void Halrf8822b::iqk_by_path_subfunction(uint8_t path) {
  while (true) {
    rx_iqk_by_path(path);
    if (_rxiqk_step == 5) {
      _iqk_step++;
      _rxiqk_step = 1;
      break;
    }
  }
  _kcount++;
}

/* _iqk_iqk_by_path_8822b (non-segment: run to iqk_step==7) */
void Halrf8822b::iqk_by_path() {
  bool KFAIL;
  while (true) {
    switch (_iqk_step) {
    case 1: /* S0 LOK */
      lok_setting(0);
      lok_one_shot(0);
      _iqk_step++;
      break;
    case 2: /* S1 LOK */
      lok_setting(1);
      lok_one_shot(1);
      _iqk_step++;
      break;
    case 3: /* S0 TXIQK */
      txk_setting(0);
      KFAIL = one_shot(0, TXIQK);
      _kcount++;
      if (KFAIL && _retry[0][TXIQK] < 3)
        _retry[0][TXIQK]++;
      else
        _iqk_step++;
      break;
    case 4: /* S1 TXIQK */
      txk_setting(1);
      KFAIL = one_shot(1, TXIQK);
      _kcount++;
      if (KFAIL && _retry[1][TXIQK] < 3)
        _retry[1][TXIQK]++;
      else
        _iqk_step++;
      break;
    case 5: /* S0 RXIQK */
      iqk_by_path_subfunction(0);
      break;
    case 6: /* S1 RXIQK */
      iqk_by_path_subfunction(1);
      break;
    }
    if (_iqk_step == 7) {
      for (uint8_t i = 0; i < 2; i++) {
        w32(0x1b00, 0xf8000008u | (i << 1));
        w32(0x1b2c, 0x7);
        w32(0x1bcc, 0x0);
        w32(0x1b38, 0x20000000);
      }
      break;
    }
  }
}

void Halrf8822b::start_iqk() {
  /* GNT_WL = 1 (RF 0x1 |= BIT5|BIT0), both paths */
  uint32_t tmp = rf_get(0, 0x1) | (1u << 5) | (1u << 0);
  rf_set(0, 0x1, MASK20, tmp);
  tmp = rf_get(1, 0x1) | (1u << 5) | (1u << 0);
  rf_set(1, 0x1, MASK20, tmp);
  iqk_by_path();
}

void Halrf8822b::iqk_init() {
  /* cu_distance (IQK result variation) */
  w32(0x1b10, 0x88011c00);
}

/* _phy_iq_calibrate_8822b — fresh (non-segment, no reload) IQK. */
void Halrf8822b::iqk_trigger(bool band2g) {
  _band2g = band2g;
  _bw = 0; /* 20 MHz */

  uint32_t mac_bk[2], bb_bk[21], rf_bk[5][2];
  const uint32_t mac_reg[2] = {0x520, 0x550};
  const uint32_t bb_reg[21] = {0x808, 0x90c, 0xc00, 0xcb0, 0xcb4, 0xcbc, 0xe00,
                               0xeb0, 0xeb4, 0xebc, 0x1990, 0x9a4, 0xa04, 0xb00,
                               0x838, 0xc58, 0xc5c, 0xc6c, 0xe58, 0xe5c, 0xe6c};
  const uint32_t rf_reg[5] = {0xdf, 0x8f, 0x65, 0x0, 0x1};

  iqk_init();

  /* reset run state */
  _kcount = 0;
  _iqk_step = 1;
  _rxiqk_step = 1;
  for (int p = 0; p < 2; p++) {
    _lok_fail[p] = true;
    _rxiqk_fail_code[p] = 0;
    for (int k = 0; k < 3; k++)
      _retry[p][k] = 0;
    for (int k = 0; k < 2; k++)
      _gs_retry[p][k] = 0;
  }

  _tmp_gntwl = ltec_read(0x38);
  backup_mac_bb(mac_bk, bb_bk, mac_reg, bb_reg);
  backup_rf(rf_bk, rf_reg);

  while (true) {
    configure_macbb();
    afe_setting(true);
    rfe_setting(false);
    agc_bnd_int();
    rf_setting();
    start_iqk();
    afe_setting(false);
    restore_mac_bb(mac_bk, bb_bk, mac_reg, bb_reg);
    restore_rf(rf_reg, rf_bk);
    if (_iqk_step == 7)
      break;
    _kcount = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  /* rf0xb0 workaround: RF_A 0xb8 phase-noise nudge */
  rf_set(0, 0xb8, MASK20, 0x00a00);
  rf_set(0, 0xb8, MASK20, 0x80a00);

  _logger->info("Jaguar2 IQK done: LOK A/B fail={}/{} | TXK retry A/B={}/{} | "
                "RXK gs_retry A(g1={} g2={}) B(g1={} g2={}) 1shot_retry "
                "A(k1={} k2={}) B(k1={} k2={}) | RXK failcode A/B={}/{} "
                "(0=gs1,1=rxk1,2=rxk2; only meaningful if that step ran)",
                _lok_fail[0], _lok_fail[1], _retry[0][TXIQK], _retry[1][TXIQK],
                _gs_retry[0][0], _gs_retry[0][1], _gs_retry[1][0],
                _gs_retry[1][1], _retry[0][RXIQK1], _retry[0][RXIQK2],
                _retry[1][RXIQK1], _retry[1][RXIQK2], _rxiqk_fail_code[0],
                _rxiqk_fail_code[1]);
}

/* ===== Thermal TX-power tracking =================================
 * Vendor-faithful MIX_MODE port of odm_txpowertracking_new_callback_thermal_meter
 * + odm_tx_pwr_track_set_pwr8822b / get_mix_mode_tx_agc_bb_swing_offset_8822b.
 * Tables transcribed from reference/rtl88x2bu/hal/phydm/halrf/rtl8822b/
 * halhwimg8822b_rf.c (delta_swingidx_mp_*_txpwrtrk_8822b, D_S_SIZE=30) and
 * tx_scaling_table_jaguar (halrf_powertracking_ce.c, TXSCALE_TABLE_SIZE=37). */
namespace {
constexpr int D_S = 30; /* DELTA_SWINGIDX_SIZE */

/* BB TX-scale multipliers (0xc1c/0xe1c[31:21]); index 24 = 0 dB, 0.5 dB/step. */
const uint32_t tx_scaling_table_jaguar[37] = {
    0x081, 0x088, 0x090, 0x099, 0x0A2, 0x0AC, 0x0B6, 0x0C0, 0x0CC, 0x0D8,
    0x0E5, 0x0F2, 0x101, 0x110, 0x120, 0x131, 0x143, 0x156, 0x16A, 0x180,
    0x197, 0x1AF, 0x1C8, 0x1E3, 0x200, 0x21E, 0x23E, 0x261, 0x285, 0x2AB,
    0x2D3, 0x2FE, 0x32B, 0x35C, 0x38E, 0x3C4, 0x3FE};

/* 2.4 GHz swing (path A = 2ga, path B = 2gb), warming (p) / cooling (n). */
const uint8_t k2ga_p[D_S] = {0, 1, 2, 3, 3, 4, 5,  6,  6,  7,  8,  9,  9,  10, 11,
                             12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 22, 22};
const uint8_t k2ga_n[D_S] = {0, 1, 2, 3, 3, 4, 5,  6,  6,  7,  8,  9,  10, 11, 12,
                             13, 13, 14, 15, 16, 17, 18, 18, 18, 18, 18, 18, 18, 18, 18};
const uint8_t k2gb_p[D_S] = {0, 1, 1, 2, 3, 4, 4,  5,  6,  7,  7,  8,  9,  10, 11,
                             12, 12, 13, 14, 15, 16, 17, 17, 18, 19, 20, 21, 22, 22, 22};
const uint8_t k2gb_n[D_S] = {0, 1, 2, 3, 3, 4, 4,  5,  6,  7,  8,  9,  10, 11, 12,
                             13, 13, 14, 15, 16, 16, 17, 18, 18, 18, 18, 18, 18, 18, 18};

/* 5 GHz swing, 3 sub-bands: [0]=ch36-64, [1]=ch100-144, [2]=ch149-177. */
const uint8_t k5ga_p[3][D_S] = {
    {0, 1, 2, 2, 3, 4, 5, 5, 6, 7, 8, 9, 9, 10, 11, 12, 13, 14, 14, 15, 16, 17, 18, 19, 19, 20, 20, 20, 20, 20},
    {0, 1, 2, 2, 3, 4, 4, 5, 6, 6, 7, 7, 8, 9, 9, 10, 11, 11, 12, 13, 14, 15, 16, 16, 17, 17, 18, 18, 18, 18},
    {0, 1, 2, 3, 3, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 12, 13, 14, 15, 15, 16, 17, 17, 18, 18, 18, 18, 18}};
const uint8_t k5ga_n[3][D_S] = {
    {0, 1, 2, 2, 3, 3, 4, 5, 6, 7, 8, 8, 9, 9, 10, 11, 11, 12, 12, 12, 13, 13, 14, 14, 14, 15, 15, 15, 15, 15},
    {0, 1, 2, 2, 3, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 14, 14, 14, 14, 14},
    {0, 1, 2, 2, 3, 4, 4, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 12, 12, 13, 13, 14, 14, 14, 14, 14, 14, 14}};
const uint8_t k5gb_p[3][D_S] = {
    {0, 1, 2, 2, 3, 4, 5, 6, 7, 8, 8, 9, 10, 11, 11, 12, 13, 14, 15, 15, 16, 17, 18, 18, 19, 19, 19, 19, 19, 19},
    {0, 1, 2, 2, 3, 4, 5, 6, 6, 7, 8, 8, 9, 9, 10, 11, 12, 12, 13, 14, 15, 16, 17, 17, 18, 18, 18, 18, 18, 18},
    {0, 1, 2, 2, 3, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 10, 11, 12, 13, 14, 15, 15, 16, 16, 17, 17, 17, 17, 17, 17}};
const uint8_t k5gb_n[3][D_S] = {
    {0, 1, 2, 3, 3, 4, 5, 6, 6, 7, 8, 9, 9, 10, 10, 11, 11, 12, 12, 12, 13, 13, 14, 14, 14, 15, 15, 15, 15, 15},
    {0, 1, 1, 2, 2, 3, 3, 4, 5, 5, 6, 7, 7, 8, 8, 9, 10, 10, 11, 12, 12, 13, 13, 14, 14, 14, 14, 14, 14, 14},
    {0, 1, 2, 2, 3, 3, 4, 4, 5, 6, 6, 7, 7, 8, 9, 9, 10, 10, 11, 12, 12, 13, 13, 14, 14, 14, 14, 14, 14, 14}};
} /* namespace */

void Halrf8822b::set_pwr_track_ctx(uint8_t baseline, uint8_t channel) {
  _pt_baseline = baseline;
  _pt_channel = channel;
  for (auto &a : _pt_avg)
    for (auto &x : a)
      x = 0;
  _pt_avg_idx[0] = _pt_avg_idx[1] = 0;
  _pt_last_swing[0] = _pt_last_swing[1] = 0x7fffffff;
  /* default_ofdm_index = reverse-map the current 0xc1c[31:21] BB swing to a
   * tx_scaling_table_jaguar index (the level the phydm BB table left); 24
   * (0 dB) if no exact match — matches the vendor `else` fallback. */
  _pt_default_ofdm = 24;
  const uint32_t cur = bb_get(0xc1c, 0xFFE00000);
  for (int i = 0; i < 37; i++)
    if (tx_scaling_table_jaguar[i] == cur) {
      _pt_default_ofdm = static_cast<uint8_t>(i);
      break;
    }
  _logger->info("Jaguar2 8822B thermal-track: baseline={} ch={} default_ofdm={}",
                baseline, channel, _pt_default_ofdm);
}

void Halrf8822b::pwr_track_write(uint8_t path, int swing, int cur_ofdm_idx) {
  /* MIX_MODE split: coarse steps to TXAGC (0xc94), remnant to BB swing (0xc1c),
   * bounded by the headroom above the current OFDM index and the BB table. */
  const int tx_pwr_idx = cur_ofdm_idx < 0 ? 63 : (cur_ofdm_idx > 63 ? 63 : cur_ofdm_idx);
  int headroom = 63 - tx_pwr_idx; /* tx_power_index_offset */
  if (headroom > 0xF)
    headroom = 0xF;
  const int def = _pt_default_ofdm;
  const int ub = def + 10;
  int tx_agc_index, bb_swing_index;
  if (swing >= 0 && swing <= headroom) {
    tx_agc_index = swing;
    bb_swing_index = def;
  } else if (swing > headroom) {
    tx_agc_index = headroom;
    bb_swing_index = def + (swing - headroom);
    if (bb_swing_index > ub)
      bb_swing_index = ub;
  } else { /* swing < 0 (cooling) */
    tx_agc_index = 0;
    bb_swing_index = def > -swing ? def + swing : 0;
  }
  if (bb_swing_index < 0)
    bb_swing_index = 0;
  if (bb_swing_index > 36)
    bb_swing_index = 36;
  const uint16_t c94 = (path == 0) ? 0xc94 : 0xe94;
  const uint16_t c1c = (path == 0) ? 0xc1c : 0xe1c;
  bb_set(c94, 0x3E000000, static_cast<uint32_t>(tx_agc_index) & 0x1f); /* [29:25] */
  bb_set(c1c, 0xFFE00000, tx_scaling_table_jaguar[bb_swing_index]);
}

void Halrf8822b::pwr_track(int current_ofdm_index) {
  if (_pt_baseline == 0xff)
    return;
  const bool g2 = _pt_channel <= 14;
  const int sub = g2 ? 0 : (_pt_channel <= 64 ? 0 : (_pt_channel <= 144 ? 1 : 2));
  /* Single die thermal sensor: only path A's RF 0x42 meter reads back (path B
   * reads 0 — its sensor isn't enabled in this bring-up), and both PA chains
   * share the die and heat together, so one measured delta drives both. Each
   * path still uses its own delta-swing curve (2ga/2gb, 5ga/5gb). */
  const int cur = static_cast<int>((rf_get(0, 0x42) >> 10) & 0x3f);
  _pt_avg[0][_pt_avg_idx[0]] = cur;
  _pt_avg_idx[0] = (_pt_avg_idx[0] + 1) & 3;
  int sum = 0, cnt = 0;
  for (int i = 0; i < 4; i++)
    if (_pt_avg[0][i]) {
      sum += _pt_avg[0][i];
      cnt++;
    }
  if (!cnt)
    return; /* meter not ready — no valid sample yet */
  const int avg = sum / cnt;
  int delta = avg > _pt_baseline ? avg - _pt_baseline : _pt_baseline - avg;
  if (delta > D_S - 1)
    delta = D_S - 1;
  const int paths = _2t2r ? 2 : 1;
  for (int p = 0; p < paths; p++) {
    const uint8_t *up = g2 ? (p == 0 ? k2ga_p : k2gb_p) : (p == 0 ? k5ga_p[sub] : k5gb_p[sub]);
    const uint8_t *dn = g2 ? (p == 0 ? k2ga_n : k2gb_n) : (p == 0 ? k5ga_n[sub] : k5gb_n[sub]);
    const int swing = avg > _pt_baseline ? up[delta] : -static_cast<int>(dn[delta]);
    if (swing == _pt_last_swing[p])
      continue; /* no change — skip the USB write */
    _pt_last_swing[p] = swing;
    pwr_track_write(static_cast<uint8_t>(p), swing, current_ofdm_index);
    _logger->info("Jaguar2 8822B thermal-track: path{} avg={} d={} swing={}",
                  p, avg, delta, swing);
  }
}

#if defined(DEVOURER_HAVE_JAGUAR2_8821C)
/* Defined in Halrf8821c.cpp. */
std::unique_ptr<Jaguar2Calibration>
make_calibration_8821c(RtlAdapter device, Logger_t logger, uint8_t cut,
                       bool is_2t2r);
#endif

std::unique_ptr<Jaguar2Calibration>
make_jaguar2_calibration(ChipVariant variant, RtlAdapter device,
                         Logger_t logger, uint8_t cut, bool is_2t2r) {
#if defined(DEVOURER_HAVE_JAGUAR2_8821C)
  if (variant == ChipVariant::C8821C)
    return make_calibration_8821c(std::move(device), std::move(logger), cut,
                                  is_2t2r);
#endif
  (void)variant;
  return std::make_unique<Halrf8822b>(std::move(device), std::move(logger), cut,
                                      is_2t2r);
}

} /* namespace jaguar2 */
