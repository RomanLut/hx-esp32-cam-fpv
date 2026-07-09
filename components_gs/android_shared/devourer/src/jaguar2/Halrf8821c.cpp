#include "Halrf8821c.h"

#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace jaguar2 {

namespace {
constexpr uint32_t MASK20 = 0x000FFFFF; /* RFREGOFFSETMASK (0xfffff) */
constexpr uint32_t MASKDWORD = 0xFFFFFFFFu;
/* halrf_iqk.h: TX_IQK=0, RX_IQK=1, RXIQK1=1, RXIQK2=2; halrf_iqk_8821c.h:
 * TXIQK=0, RXIQK=1 */
constexpr uint8_t TXIQK = 0, RXIQK1 = 1, RXIQK2 = 2;
constexpr uint8_t TX_IQK = 0, RX_IQK = 1;
constexpr uint8_t rxiqk_gs_limit = 6;

/* _iqk_rx_iqk_gain_search LNA tables (module-level in vendor). */
const uint8_t btg_lna[5] = {0x0, 0x4, 0x8, 0xc, 0xf};
const uint8_t wlg_lna[5] = {0x0, 0x1, 0x2, 0x3, 0x5};
const uint8_t wla_lna[5] = {0x0, 0x1, 0x3, 0x4, 0x5};

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

Halrf8821c::Halrf8821c(RtlAdapter device, Logger_t logger, uint8_t cut,
                       bool is_2t2r)
    : _device{std::move(device)}, _logger{std::move(logger)}, _cut{cut},
      _2t2r{is_2t2r} {}

/* --- BB access (odm_get/set_bb_reg) --- */
uint32_t Halrf8821c::bb_get(uint16_t addr, uint32_t mask) {
  return (_device.rtw_read32(addr) & mask) >> bshift(mask);
}
void Halrf8821c::bb_set(uint16_t addr, uint32_t mask, uint32_t data) {
  _device.phy_set_bb_reg(addr, mask, data);
}

/* --- RF access: read = direct BB window; write = 3-wire LSSI (0xC90). --- */
uint32_t Halrf8821c::rf_get(uint8_t path, uint32_t addr) {
  const uint32_t base = (path == 0) ? 0x2800u : 0x2c00u;
  const uint16_t direct = static_cast<uint16_t>(base + ((addr & 0xff) << 2));
  return _device.rtw_read32(direct) & MASK20;
}
void Halrf8821c::rf_set(uint8_t path, uint32_t addr, uint32_t mask,
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

/* --- backup / restore (_iqk_backup_*_8821c, _iqk_restore_*_8821c) --- */
void Halrf8821c::backup_mac_bb(uint32_t *mac_bk, uint32_t *bb_bk,
                               const uint32_t *mac_reg,
                               const uint32_t *bb_reg) {
  for (int i = 0; i < 3; i++) /* MAC_REG_NUM_8821C */
    mac_bk[i] = r32(static_cast<uint16_t>(mac_reg[i]));
  for (int i = 0; i < 16; i++) /* BB_REG_NUM_8821C (+0xc94,0xb00 for TX-path restore) */
    bb_bk[i] = r32(static_cast<uint16_t>(bb_reg[i]));
}
void Halrf8821c::backup_rf(uint32_t *rf_bk, const uint32_t *reg) {
  for (int i = 0; i < 5; i++) /* RF_REG_NUM_8821C, SS_8821C=1 (path A) */
    rf_bk[i] = rf_get(0, reg[i]);
}
void Halrf8821c::restore_mac_bb(const uint32_t *mac_bk, const uint32_t *bb_bk,
                                const uint32_t *mac_reg,
                                const uint32_t *bb_reg) {
  for (int i = 0; i < 3; i++)
    w32(static_cast<uint16_t>(mac_reg[i]), mac_bk[i]);
  for (int i = 0; i < 16; i++)
    w32(static_cast<uint16_t>(bb_reg[i]), bb_bk[i]);
}
/* backup_rf_reg = {0xdf, 0xde, 0x8f, 0x0, 0x1}; RF_backup[i] path A. */
void Halrf8821c::restore_rf(const uint32_t *reg, const uint32_t *rf_bk) {
  rf_set(0, 0xef, MASK20, 0x0);
  rf_set(0, 0xee, MASK20, 0x0);
  rf_set(0, 0xdf, MASK20, rf_bk[0] & ~(1u << 4));
  rf_set(0, 0xde, MASK20, rf_bk[1] & ~(1u << 4));
  for (int i = 2; i < (5 - 1); i++)
    rf_set(0, reg[i], MASK20, rf_bk[i]);
  rf_set(0, 0x1, MASK20, rf_bk[4] & ~(1u << 0));
}

void Halrf8821c::agc_bnd_int() {
  w32(0x1b00, 0xf8000008);
  w32(0x1b00, 0xf80a7008);
  w32(0x1b00, 0xf8015008);
  w32(0x1b00, 0xf8000008);
}

void Halrf8821c::bb_reset() {
  uint32_t count = 0;
  rf_set(0, 0x0, MASK20, 0x10000);
  bb_set(0x8f8,
         (1u << 27) | (1u << 26) | (1u << 25) | (1u << 24) | (1u << 23) |
             (1u << 22) | (1u << 21) | (1u << 20),
         0x0);
  while (true) {
    w32(0x8fc, 0x0);
    bb_set(0x198c, 0x7, 0x7);
    bool cca_ing = bb_get(0xfa0, 1u << 3) != 0;
    if (count > 30)
      cca_ing = false;
    if (cca_ing) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      count++;
    } else {
      w8(0x808, 0x0); /* RX ant off */
      bb_set(0xa04, (1u << 27) | (1u << 26) | (1u << 25) | (1u << 24),
             0x0); /* CCK RX path off */
      bb_set(0x0, 1u << 16, 0x0);
      bb_set(0x0, 1u << 16, 0x1);
      if (bb_get(0x660, 1u << 16))
        w32(0x6b4, 0x89000006);
      break;
    }
  }
}

void Halrf8821c::afe_setting(bool do_iqk) {
  if (do_iqk) {
    w32(0xc60, 0x50000000);
    w32(0xc60, 0x700F0040);
    w32(0xc58, 0xd8000402);
    w32(0xc5c, 0xd1000120);
    w32(0xc6c, 0x00000a15);
    bb_reset();
  } else {
    w32(0xc60, 0x50000000);
    w32(0xc60, 0x700B8040);
    w32(0xc58, 0xd8020402);
    w32(0xc5c, 0xde000120);
    w32(0xc6c, 0x0000122a);
  }
  bb_set(0x9a4, 1u << 31, 0x0); /* select da clock */
}

void Halrf8821c::rfe_setting(bool ext_pa_on) {
  if (ext_pa_on) {
    w32(0xcb0, 0x77777777);
    w32(0xcb4, 0x00007777);
    w32(0xcbc, 0x0000083B);
  } else {
    w32(0xcb0, 0x77171117);
    w32(0xcb4, 0x00001177);
    w32(0xcbc, 0x00000404);
  }
}

void Halrf8821c::rf_setting() {
  w32(0x1b00, 0xf8000008);
  w32(0x1bb8, 0x00000000);
  const uint8_t path = 0; /* SS_8821C = 1 */

  /* 0xdf: B11=1, B4=0, B1=1 */
  uint32_t tmp = rf_get(path, 0xdf);
  tmp = (tmp & ~(1u << 4)) | (1u << 1) | (1u << 11);
  rf_set(path, 0xdf, MASK20, tmp);

  if (_is_btg) {
    tmp = rf_get(0, 0xde);
    tmp = (tmp & ~(1u << 4)) | (1u << 15);
    rf_set(0, 0xde, MASK20, tmp);
  }

  if (!_is_btg) {
    /* WLAN_AG TX IQK mode init */
    rf_set(path, 0xef, MASK20, 0x80000);
    rf_set(path, 0x33, MASK20, 0x00024);
    rf_set(path, 0x3e, MASK20, 0x0003f);
    rf_set(path, 0x3f, MASK20, 0xe0fde);
    rf_set(path, 0xef, MASK20, 0x00000);
    if (!_band2g) { /* ODM_BAND_5G */
      rf_set(path, 0xef, 1u << 19, 0x1);
      rf_set(path, 0x33, MASK20, 0x00026);
      rf_set(path, 0x3e, MASK20, 0x00037);
      rf_set(path, 0x3f, MASK20, 0xdefce);
      rf_set(path, 0xef, 1u << 19, 0x0);
    } else {
      rf_set(path, 0xef, 1u << 19, 0x1);
      rf_set(path, 0x33, MASK20, 0x00026);
      rf_set(path, 0x3e, MASK20, 0x00037);
      rf_set(path, 0x3f, MASK20, 0x5efce);
      rf_set(path, 0xef, 1u << 19, 0x0);
    }
  } else {
    /* WLAN_BTG TX IQK mode init */
    rf_set(path, 0xee, MASK20, 0x01000);
    rf_set(path, 0x33, MASK20, 0x00004);
    rf_set(path, 0x3f, MASK20, 0x01ec1);
    rf_set(path, 0xee, MASK20, 0x00000);
  }
}

void Halrf8821c::configure_macbb() {
  w8(0x522, 0x7f);
  bb_set(0x1518, 1u << 16, 0x1);
  bb_set(0x550, (1u << 11) | (1u << 3), 0x0);
  bb_set(0x90c, 1u << 15, 0x1); /* dac_buf reset selection */
  bb_set(0xc94, 1u << 0, 0x1);  /* Let tx from IQK */
  bb_set(0xc94, (1u << 11) | (1u << 10), 0x1);
  w32(0xc00, 0x00000004);       /* 3-wire off */
  bb_set(0xb00, 1u << 8, 0x0);  /* disable PMAC */
  bb_set(0x808, 1u << 28, 0x0); /* disable CCK block */
  bb_set(0x838, (1u << 3) | (1u << 2) | (1u << 1), 0x7); /* disable OFDM CCA */
}

void Halrf8821c::lok_setting(uint8_t path, uint8_t pad_index) {
  uint32_t LOK0x56_2G = 0x50ef3;
  uint32_t LOK0x56_5G = 0x50ee8;
  uint32_t LOK0x78 = 0xbcbba;
  uint32_t LOK0x33 = pad_index;

  /* mp_mode-only 0x810 tweak omitted (not mp). */

  if (_is_btg) {
    w32(0x1b00, 0xf8000008u | (path << 1));
    w32(0x1bcc, 0x1b);
    w8(0x1b23, 0x00);
    w8(0x1b2b, 0x80);
    LOK0x78 = LOK0x78 & (0xe3fff | (static_cast<uint32_t>(pad_index) << 14));
    rf_set(path, 0x78, MASK20, LOK0x78);
    rf_set(path, 0x5c, MASK20, 0x05320);
    rf_set(path, 0x8f, MASK20, 0xac018);
    rf_set(0, 0xee, 1u << 4, 0x1);
    rf_set(0, 0x33, 1u << 3, 0x0);
  } else {
    w32(0x1b00, 0xf8000008u | (path << 1));
    w32(0x1bcc, 0x9);
    w8(0x1b23, 0x00);
    if (_band2g) { /* ODM_BAND_2_4G */
      w8(0x1b2b, 0x00);
      LOK0x56_2G =
          LOK0x56_2G & (0xfff1f | (static_cast<uint32_t>(pad_index) << 5));
      rf_set(path, 0x56, MASK20, LOK0x56_2G);
      rf_set(path, 0x8f, MASK20, 0xadc18);
      rf_set(0, 0xef, 1u << 4, 0x1);
      rf_set(0, 0x33, 1u << 3, 0x0);
    } else { /* ODM_BAND_5G */
      w8(0x1b2b, 0x00);
      LOK0x56_5G =
          LOK0x56_5G & (0xfff1f | (static_cast<uint32_t>(pad_index) << 5));
      rf_set(path, 0x56, MASK20, LOK0x56_5G);
      rf_set(path, 0x8f, MASK20, 0xadc18);
      rf_set(0, 0xef, 1u << 4, 0x1);
      rf_set(0, 0x33, 1u << 3, 0x1);
    }
  }
  /* IDAC LUT by PAD idx */
  rf_set(path, 0x33, (1u << 2) | (1u << 1) | (1u << 0), LOK0x33);
}

void Halrf8821c::txk_setting(uint8_t path) {
  if (_is_btg) {
    w32(0x1b00, 0xf8000008u | (path << 1));
    w32(0x1bcc, 0x1b);
    w32(0x1b20, 0x00840008);
    rf_set(path, 0x78, MASK20, 0xbcbba);
    rf_set(path, 0x5c, MASK20, 0x04320);
    rf_set(path, 0x8f, MASK20, 0xac018);
    w8(0x1b2b, 0x80);
  } else {
    w32(0x1b00, 0xf8000008u | (path << 1));
    w32(0x1bcc, 0x9);
    w32(0x1b20, 0x01440008);
    if (_band2g) { /* ODM_BAND_2_4G */
      rf_set(path, 0x56, MASK20, 0x50EF3);
      rf_set(path, 0x8f, MASK20, 0xadc18);
      w8(0x1b2b, 0x00);
    } else { /* ODM_BAND_5G */
      rf_set(path, 0x56, MASK20, 0x5004e);
      rf_set(path, 0x8f, MASK20, 0xa9c18);
      w8(0x1b2b, 0x00);
    }
  }
}

void Halrf8821c::rxk1_setting(uint8_t path) {
  if (_is_btg) {
    w32(0x1b00, 0xf8000008u | (path << 1));
    w8(0x1b2b, 0x80);
    w32(0x1bcc, 0x09);
    w32(0x1b20, 0x01450008);
    w32(0x1b24, 0x01460c88);
    rf_set(path, 0x78, MASK20, 0x8cbba);
    rf_set(path, 0x5c, MASK20, 0x00320);
    rf_set(path, 0x8f, MASK20, 0xa8018);
  } else {
    w32(0x1b00, 0xf8000008u | (path << 1));
    if (_band2g) { /* ODM_BAND_2_4G */
      w8(0x1bcc, 0x12);
      w8(0x1b2b, 0x00);
      w32(0x1b20, 0x01450008);
      w32(0x1b24, 0x01461068);
      rf_set(path, 0x56, MASK20, 0x510f3);
      rf_set(path, 0x8f, MASK20, 0xa9c00);
    } else { /* ODM_BAND_5G */
      w8(0x1bcc, 0x9);
      w8(0x1b2b, 0x00);
      w32(0x1b20, 0x00450008);
      w32(0x1b24, 0x00461468);
      rf_set(path, 0x56, MASK20, 0x510f3);
      rf_set(path, 0x8f, MASK20, 0xa9c00);
    }
  }
}

void Halrf8821c::rxk2_setting(uint8_t path, bool is_gs) {
  if (_is_btg) {
    if (is_gs) {
      _tmp1bcc = 0x1b;
      _lna_idx = 2;
    }
    w32(0x1b00, 0xf8000008u | (path << 1));
    w8(0x1b2b, 0x80);
    w32(0x1bcc, _tmp1bcc);
    w32(0x1b20, 0x01450008);
    w32(0x1b24, 0x01460048u | (btg_lna[_lna_idx] << 10));
    rf_set(path, 0x78, MASK20, 0x8cbba);
    rf_set(path, 0x5c, MASK20, 0x00320);
    rf_set(path, 0x8f, MASK20, 0xa8018);
  } else {
    w32(0x1b00, 0xf8000008u | (path << 1));
    if (_band2g) { /* ODM_BAND_2_4G */
      if (is_gs) {
        _tmp1bcc = 0x12;
        _lna_idx = 2;
      }
      w8(0x1bcc, _tmp1bcc);
      w8(0x1b2b, 0x00);
      w32(0x1b20, 0x01450008);
      w32(0x1b24, 0x01460048u | (wlg_lna[_lna_idx] << 10));
      rf_set(path, 0x56, MASK20, 0x510f3);
      rf_set(path, 0x8f, MASK20, 0xa9c00);
    } else { /* ODM_BAND_5G */
      if (is_gs) {
        _tmp1bcc = 0x09;
        _lna_idx = 2;
      }
      w8(0x1bcc, _tmp1bcc);
      w8(0x1b2b, 0x00);
      w32(0x1b20, 0x00450008);
      w32(0x1b24, 0x01460048u | (wla_lna[_lna_idx] << 10));
      rf_set(path, 0x56, MASK20, 0x51060);
      rf_set(path, 0x8f, MASK20, 0xa9c00);
    }
  }
}

/* --- nctl indirect (LTE-coex) reg access (_iqk_indirect_*_reg) --- */
uint32_t Halrf8821c::indirect_read(uint16_t reg) {
  w32(0x1700, 0x800f0000u | reg);
  uint32_t j = 0;
  while (((_device.rtw_read8(0x1703) & (1u << 5)) == 0) && (j < 30000))
    j++;
  return r32(0x1708);
}
void Halrf8821c::indirect_write(uint16_t reg, uint32_t mask, uint32_t val) {
  if (mask == 0)
    return;
  uint32_t data = val;
  if (mask != 0xffffffffu) {
    uint32_t bitpos = bshift(mask);
    uint32_t cur = indirect_read(reg);
    data = (cur & ~mask) | (val << bitpos);
  }
  w32(0x1704, data);
  uint32_t j = 0;
  while (((_device.rtw_read8(0x1703) & (1u << 5)) == 0) && (j < 30000))
    j++;
  w32(0x1700, 0xc00f0000u | reg);
}

/* _iqk_set_gnt_wl_gnt_bt: before-K forces GNT_WL=1 / GNT_BT=0 via 0x38 fields;
 * after-K restores the saved coex word. */
void Halrf8821c::set_gnt_wl_gnt_bt(bool before_k) {
  if (before_k) {
    /* GNT_WL high: state=1, sw_control=1 -> (1<<1)|1 = 0x3 */
    indirect_write(0x38, 0x3000, 0x3); /* 0x38[13:12] */
    indirect_write(0x38, 0x0300, 0x3); /* 0x38[9:8] */
    /* GNT_BT low: state=0, sw_control=1 -> (0<<1)|1 = 0x1 */
    indirect_write(0x38, 0xc000, 0x1); /* 0x38[15:14] */
    indirect_write(0x38, 0x0c00, 0x1); /* 0x38[11:10] */
  } else {
    indirect_write(0x38, MASKDWORD, _tmp_gntwl);
  }
}

/* _iqk_check_cal_8821c: poll 0x1b00 == (cmd & 0xffffff0f), fail = 0x1b08[26]. */
bool Halrf8821c::check_cal(uint32_t iqk_cmd) {
  bool notready = true, fail = true;
  uint32_t delay_count = 0;
  while (notready) {
    if (r32(0x1b00) == (iqk_cmd & 0xffffff0fu)) {
      fail = bb_get(0x1b08, 1u << 26) != 0;
      notready = false;
    } else {
      udelay(10);
      delay_count++;
    }
    if (delay_count >= 50000) {
      fail = true;
      break;
    }
  }
  return fail;
}

/* _iqk_check_nctl_done_8821c: poll RF0x8 for the done code, fail = 0x1b08[26],
 * then clear RF0x8. */
bool Halrf8821c::check_nctl_done(uint8_t path, uint32_t iqk_cmd) {
  bool notready = true, fail = true;
  uint32_t delay_count = 0;
  const uint32_t done = (((iqk_cmd & 0x00000f00u) >> 8) == 0xc) ? 0x1a3b5u
                                                                : 0x12345u;
  while (notready) {
    if (rf_get(path, 0x08) == done)
      notready = false;
    else
      notready = true;

    if (notready) {
      udelay(10);
      delay_count++;
    } else {
      fail = bb_get(0x1b08, 1u << 26) != 0;
      break;
    }
    if (delay_count >= 50000)
      break;
  }
  rf_set(path, 0x8, MASK20, 0x0);
  return fail;
}

/* _lok_one_shot_8821c: one PAD stage. delay_count widened to u32 (vendor u8
 * never reaches its 50000 cap — safe iteration ceiling). */
bool Halrf8821c::lok_one_shot(uint8_t path, uint8_t /*pad_index*/) {
  uint32_t IQK_CMD = 0xf8000008u | (1u << (4 + path));

  set_gnt_wl_gnt_bt(true);
  w32(0x1b00, IQK_CMD);
  w32(0x1b00, IQK_CMD + 1);
  udelay(10);

  uint32_t delay_count = 0;
  bool notready = true;
  while (notready) {
    if (rf_get(path, 0x8) == 0x12345)
      notready = false;
    else
      notready = true;

    if (notready) {
      udelay(10);
      delay_count++;
    }
    if (delay_count >= 50000)
      break;
  }
  rf_set(path, 0x8, MASK20, 0x0);
  set_gnt_wl_gnt_bt(false);

  _lok_fail = notready;
  return notready;
}

/* _iqk_rx_iqk_gain_search_fail_8821c: RXK1/RXK2 gain search. */
bool Halrf8821c::rxk_gsearch(uint8_t path, uint8_t step) {
  bool fail = true;
  uint32_t IQK_CMD;
  const uint8_t IQMUX[4] = {0x9, 0x12, 0x1b, 0x24};
  const uint8_t *plna;
  uint8_t idx;

  if (_is_btg)
    plna = btg_lna;
  else if (_band2g)
    plna = wlg_lna;
  else
    plna = wla_lna;

  for (idx = 0; idx < 4; idx++)
    if (_tmp1bcc == IQMUX[idx])
      break;

  w32(0x1b00, 0xf8000008u | (path << 1));
  w32(0x1bcc, _tmp1bcc);

  if (step == RXIQK1)
    IQK_CMD = 0xf8000208u | (1u << (path + 4));
  else
    IQK_CMD = 0xf8000308u | (1u << (path + 4));

  set_gnt_wl_gnt_bt(true);
  w32(0x1b00, IQK_CMD);
  w32(0x1b00, IQK_CMD + 0x1);
  fail = check_cal(IQK_CMD);
  set_gnt_wl_gnt_bt(false);

  if (step == RXIQK2) {
    uint32_t rf_reg0 = rf_get(path, 0x0);
    w32(0x1b00, 0xf8000008u | (path << 1));
    uint32_t tmp = (rf_reg0 & 0x1fe0) >> 5;
    uint32_t rxbb = tmp & 0x1f;

    if (rxbb == 0x1) {
      if (idx != 3)
        idx++;
      else if (_lna_idx != 0x0)
        _lna_idx--;
      else
        _isbnd = true;
      fail = true;
    } else if (rxbb == 0xa) {
      if (idx != 0)
        idx--;
      else if (_lna_idx != 0x4)
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
      w32(0x1b24, (r32(0x1b24) & 0xffffc3ffu) | (plna[_lna_idx] << 10));
    }
  }
  return fail;
}

/* _iqk_one_shot_8821c: TXIQK / RXIQK1 / RXIQK2. */
bool Halrf8821c::one_shot(uint8_t path, uint8_t idx) {
  uint32_t IQK_CMD = 0;
  const uint16_t iqk_apply[2] = {0xc94, 0xe94};

  if (idx == TXIQK) {
    IQK_CMD = 0xf8000008u | ((_bw + 4) << 8) | (1u << (path + 4));
  } else if (idx == RXIQK1) {
    IQK_CMD = (_bw == 2) ? (0xf8000808u | (1u << (path + 4)))
                         : (0xf8000708u | (1u << (path + 4)));
  } else { /* RXIQK2 */
    IQK_CMD = 0xf8000008u | ((_bw + 9) << 8) | (1u << (path + 4));
  }

  set_gnt_wl_gnt_bt(true);

  w32(0x1bc8, 0x80000000);
  w32(0x8f8, 0x41400080);

  if (rf_get(path, 0x08) != 0x0)
    rf_set(path, 0x8, MASK20, 0x0);

  w32(0x1b00, IQK_CMD);
  w32(0x1b00, IQK_CMD + 0x1);

  bool fail = check_nctl_done(path, IQK_CMD);

  set_gnt_wl_gnt_bt(false);

  w32(0x1b00, 0xf8000008u | (path << 1));
  if (idx == TXIQK) {
    if (fail)
      bb_set(iqk_apply[path], 1u << 0, 0x0);
    /* success: HW keeps applied TX result (CFIR backup for reload skipped). */
  }
  if (idx == RXIQK2) {
    _rxiqk_agc[path] = static_cast<uint16_t>(
        ((rf_get(path, 0x0) >> 5) & 0xff) | (_tmp1bcc << 8));
    w32(0x1b38, 0x20000000);
    if (fail)
      bb_set(iqk_apply[path], (1u << 11) | (1u << 10), 0x0);
    /* success: HW keeps applied RX result (CFIR backup skipped). */
  }

  if (idx == TXIQK)
    _iqk_fail_report[TX_IQK] = fail;
  else
    _iqk_fail_report[RX_IQK] = fail;

  return fail;
}

/* _iqk_rxiqkbystep_8821c: RXK step machine (xym debug reads are no-ops). */
bool Halrf8821c::rxiqk_by_step(uint8_t path) {
  bool KFAIL = true, gonext;
  switch (_rxiqk_step) {
  case 1: /* gain search RXK1 */
    rxk1_setting(path);
    gonext = false;
    while (true) {
      KFAIL = rxk_gsearch(path, RXIQK1);
      if (KFAIL && _gs_retry[0] < 2) {
        _gs_retry[0]++;
      } else if (KFAIL) {
        _rxiqk_fail_code = 0;
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
      if (KFAIL && _gs_retry[1] < rxiqk_gs_limit)
        _gs_retry[1]++;
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
      if (KFAIL && _retry[RXIQK1] < 2) {
        _retry[RXIQK1]++;
      } else if (KFAIL) {
        _rxiqk_fail_code = 1;
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
      if (KFAIL && _retry[RXIQK2] < 2) {
        _retry[RXIQK2]++;
      } else if (KFAIL) {
        _rxiqk_fail_code = 2;
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

/* _iqk_iqk_by_path_8821c (non-segment: run to iqk_step==4, path A). */
void Halrf8821c::iqk_by_path() {
  const uint8_t path = 0; /* RF_PATH_A */
  bool KFAIL;
  uint32_t guard = 0;
  while (true) {
    switch (_iqk_step) {
    case 1: /* S0 LOK: sweep 8 PAD stages */
      for (uint8_t i = 0; i < 8; i++) {
        lok_setting(path, i);
        lok_one_shot(path, i);
      }
      _iqk_step++;
      break;
    case 2: /* S0 TXIQK */
      txk_setting(path);
      KFAIL = one_shot(path, TXIQK);
      _kcount++;
      if (KFAIL && _retry[TXIQK] < 3)
        _retry[TXIQK]++;
      else
        _iqk_step++;
      break;
    case 3: /* S0 RXIQK */
      while (true) {
        KFAIL = rxiqk_by_step(path);
        (void)KFAIL;
        if (_rxiqk_step == 5) {
          _iqk_step++;
          _rxiqk_step = 1;
          break;
        }
      }
      _kcount++;
      break;
    }

    if (_iqk_step == 4) {
      w32(0x1b00, 0xf8000008u | (path << 1));
      w32(0x1b2c, 0x7);
      w32(0x1bcc, 0x0);
      w32(0x1b38, 0x20000000);
      break;
    }
    /* segment_iqk == false: no kcount-limit break. Hard safety ceiling. */
    if (++guard > 100)
      break;
  }
}

void Halrf8821c::start_iqk() {
  w32(0x1b00, 0xf8000008);
  w32(0x1bb8, 0x00000000);
  /* GNT_WL = 1 (RF 0x1) */
  uint32_t tmp = rf_get(0, 0x1);
  if (_is_btg)
    tmp = (tmp & ~(1u << 3)) | (1u << 0) | (1u << 2) | (1u << 5);
  else
    tmp = ((tmp & ~(1u << 3)) & ~(1u << 5)) | (1u << 0) | (1u << 2);
  rf_set(0, 0x1, MASK20, tmp);
  iqk_by_path();
}

/* _iq_calibrate_8821c_init: software struct init (iqc_matrix default). Inert
 * here — no reload path consumes it — but kept for parity. */
void Halrf8821c::iqk_init() {
  _lok_fail = true;
  _iqk_fail_report[TX_IQK] = true;
  _iqk_fail_report[RX_IQK] = true;
  _iqc_matrix[0] = 0x20000000;
  _iqc_matrix[1] = 0x20000000;
  for (int k = 0; k < 3; k++)
    _retry[k] = 0;
}

/* _iqk_fill_iqk_report_8821c (channel 0, path A). */
void Halrf8821c::fill_iqk_report() {
  uint32_t tmp1 = (_iqk_fail_report[TX_IQK] & 0x1u) << 0;
  uint32_t tmp2 = (_iqk_fail_report[RX_IQK] & 0x1u) << 4;
  uint32_t tmp3 = (_rxiqk_fail_code & 0x3u) << 8;
  w32(0x1b00, 0xf8000008);
  bb_set(0x1bf0, 0x00ffffff, tmp1 | tmp2 | tmp3);
  w32(0x1be8, (static_cast<uint32_t>(_rxiqk_agc[1]) << 16) | _rxiqk_agc[0]);
}

/* _phy_iq_calibrate_8821c — fresh (non-segment, no reload) IQK. */
void Halrf8821c::iqk_trigger(bool band2g) {
  _band2g = band2g;
  _bw = 0; /* 20 MHz */

  uint32_t mac_bk[3], bb_bk[16], rf_bk[5];
  const uint32_t mac_reg[3] = {0x520, 0x550, 0x1518};
  /* The vendor backup list is {0x808,0x90c,0xc00,0xcb0,0xcb4,0xcbc,0x1990,0x9a4,
   * 0xa04,0x838} — it relies on its continued post-IQK PHY init to reset the
   * OTHER registers configure_macbb / afe_setting(false) disturb. devourer's
   * 8821C config_trx_mode is a no-op, so nothing resets them and OFDM/HT TX
   * stays broken after IQK (CCK uses a different path, so 1M keeps working).
   * Extend the backup so those are saved (operational value, pre-IQK) and
   * restored — restore_mac_bb runs AFTER afe_setting(false), so it wins:
   *   0xc94 ("tx from IQK"), 0xb00[8] ("disable PMAC"), and the AFE quad
   *   0xc58/0xc5c/0xc60/0xc6c (afe_setting(false) leaves these at IQK-exit
   *   values, not the operational ones the BB tables set). */
  const uint32_t bb_reg[16] = {0x808,  0x90c, 0xc00,  0xcb0, 0xcb4, 0xcbc,
                               0x1990, 0x9a4, 0xa04,  0x838, 0xc94, 0xb00,
                               0xc58,  0xc5c, 0xc60,  0xc6c};
  const uint32_t rf_reg[5] = {0xdf, 0xde, 0x8f, 0x0, 0x1};

  /* is_btg from BB 0xcb8[16] (RFE front-end config). */
  _is_btg = bb_get(0xcb8, 1u << 16) != 0;

  iqk_init();

  /* reset run state */
  _kcount = 0;
  _iqk_step = 1;
  _rxiqk_step = 1;
  _lok_fail = true;
  _rxiqk_fail_code = 0;
  _isbnd = false;
  _tmp1bcc = 0x12;
  _lna_idx = 0;
  _rxiqk_agc[0] = 0;
  _rxiqk_agc[1] = 0;
  for (int k = 0; k < 3; k++)
    _retry[k] = 0;
  for (int k = 0; k < 2; k++)
    _gs_retry[k] = 0;

  _tmp_gntwl = indirect_read(0x38);
  backup_mac_bb(mac_bk, bb_bk, mac_reg, bb_reg);
  backup_rf(rf_bk, rf_reg);

  uint32_t guard = 0;
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
    if (_iqk_step == 4)
      break;
    _kcount = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (++guard > 4)
      break;
  }

  fill_iqk_report();

  _logger->info("Jaguar2/8821C IQK done (is_btg={}): LOK fail={} | TXK retry={} "
                "| RXK gs_retry(g1={} g2={}) 1shot_retry(k1={} k2={}) | "
                "TX/RX fail={}/{} RXK failcode={} "
                "(0=gs1,1=rxk1,2=rxk2; only meaningful if that step ran)",
                _is_btg, _lok_fail, _retry[TXIQK], _gs_retry[0], _gs_retry[1],
                _retry[RXIQK1], _retry[RXIQK2], _iqk_fail_report[TX_IQK],
                _iqk_fail_report[RX_IQK], _rxiqk_fail_code);
}

/* ===== Thermal TX-power tracking =================================
 * 1T1R (path A only). Vendor MIX_MODE port — tables from reference/8821cu/hal/
 * phydm/halrf/rtl8821c/halhwimg8821c_rf.c (delta_swingidx_mp_*_txpwrtrk_8821c,
 * D_S_SIZE=30); tx_scaling_table_jaguar shared with the 8822B (37 entries). The
 * 8821C writes the coarse TXAGC swing to 0xc94[6:1] (6-bit) vs the 8822B's
 * [29:25], and has no path B. */
namespace {
constexpr int D_S = 30;

const uint32_t tx_scaling_table_jaguar[37] = {
    0x081, 0x088, 0x090, 0x099, 0x0A2, 0x0AC, 0x0B6, 0x0C0, 0x0CC, 0x0D8,
    0x0E5, 0x0F2, 0x101, 0x110, 0x120, 0x131, 0x143, 0x156, 0x16A, 0x180,
    0x197, 0x1AF, 0x1C8, 0x1E3, 0x200, 0x21E, 0x23E, 0x261, 0x285, 0x2AB,
    0x2D3, 0x2FE, 0x32B, 0x35C, 0x38E, 0x3C4, 0x3FE};

const uint8_t k2ga_p[D_S] = {0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 4, 4, 5,
                             5, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 9, 9, 9, 9};
const uint8_t k2ga_n[D_S] = {0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3, 4,
                             4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 7, 7, 8, 8, 9};
const uint8_t k5ga_p[3][D_S] = {
    {0, 1, 1, 2, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 11, 11, 12, 12, 12, 12, 12, 12, 12},
    {0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 5, 6, 7, 7, 8, 8, 9, 10, 10, 11, 11, 12, 12, 12, 12, 12, 12, 12, 12},
    {0, 1, 1, 1, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 7, 8, 8, 9, 10, 10, 11, 11, 12, 12, 12, 12, 12, 12, 12}};
const uint8_t k5ga_n[3][D_S] = {
    {0, 1, 1, 2, 3, 3, 3, 4, 4, 5, 5, 6, 6, 6, 7, 8, 8, 8, 9, 9, 9, 10, 10, 11, 11, 12, 12, 12, 12, 12},
    {0, 1, 1, 1, 2, 3, 3, 4, 4, 5, 5, 5, 6, 6, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 12, 12, 12, 12, 12},
    {0, 1, 2, 2, 3, 4, 4, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 9, 10, 10, 11, 11, 12, 12, 12, 12, 12, 12}};
} /* namespace */

void Halrf8821c::set_pwr_track_ctx(uint8_t baseline, uint8_t channel) {
  _pt_baseline = baseline;
  _pt_channel = channel;
  for (auto &x : _pt_avg)
    x = 0;
  _pt_avg_idx = 0;
  _pt_last_swing = 0x7fffffff;
  _pt_default_ofdm = 24;
  const uint32_t cur = bb_get(0xc1c, 0xFFE00000);
  for (int i = 0; i < 37; i++)
    if (tx_scaling_table_jaguar[i] == cur) {
      _pt_default_ofdm = static_cast<uint8_t>(i);
      break;
    }
  _logger->info("Jaguar2 8821C thermal-track: baseline={} ch={} default_ofdm={}",
                baseline, channel, _pt_default_ofdm);
}

void Halrf8821c::pwr_track_write(int swing, int cur_ofdm_idx) {
  const int tx_pwr_idx = cur_ofdm_idx < 0 ? 63 : (cur_ofdm_idx > 63 ? 63 : cur_ofdm_idx);
  int headroom = 63 - tx_pwr_idx;
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
  } else {
    tx_agc_index = 0;
    bb_swing_index = def > -swing ? def + swing : 0;
  }
  if (bb_swing_index < 0)
    bb_swing_index = 0;
  if (bb_swing_index > 36)
    bb_swing_index = 36;
  bb_set(0xc94, 0x0000007E, static_cast<uint32_t>(tx_agc_index) & 0x3f); /* [6:1] */
  bb_set(0xc1c, 0xFFE00000, tx_scaling_table_jaguar[bb_swing_index]);
}

void Halrf8821c::pwr_track(int current_ofdm_index) {
  if (_pt_baseline == 0xff)
    return;
  const bool g2 = _pt_channel <= 14;
  int sub = 0;
  if (!g2)
    sub = _pt_channel <= 64 ? 0 : (_pt_channel <= 144 ? 1 : 2);
  const int cur = static_cast<int>((rf_get(0, 0x42) >> 10) & 0x3f);
  _pt_avg[_pt_avg_idx] = cur;
  _pt_avg_idx = (_pt_avg_idx + 1) & 3;
  int sum = 0, cnt = 0;
  for (int i = 0; i < 4; i++)
    if (_pt_avg[i]) {
      sum += _pt_avg[i];
      cnt++;
    }
  if (!cnt)
    return; /* meter not ready — no valid sample yet */
  const int avg = sum / cnt;
  int delta = avg > _pt_baseline ? avg - _pt_baseline : _pt_baseline - avg;
  if (delta > D_S - 1)
    delta = D_S - 1;
  const uint8_t *up = g2 ? k2ga_p : k5ga_p[sub];
  const uint8_t *dn = g2 ? k2ga_n : k5ga_n[sub];
  const int swing = avg > _pt_baseline ? up[delta] : -static_cast<int>(dn[delta]);
  if (swing == _pt_last_swing)
    return;
  _pt_last_swing = swing;
  pwr_track_write(swing, current_ofdm_index);
  _logger->info("Jaguar2 8821C thermal-track: avg={} d={} swing={}", avg, delta,
                swing);
}

/* Calibration-factory hook, called by make_jaguar2_calibration() in
 * Halrf8822b.cpp when the ChipVariant is C8821C. */
std::unique_ptr<Jaguar2Calibration>
make_calibration_8821c(RtlAdapter device, Logger_t logger, uint8_t cut,
                       bool is_2t2r) {
  return std::make_unique<Halrf8821c>(std::move(device), std::move(logger), cut,
                                      is_2t2r);
}

} /* namespace jaguar2 */
