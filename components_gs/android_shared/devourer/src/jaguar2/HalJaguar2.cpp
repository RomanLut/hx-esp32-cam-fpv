#include "HalJaguar2.h"

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(DEVOURER_HAVE_JAGUAR2_8822B)
#include "Hal8822b_TxpwrLmt.h" /* generated: hal8822b_txpwr_lmt() WW-min limits */
#endif
#if defined(DEVOURER_HAVE_JAGUAR2_8821C)
#include "Hal8821c_TxpwrLmt.h" /* generated: hal8821c_txpwr_lmt() WW-min limits */
#endif
#include "ChannelFreq.h" /* chan_to_freq for the spur-notch center */
#include "HopProf.h" /* DEVOURER_HOP_PROF fast-retune stage timing */
#include "PhyTableLoader.h"
#include "ToneMask.h" /* NBI notch + CSI mask appliers (spur calibration) */

namespace jaguar2 {

namespace {
/* RTL8822B USB power sequence, transcribed from the halmac WLAN_PWR_CFG flow
 * (reference/rtl88x2bu/hal/halmac/halmac_88xx/halmac_8822b/halmac_pwr_seq_8822b.c),
 * keeping the USB- and ALL-interface entries and dropping the SDIO/PCI-only
 * steps. Each step is a byte read-modify-write, a byte poll, or a delay. The
 * 0x10A8..0x10AA writes are cut-C-only in halmac; production RTL8822BU is C-cut
 * (SYS_CFG1 cut field = 2), so they are included. */
enum PwrCmd { PC_WRITE, PC_POLL, PC_DELAY, PC_END };
struct PwrCfg { uint16_t off; uint8_t cmd; uint8_t msk; uint8_t val; };
constexpr uint8_t B(int n) { return static_cast<uint8_t>(1u << n); }

/* card_en_flow_8822b = CARDDIS_TO_CARDEMU + CARDEMU_TO_ACT (USB/ALL). */
const PwrCfg kPwrOn8822bUsb[] = {
    /* --- card-disable -> card-emulation --- */
    {0x004A, PC_WRITE, B(0), 0},
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(3) | B(4) | B(7)), 0},
    /* --- card-emulation -> active --- */
    {0xFF0A, PC_WRITE, 0xFF, 0},
    {0xFF0B, PC_WRITE, 0xFF, 0},
    {0x0012, PC_WRITE, B(1), 0},
    {0x0012, PC_WRITE, B(0), B(0)},
    {0x0020, PC_WRITE, B(0), B(0)},
    {0x0001, PC_DELAY, 0, 1}, /* 1 ms */
    {0x0000, PC_WRITE, B(5), 0},
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(4) | B(3) | B(2)), 0},
    {0x0006, PC_POLL, B(1), B(1)},
    {0xFF1A, PC_WRITE, 0xFF, 0},
    {0x0006, PC_WRITE, B(0), B(0)},
    {0x0005, PC_WRITE, B(7), 0},
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(4) | B(3)), 0},
    {0x10C3, PC_WRITE, B(0), B(0)},
    {0x0005, PC_WRITE, B(0), B(0)},
    {0x0005, PC_POLL, B(0), 0},
    {0x0020, PC_WRITE, B(3), B(3)},
    {0x10A8, PC_WRITE, 0xFF, 0},    /* cut-C */
    {0x10A9, PC_WRITE, 0xFF, 0xef}, /* cut-C */
    {0x10AA, PC_WRITE, 0xFF, 0x0c}, /* cut-C */
    {0x0029, PC_WRITE, 0xFF, 0xF9},
    {0x0024, PC_WRITE, B(2), 0},
    {0x00AF, PC_WRITE, B(5), B(5)},
    {0, PC_END, 0, 0},
};

/* card_dis_flow_8822b = ACT_TO_CARDEMU + CARDEMU_TO_CARDDIS (USB/ALL). */
const PwrCfg kPwrOff8822bUsb[] = {
    /* --- active -> card-emulation --- */
    {0x0093, PC_WRITE, 0xFF, 0xC4},
    {0x001F, PC_WRITE, 0xFF, 0},
    {0x00EF, PC_WRITE, 0xFF, 0},
    {0xFF1A, PC_WRITE, 0xFF, 0x30},
    {0x0049, PC_WRITE, B(1), 0},
    {0x0006, PC_WRITE, B(0), B(0)},
    {0x0002, PC_WRITE, B(1), 0},
    {0x10C3, PC_WRITE, B(0), 0},
    {0x0005, PC_WRITE, B(1), B(1)},
    {0x0005, PC_POLL, B(1), 0},
    {0x0020, PC_WRITE, B(3), 0},
    {0x0000, PC_WRITE, B(5), B(5)},
    /* --- card-emulation -> card-disable --- */
    {0x0007, PC_WRITE, 0xFF, 0x20},
    {0x0067, PC_WRITE, B(5), 0},
    {0x004A, PC_WRITE, B(0), 0},
    {0x0081, PC_WRITE, static_cast<uint8_t>(B(7) | B(6)), 0},
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(3) | B(4)), B(3)},
    {0x0090, PC_WRITE, B(1), 0},
    {0, PC_END, 0, 0},
};

/* card_en_flow_8821c = CARDDIS_TO_CARDEMU + CARDEMU_TO_ACT (USB/ALL),
 * transcribed from reference/8821cu/hal/halmac/halmac_88xx/halmac_8821c/
 * halmac_pwr_seq_8821c.c. Every row is PWR_CUT_ALL_MSK (no cut-specific rows,
 * unlike 8822B). PCI/SDIO-only rows dropped. */
const PwrCfg kPwrOn8821cUsb[] = {
    /* --- card-disable -> card-emulation --- */
    {0x004A, PC_WRITE, B(0), 0},
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(3) | B(4) | B(7)), 0},
    /* --- card-emulation -> active --- */
    {0x0020, PC_WRITE, B(0), B(0)},
    {0x0001, PC_DELAY, 0, 1}, /* 1 ms */
    {0x0000, PC_WRITE, B(5), 0},
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(4) | B(3) | B(2)), 0},
    {0x0006, PC_POLL, B(1), B(1)},
    {0x0006, PC_WRITE, B(0), B(0)},
    {0x0005, PC_WRITE, B(7), 0},
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(4) | B(3)), 0},
    {0x10C3, PC_WRITE, B(0), B(0)},
    {0x0005, PC_WRITE, B(0), B(0)},
    {0x0005, PC_POLL, B(0), 0},
    {0x0020, PC_WRITE, B(3), B(3)},
    {0x007C, PC_WRITE, B(1), 0},
    {0, PC_END, 0, 0},
};

/* card_dis_flow_8821c = ACT_TO_CARDEMU + CARDEMU_TO_CARDDIS (USB/ALL). */
const PwrCfg kPwrOff8821cUsb[] = {
    /* --- active -> card-emulation --- */
    {0x0093, PC_WRITE, 0xFF, 0xC4},
    {0x001F, PC_WRITE, 0xFF, 0},
    {0x0049, PC_WRITE, B(1), 0},
    {0x0006, PC_WRITE, B(0), B(0)},
    {0x0002, PC_WRITE, B(1), 0},
    {0x10C3, PC_WRITE, B(0), 0},
    {0x0005, PC_WRITE, B(1), B(1)},
    {0x0005, PC_POLL, B(1), 0},
    {0x0020, PC_WRITE, B(3), 0},
    {0x0000, PC_WRITE, B(5), B(5)},
    /* --- card-emulation -> card-disable --- */
    {0x0007, PC_WRITE, 0xFF, 0x20},
    {0x0067, PC_WRITE, B(5), 0},
    {0x004A, PC_WRITE, B(0), 0},
    {0x0081, PC_WRITE, static_cast<uint8_t>(B(7) | B(6)), 0},
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(3) | B(4)), B(3)},
    {0x0090, PC_WRITE, B(1), 0},
    {0, PC_END, 0, 0},
};

#if defined(DEVOURER_HAVE_PCIE)
/* PCIe (RTL8821CE) power sequences, transcribed from the in-tree rtw88
 * rtw8821c.c tables (v6.12 — the driver proven on this exact silicon), keeping
 * the PCI- and ALL-interface rows and dropping USB/SDIO-only steps. The
 * PCI-only enable rows gate the PCIe reference clock/power (0x75[0] toggle
 * around the 0x06[1] poll, 0x74[5], 0x22[1] clear, 0x62/0x61 bit dance) —
 * without them the power-on polls fail on the E-variant. */
const PwrCfg kPwrOn8821cPcie[] = {
    /* --- card-disable -> card-emulation --- */
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(3) | B(4) | B(7)), 0},
    {0x0300, PC_WRITE, 0xFF, 0},
    {0x0301, PC_WRITE, 0xFF, 0},
    /* --- card-emulation -> active --- */
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(4) | B(3) | B(2)), 0},
    {0x0075, PC_WRITE, B(0), B(0)},
    {0x0006, PC_POLL, B(1), B(1)},
    {0x0075, PC_WRITE, B(0), 0},
    {0x0006, PC_WRITE, B(0), B(0)},
    {0x0005, PC_WRITE, B(7), 0},
    {0x0005, PC_WRITE, static_cast<uint8_t>(B(4) | B(3)), 0},
    {0x0005, PC_WRITE, B(0), B(0)},
    {0x0005, PC_POLL, B(0), 0},
    {0x0020, PC_WRITE, B(3), B(3)},
    {0x0074, PC_WRITE, B(5), B(5)},
    {0x0022, PC_WRITE, B(1), 0},
    {0x0062, PC_WRITE, static_cast<uint8_t>(B(7) | B(6) | B(5)),
     static_cast<uint8_t>(B(7) | B(6) | B(5))},
    {0x0061, PC_WRITE, static_cast<uint8_t>(B(7) | B(6) | B(5)), 0},
    {0x007C, PC_WRITE, B(1), 0},
    {0, PC_END, 0, 0},
};

const PwrCfg kPwrOff8821cPcie[] = {
    /* --- active -> card-emulation --- */
    {0x0093, PC_WRITE, B(3), 0},
    {0x001F, PC_WRITE, 0xFF, 0},
    {0x0049, PC_WRITE, B(1), 0},
    {0x0006, PC_WRITE, B(0), B(0)},
    {0x0002, PC_WRITE, B(1), 0},
    {0x0005, PC_WRITE, B(1), B(1)},
    {0x0005, PC_POLL, B(1), 0},
    {0x0020, PC_WRITE, B(3), 0},
    /* --- card-emulation -> card-disable --- */
    {0x0067, PC_WRITE, B(5), 0},
    {0x0005, PC_WRITE, B(2), B(2)},
    {0x0081, PC_WRITE, static_cast<uint8_t>(B(7) | B(6)), 0},
    {0x0090, PC_WRITE, B(1), 0},
    {0, PC_END, 0, 0},
};
#endif /* DEVOURER_HAVE_PCIE */

void run_pwr_seq(RtlAdapter &dev, const PwrCfg *seq, uint32_t poll_max,
                 bool poll_fatal) {
  /* rtw88 rtw_pwr_cmd_polling PCIe quirk: a timed-out POLL step is retried
   * once after toggling BIT_PFM_WOWL in REG_SYS_PW_CTRL (0x04[3]) set->clear —
   * a known 8821CE cold-boot fixer. Applied on the PCIe backend only. */
  auto poll_once = [&](const PwrCfg *p) -> bool {
    uint32_t cnt = poll_max;
    while ((dev.rtw_read8(p->off) & p->msk) != (p->val & p->msk)) {
      if (--cnt == 0)
        return false;
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
    return true;
  };
  for (const PwrCfg *p = seq; p->cmd != PC_END; ++p) {
    if (p->cmd == PC_WRITE) {
      uint8_t v = dev.rtw_read8(p->off);
      v = static_cast<uint8_t>((v & ~p->msk) | (p->val & p->msk));
      dev.rtw_write8(p->off, v);
    } else if (p->cmd == PC_DELAY) {
      std::this_thread::sleep_for(std::chrono::milliseconds(p->val));
    } else { /* PC_POLL */
      bool ok = poll_once(p);
      if (!ok && !dev.is_usb()) {
        uint8_t v = dev.rtw_read8(0x0004);
        dev.rtw_write8(0x0004, static_cast<uint8_t>(v | (1u << 3)));
        dev.rtw_write8(0x0004, static_cast<uint8_t>(v & ~(1u << 3)));
        ok = poll_once(p);
      }
      if (!ok && poll_fatal)
        throw std::runtime_error(
            "Jaguar2: power-on poll timeout (chip not responding)");
    }
  }
}
} /* namespace */

HalJaguar2::HalJaguar2(RtlAdapter device, Logger_t logger,
                       ChipVariant variant, const devourer::DeviceConfig &cfg)
    : _device{std::move(device)}, _logger{std::move(logger)}, _cfg{cfg},
      _variant{variant}, _tables{make_jaguar2_phy_tables(variant)} {}

void HalJaguar2::power_off() {
  const PwrCfg *seq =
      _variant == ChipVariant::C8821C ? kPwrOff8821cUsb : kPwrOff8822bUsb;
#if defined(DEVOURER_HAVE_PCIE)
  if (!_device.is_usb())
    seq = kPwrOff8821cPcie; /* only the 8821CE exists on PCIe */
#endif
  run_pwr_seq(_device, seq, /*poll_max=*/2000, /*poll_fatal=*/false);
  _logger->info("Jaguar2: power-off (card-disable) sequence applied");
}

void HalJaguar2::power_on() {
  power_off(); /* reset from any prior (kernel-left active) state first */
  const PwrCfg *seq =
      _variant == ChipVariant::C8821C ? kPwrOn8821cUsb : kPwrOn8822bUsb;
#if defined(DEVOURER_HAVE_PCIE)
  if (!_device.is_usb())
    seq = kPwrOn8821cPcie;
#endif
  run_pwr_seq(_device, seq, /*poll_max=*/5000, /*poll_fatal=*/true);
  _logger->info("Jaguar2: power-on sequence complete (card active)");
}

void HalJaguar2::read_chip_version() {
  constexpr uint16_t REG_SYS_CFG1_8822B = 0x00F0;
  uint32_t v = _device.rtw_read32(REG_SYS_CFG1_8822B);

  _ver.test_chip = (v & (1u << 23)) != 0;
  _ver.cut = static_cast<uint8_t>((v >> 12) & 0xf);
  _ver.rf_2t2r = (v & (1u << 27)) ? 1 : 0;
  uint8_t vend = static_cast<uint8_t>(((v >> 16) & 0xf) >> 2);
  _ver.vendor = (vend <= 2) ? vend : 0;

  static const char *vn[] = {"TSMC", "SMIC", "UMC"};
  const char *chip = _variant == ChipVariant::C8821C ? "8821C" : "8822B";
  _logger->info("Jaguar2 chip: {} cut={} ({}{}) {} (SYS_CFG1=0x{:08x})", chip,
                _ver.cut, vn[_ver.vendor], _ver.test_chip ? ",test" : "",
                _ver.rf_2t2r ? "2T2R" : "1T1R", v);
}

void HalJaguar2::read_efuse_logical_map(uint8_t *map, uint16_t map_size,
                                        bool dump) {
  /* Standard 88xx EFUSE logical-map decode (efuse_OneByteRead + 2-byte
   * extended-header format). Ported from the jaguar3 non-EU path; 8822B uses
   * the same physical primitive.
   *
   * The physical efuse stores logical words in an arbitrary order (a block for a
   * high logical offset can appear physically before a block for a low offset),
   * so the whole chain must be walked to the natural end (hdr == 0xFF). An
   * earlier version broke out as soon as a block's base exceeded a target offset
   * — but the VID/PID block (logical base 0x100) appears near the *start* of the
   * physical layout, so the walk aborted long before reaching the physically-
   * later blocks (RFE 0xCA always read back 0xFF). */
  constexpr uint16_t kPhysMax = 1024;
  for (uint16_t i = 0; i < map_size; i++)
    map[i] = 0xFF;

  auto rd = [this](uint16_t a) -> uint8_t {
    uint8_t d = 0xFF;
    return _device.efuse_OneByteRead(a, &d) ? d : 0xFF;
  };

  if (dump) {
    /* Raw physical efuse dump (first 0x100 bytes) for hand-decoding the header
     * chain — proves whether the logical-map walk sees the programmed words. */
    char line[3 * 16 + 1];
    for (uint16_t p = 0; p < 0x100; p += 16) {
      int n = 0;
      for (uint16_t i = 0; i < 16; i++)
        n += snprintf(line + n, sizeof(line) - n, "%02x ", rd(p + i));
      _logger->info("Jaguar2: EFUSE phys[{:03x}]: {}", p, line);
    }
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
        continue;
      offset = static_cast<uint16_t>(((ext & 0xF0) >> 1) | ((hdr & 0xE0) >> 5));
      word_en = ext & 0x0F;
    } else {
      offset = (hdr >> 4) & 0x0F;
      word_en = hdr & 0x0F;
    }
    uint16_t base = static_cast<uint16_t>(offset << 3);
    for (uint8_t i = 0; i < 4; i++) {
      if (word_en & (1u << i))
        continue;
      for (uint8_t k = 0; k < 2; k++) {
        uint8_t d = rd(phys++);
        uint16_t idx = static_cast<uint16_t>(base + i * 2 + k);
        if (idx < map_size)
          map[idx] = d;
      }
    }
  }
  if (dump) {
    _logger->info("Jaguar2: EFUSE dump VID={:02x}{:02x} PID={:02x}{:02x} "
                  "chplan(0xB8)={:02x} rfe(0xCA)={:02x} txcal(0xC8)={:02x}",
                  map[0x101], map[0x100], map[0x103], map[0x102], map[0xB8],
                  map[0xCA], map[0xC8]);
  }
}

uint8_t HalJaguar2::efuse_logical_byte(uint16_t off) {
  if (!_efuse_valid) {
    read_efuse_logical_map(_efuse_map, sizeof(_efuse_map), /*dump=*/false);
    _efuse_valid = true;
  }
  return off < sizeof(_efuse_map) ? _efuse_map[off] : 0xFF;
}

uint8_t HalJaguar2::read_efuse_rfe() {
  constexpr uint16_t kRfeOff = 0xCA; /* EEPROM_RFE_OPTION_8822B */
  read_efuse_logical_map(_efuse_map, sizeof(_efuse_map),
                         _cfg.debug.efuse_dump);
  _efuse_valid = true; /* cache for apply_tx_power (avoid a 2nd physical walk) */
  uint8_t rfe = _efuse_map[kRfeOff];
  /* Unprogrammed efuse (0xFF) falls back to rfe_type 0 — matches the vendor
   * Hal_ReadRFEType_8822b error path (rtl8822b_ops.c). Many retail 8812BU/
   * 8822BU dongles (incl. the Archer T3U) ship with a blank RFE byte. */
  if (rfe == 0xFF) {
    _logger->info("Jaguar2: EFUSE rfe_type unprogrammed (0xff) -> default 0");
    rfe = 0;
  } else {
    _logger->info("Jaguar2: EFUSE rfe_type=0x{:02x}", rfe);
  }
  return rfe;
}

void HalJaguar2::phydm_pre_post_setting(bool post) {
  /* config_phydm_parameter_init_8822b: 0x808[29:28] = 0 (pre, disable OFDM/CCK)
   * or 0x3 (post, enable). */
  _device.phy_set_bb_reg(0x808, (1u << 28) | (1u << 29), post ? 0x3 : 0x0);
}

void HalJaguar2::bb_write(uint32_t addr, uint32_t value) {
  switch (addr) {
  case 0xfe: std::this_thread::sleep_for(std::chrono::milliseconds(50)); return;
  case 0xfd: std::this_thread::sleep_for(std::chrono::milliseconds(5)); return;
  case 0xfc: std::this_thread::sleep_for(std::chrono::milliseconds(1)); return;
  case 0xfb: std::this_thread::sleep_for(std::chrono::microseconds(50)); return;
  case 0xfa: std::this_thread::sleep_for(std::chrono::microseconds(5)); return;
  case 0xf9: std::this_thread::sleep_for(std::chrono::microseconds(1)); return;
  default: break;
  }
  _device.phy_set_bb_reg(static_cast<uint16_t>(addr), 0xFFFFFFFFu, value);
  std::this_thread::sleep_for(std::chrono::microseconds(1));
}

void HalJaguar2::rf_write(uint8_t path, uint32_t addr, uint32_t value) {
  if (addr == 0xfe || addr == 0xffe) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return;
  }
  /* Jaguar 3-wire LSSI write: BB reg 0xC90 (path A) / 0xE90 (path B),
   * data = (rf_addr << 20) | (rf_data & 0xfffff), masked to 28 bits. */
  const uint16_t lssi = (path == 0) ? 0x0C90 : 0x0E90;
  uint32_t data_and_addr =
      (((addr & 0xff) << 20) | (value & 0x000fffffu)) & 0x0fffffffu;
  _device.phy_set_bb_reg(lssi, 0xFFFFFFFFu, data_and_addr);
  std::this_thread::sleep_for(std::chrono::microseconds(1));
}

void HalJaguar2::apply_bb_rf_agc_tables(uint8_t rfe_type) {
  JaguarPhyContext ctx{};
  ctx.cut_version = _ver.cut;
  ctx.support_interface = 0x02; /* ODM_ITRF_USB */
  ctx.support_platform = 0x04;  /* ODM_CE */
  ctx.package_type = 0;
  /* The phydm table conditional blocks are gated on the DECODED rfe_type. On
   * 8821C the raw efuse RFE byte is `rfe_type_expand` and the value the tables
   * match against is `rfe_type_expand >> 3` (config_phydm_parameter_init_8821c:
   * `dm->rfe_type = dm->rfe_type_expand >> 3`). Feeding the raw byte (e.g. 0x22)
   * matches no rfe block, so the rfe-specific AGC/RF rows never apply. 8822B
   * uses the raw byte directly. */
  ctx.rfe_type =
      (_variant == ChipVariant::C8821C) ? (rfe_type >> 3) : rfe_type;

  /* rtl8822b_phy.c order: PRE -> init_bb_reg (phy_reg, agc_tab) ->
   * init_rf_reg (radioa, radiob) -> POST. */
  phydm_pre_post_setting(/*post=*/false);

  auto bb = [this](uint32_t a, uint32_t v) { bb_write(a, v); };
  const auto phy_reg = _tables->phy_reg();
  const auto agc_tab = _tables->agc_tab();
  _logger->info("Jaguar2: applying BB phy_reg ({} words) + agc_tab ({} words)",
                phy_reg.len, agc_tab.len);
  PhyTableLoader::Load(phy_reg.data, phy_reg.len, ctx, bb);
  PhyTableLoader::Load(agc_tab.data, agc_tab.len, ctx, bb);

  const auto radioa = _tables->radioa();
  const auto radiob = _tables->radiob();
  _logger->info("Jaguar2: applying RF radioa ({} words) + radiob ({} words)",
                radioa.len, radiob.len);
  PhyTableLoader::Load(radioa.data, radioa.len, ctx,
                       [this](uint32_t a, uint32_t v) { rf_write(0, a, v); });
  /* 1T1R chips (8821C) supply no radiob table (path A only) — skip the path-B
   * RF walk rather than feed the loader a null table. */
  if (radiob.len)
    PhyTableLoader::Load(radiob.data, radiob.len, ctx,
                         [this](uint32_t a, uint32_t v) { rf_write(1, a, v); });

  phydm_pre_post_setting(/*post=*/true);

  /* 8821C: snapshot the CCK TX-filter defaults the BB table just loaded
   * (config_phydm_parameter_init_8821c POST saves rega24/28/aac_8821c). The
   * per-channel 2.4G tune restores these for non-ch14. */
  if (_variant == ChipVariant::C8821C) {
    _cck_a24_8821c = _device.rtw_read32(0x0a24);
    _cck_a28_8821c = _device.rtw_read32(0x0a28);
    _cck_aac_8821c = _device.rtw_read32(0x0aac);
    _cck_saved_8821c = true;
  }
  _logger->info("Jaguar2: BB/AGC/RF tables applied (rfe_type=0x{:02x})",
                rfe_type);
}

namespace {
uint32_t bit_shift(uint32_t mask) {
  if (mask == 0)
    return 0;
  uint32_t s = 0;
  while (!((mask >> s) & 1u))
    s++;
  return s;
}
} /* namespace */

uint32_t HalJaguar2::rf_read(uint8_t path, uint32_t addr) {
  /* config_phydm_read_rf_reg_8822b: 8822B reads RF registers through a direct
   * BB shadow window (NOT the 3-wire LSSI readback the old driver PHY_QueryRFReg
   * uses) — base 0x2800 (path A) / 0x2c00 (path B) + (rf_addr & 0xff) << 2,
   * masked to the 20-bit RF register width. This is the mechanism all phydm/RF
   * code (channel set, IQK) uses; the 3-wire readback returns stale/garbage and
   * corrupted every masked RF read-modify-write. */
  const uint32_t base = (path == 0) ? 0x2800u : 0x2c00u;
  const uint16_t direct = static_cast<uint16_t>(base + ((addr & 0xff) << 2));
  return _device.rtw_read32(direct) & 0x000FFFFFu;
}

void HalJaguar2::rf_set(uint8_t path, uint32_t addr, uint32_t mask,
                        uint32_t value) {
  if (mask == 0)
    return;
  uint32_t data = value;
  if (mask != 0x000FFFFF) { /* not a full-register (RFREGOFFSETMASK) write */
    uint32_t orig = rf_read(path, addr);
    data = (orig & ~mask) | (value << bit_shift(mask));
  }
  rf_write(path, addr, data);
}

/* phydm_rfe_ifem (rfe_type 0 path) — antenna-switch pins for 2.4G/5G. */
void HalJaguar2::rfe_ifem(uint8_t channel) {
  const bool g2 = channel <= 14;
  if (g2) {
    _device.phy_set_bb_reg(0x0cb0, 0xffffff, 0x745774);
    _device.phy_set_bb_reg(0x0eb0, 0xffffff, 0x745774);
    _device.phy_set_bb_reg(0x0cb4, 0x0000ff00, 0x57);
    _device.phy_set_bb_reg(0x0eb4, 0x0000ff00, 0x57);
  } else {
    _device.phy_set_bb_reg(0x0cb0, 0xffffff, 0x477547);
    _device.phy_set_bb_reg(0x0eb0, 0xffffff, 0x477547);
    _device.phy_set_bb_reg(0x0cb4, 0x0000ff00, 0x75);
    _device.phy_set_bb_reg(0x0eb4, 0x0000ff00, 0x75);
  }
  _device.phy_set_bb_reg(0x0cbc, 0x3f, 0x0);
  _device.phy_set_bb_reg(0x0cbc, (1u << 11) | (1u << 10), 0x0);
  _device.phy_set_bb_reg(0x0ebc, 0x3f, 0x0);
  _device.phy_set_bb_reg(0x0ebc, (1u << 11) | (1u << 10), 0x0);
  /* antenna switch table: 2T2R => 2TX/2RX branch (rx_ant_status == BB_PATH_AB) */
  if (g2) {
    _device.phy_set_bb_reg(0x0ca0, 0x0000ffff, 0xa501);
    _device.phy_set_bb_reg(0x0ea0, 0x0000ffff, 0xa501);
  } else {
    _device.phy_set_bb_reg(0x0ca0, 0x0000ffff, 0xa5a5);
    _device.phy_set_bb_reg(0x0ea0, 0x0000ffff, 0xa5a5);
  }
}

void HalJaguar2::igi_toggle() {
  uint32_t igi = _device.rtw_read32(0x0c50) & 0x7f;
  _device.phy_set_bb_reg(0x0c50, 0x7f, igi - 2);
  _device.phy_set_bb_reg(0x0c50, 0x7f, igi);
  _device.phy_set_bb_reg(0x0e50, 0x7f, igi - 2);
  _device.phy_set_bb_reg(0x0e50, 0x7f, igi);
}

void HalJaguar2::kfree_init() {
  /* halrf_kfree.c port (8822B): phydm_get_power_trim_offset_8822b +
   * phydm_get_pa_bias_offset_8822b. All reads are PHYSICAL efuse bytes (the
   * PPG region above the logical map). */
  if (_variant == ChipVariant::C8821C)
    return; /* 8821C kfree variant not ported yet */
  auto rd = [this](uint16_t a) -> uint8_t {
    uint8_t d = 0xFF;
    return _device.efuse_OneByteRead(a, &d) ? d : 0xFF;
  };

  /* Power trim (per-band-group TX gain): 2G at 0x3EE (A low nibble / B high),
   * 5G groups at 0x3EC..0x3DB per path. 0xFF = unprogrammed. */
  const uint8_t pg2 = rd(0x3EE);
  if (pg2 != 0xFF) {
    _kfree_bb_gain[0][0] = pg2 & 0xF;
    _kfree_bb_gain[0][1] = (pg2 & 0xF0) >> 4;
    _kfree_2g = true;
  }
  const uint8_t g5l1a = rd(0x3EC);
  if (g5l1a != 0xFF) {
    static const uint16_t offs_a[5] = {0x3EC, 0x3E8, 0x3E4, 0x3E0, 0x3DC};
    static const uint16_t offs_b[5] = {0x3EB, 0x3E7, 0x3E3, 0x3DF, 0x3DB};
    for (int g = 0; g < 5; g++) {
      _kfree_bb_gain[1 + g][0] = rd(offs_a[g]);
      _kfree_bb_gain[1 + g][1] = rd(offs_b[g]);
    }
    _kfree_5g = true;
  }
  _logger->info("Jaguar2 kfree: power trim 2g={} 5g={} (0x3EE={:#04x})",
                _kfree_2g, _kfree_5g, pg2);

  /* PA bias (phydm_get_pa_bias_offset_8822b + set_pa_bias_to_rf): efuse
   * 0x3D5 (path A) / 0x3D6 (path B), sign-magnitude nibble, folded into the
   * RF 0x3f PA-bias word recomposed from RF 0x51/0x52 and written into RF
   * LUT entries 0/1/3 through the 0xef[10] window — WRITE-ONLY state (this
   * block is the 0x3f=0x4bb sequence in the kernel's usbmon golden init). */
  const uint8_t pgba = rd(0x3D5);
  if (pgba == 0xFF)
    return;
  for (uint8_t path = 0; path < (_ver.rf_2t2r ? 2u : 1u); path++) {
    uint8_t pg = rd(path == 0 ? 0x3D5 : 0x3D6) & 0xF;
    int8_t bias = (pg & 1) ? static_cast<int8_t>(pg >> 1)
                           : static_cast<int8_t>(-(pg >> 1));
    /* Close any LUT window the table apply left open — with 0xef nonzero the
     * 0x51/0x52 reads alias LUT content, not the PA config (read back wrong
     * as 0x1211 instead of the true 0x4bb until this was added). */
    rf_write(path, 0xef, 0x00000);
    const uint32_t r51 = rf_read(path, 0x51);
    const uint32_t r52 = rf_read(path, 0x52);
    uint32_t r3f = ((r52 & 0xe0000) >> 17) | (((r52 & 0x18000) >> 15) << 3) |
                   ((r52 & 0xf) << 5) | (((r51 & 0x78) >> 3) << 9) |
                   (((r52 & 0x2000) >> 13) << 13);
    int pa = static_cast<int8_t>((r3f & 0x1e00) >> 9) + bias;
    if (pa < 0)
      pa = 0;
    else if (pa > 7)
      pa = 7;
    r3f = (r3f & 0xfe1ff) | (static_cast<uint32_t>(pa) << 9);
    rf_set(path, 0xef, (1u << 10), 0x1);
    rf_write(path, 0x33, 0x0);
    rf_write(path, 0x3f, r3f);
    rf_set(path, 0x33, (1u << 0), 0x1);
    rf_write(path, 0x3f, r3f);
    rf_set(path, 0x33, (1u << 1), 0x1);
    rf_write(path, 0x3f, r3f);
    rf_set(path, 0x33, 0x3, 0x3);
    rf_write(path, 0x3f, r3f);
    rf_set(path, 0xef, (1u << 10), 0x0);
    _logger->info("Jaguar2 kfree: PA bias path {} = {} (r51={:#07x} "
                  "r52={:#07x} rf3f={:#07x})",
                  path, bias, r51, r52, r3f);
  }
}

void HalJaguar2::kfree_apply(uint8_t channel) {
  /* phydm_config_kfree / phydm_do_kfree (8822B): per-channel-group TX gain
   * trim via RF 0xde/0x65/0x55; clear-to-default when the efuse carries no
   * trim for the band. */
  if (_variant == ChipVariant::C8821C)
    return;
  const bool g2 = channel <= 14;
  int idx = -1;
  if (g2 && _kfree_2g) {
    idx = 0;
  } else if (!g2 && _kfree_5g) {
    if (channel >= 36 && channel <= 48)
      idx = 1;
    else if (channel >= 52 && channel <= 64)
      idx = 2;
    else if (channel >= 100 && channel <= 120)
      idx = 3;
    else if (channel >= 122 && channel <= 144)
      idx = 4;
    else if (channel >= 149)
      idx = 5;
  }
  for (uint8_t path = 0; path < (_ver.rf_2t2r ? 2u : 1u); path++) {
    const uint8_t data = idx >= 0 ? _kfree_bb_gain[idx][path] : 0;
    /* set (phydm_set_kfree_to_rf_8822b) — the clear variant appends the
     * de-select tail. */
    rf_set(path, 0xde, (1u << 0), 1);
    rf_set(path, 0xde, (1u << 4), 1);
    rf_set(path, 0x65, 0xffff, 0x9000);
    rf_set(path, 0x55, (1u << 5), 1);
    rf_set(path, 0x55, (1u << 19), data & 1);
    rf_set(path, 0x55, 0x7c000, (data & 0x1f) >> 1);
    if (idx < 0) { /* phydm_clear_kfree_to_rf_8822b tail */
      rf_set(path, 0xde, (1u << 0), 0);
      rf_set(path, 0xde, (1u << 4), 1);
      rf_set(path, 0x65, 0xffff, 0x9000);
      rf_set(path, 0x55, (1u << 5), 0);
      rf_set(path, 0x55, (1u << 7), 0);
    }
  }
}

void HalJaguar2::spur_calibration(uint8_t channel, uint8_t cch, uint8_t bw) {
  /* phydm_spur_calibration_8822b + phydm_dynamic_spur_det_eliminate port
   * (8822B only; CONFIG_8822B_SPUR_CALIBRATION is on in the CE vendor
   * build). The chip's own clock spurs land inside a fixed set of channels;
   * on those the vendor PSD-scans the spur frequency and, above threshold,
   * programs the NBI notch + CSI mask. On every other channel this reduces
   * to the reset half (clear masks + notch disables) — still vendor parity
   * on every channel set. */
  if (_variant == ChipVariant::C8821C)
    return;
  const bool r2t2r = _ver.rf_2t2r != 0;

  /* Entry disables + dsde_init: clear the CSI mask bank and both notch
   * enables. */
  _device.phy_set_bb_reg(0x087c, (1u << 13), 0x0);
  _device.phy_set_bb_reg(0x0c20, (1u << 28), 0x0);
  _device.phy_set_bb_reg(0x0e20, (1u << 28), 0x0);
  for (uint16_t a = 0x880; a <= 0x89c; a += 4)
    _device.rtw_write32(a, 0);
  _device.phy_set_bb_reg(0x0874, 0x1, 0x0);

  /* phydm_dsde_ch_idx: the spur-channel table. idx 16 = no spur here. */
  const bool g2 = channel <= 14;
  uint8_t idx = 16;
  if (g2) {
    if (bw == 0) { /* 20M: ch 5..8 -> 0..3, ch 13 -> 4 */
      if (channel >= 5 && channel <= 8)
        idx = static_cast<uint8_t>(channel - 5);
      else if (channel == 13)
        idx = 4;
    } else if (bw == 1) { /* 40M: ch 3..11 -> 5..13 */
      if (channel >= 3 && channel <= 11)
        idx = static_cast<uint8_t>(channel + 2);
    }
  } else {
    switch (channel) {
    case 153: idx = 0; break;
    case 161: idx = 1; break;
    case 54:  idx = 2; break;
    case 118: idx = 3; break;
    case 151: idx = 4; break;
    case 159: idx = 5; break;
    case 58:  idx = 6; break;
    case 122: idx = 7; break;
    case 155: idx = 8; break;
    default: break;
    }
  }
  if (idx > 13)
    return;

  /* PSD scan at the spur frequency point ±1 (3 samples each): 3-wire off,
   * one RX path at a time, PSD engine via 0x910 BIT(22)|freq_pt, report at
   * 0xf44[15:0]. Restores 3-wire / 0x910[15:12] / RX paths and toggles IGI
   * so the RF re-enters RX (the BB sends no 3-wire on path re-enable). */
  static const uint32_t freq_2g[14] = {0xFC67, 0xFC27, 0xFFE6, 0xFFA6, 0xFC67,
                                       0xFCE7, 0xFCA7, 0xFC67, 0xFC27, 0xFFE6,
                                       0xFFA6, 0xFF66, 0xFF26, 0xFCE7};
  static const uint32_t freq_5g[10] = {0xFFC0, 0xFFC0, 0xFC81, 0xFC81, 0xFC41,
                                       0xFC40, 0xFF80, 0xFF80, 0xFF40, 0xFD42};
  if (!g2 && idx >= 10)
    return;
  const uint32_t f_base = g2 ? freq_2g[idx] : freq_5g[idx];
  uint32_t max_a = 0, max_b = 0;
  for (int k = 0; k < 3; k++) {
    const uint32_t f_pt = f_base - 1 + static_cast<uint32_t>(k);
    for (int j = 0; j < 3; j++) {
      _device.phy_set_bb_reg(0x0c00, 0xff, 0x4); /* 3-wire off */
      _device.phy_set_bb_reg(0x0e00, 0xff, 0x4);
      const uint32_t r910_15_12 =
          (_device.rtw_read32(0x0910) >> 12) & 0xf;
      /* path A */
      _device.phy_set_bb_reg(0x0808, 0xff, 0x11);
      _device.phy_set_bb_reg(0x0910, 0xffffffff, (1u << 22) | f_pt);
      std::this_thread::sleep_for(std::chrono::microseconds(500));
      uint32_t v = _device.rtw_read32(0x0f44) & 0xffff;
      if (v > max_a)
        max_a = v;
      _device.phy_set_bb_reg(0x0910, (1u << 22), 0x0);
      if (r2t2r) { /* path B: freq_pt | BIT(16) */
        _device.phy_set_bb_reg(0x0808, 0xff, 0x22);
        _device.phy_set_bb_reg(0x0910, 0xffffffff,
                               (1u << 22) | (1u << 16) | f_pt);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        v = _device.rtw_read32(0x0f44) & 0xffff;
        if (v > max_b)
          max_b = v;
        _device.phy_set_bb_reg(0x0910, (1u << 22), 0x0);
      }
      _device.phy_set_bb_reg(0x0c00, 0xff, 0x7); /* 3-wire on */
      _device.phy_set_bb_reg(0x0e00, 0xff, 0x7);
      _device.phy_set_bb_reg(0x0910, 0xf000, r910_15_12);
      const uint8_t rx_ant = r2t2r ? 0x3 : 0x1;
      _device.phy_set_bb_reg(0x0808, 0xff,
                             static_cast<uint32_t>(rx_ant << 4) | rx_ant);
      igi_toggle();
    }
  }

  constexpr uint32_t kThreshold = 0x8D; /* NBI and CSI share it */
  const bool do_nbi = max_a >= kThreshold || max_b >= kThreshold;
  if (!do_nbi) {
    _logger->info("Jaguar2 spur-cal: ch{} bw{} psd A={} B={} — below "
                  "threshold, no notch",
                  channel, bw, max_a, max_b);
    return;
  }

  /* phydm_dsde_nbi: CCA tweak + notch at the spur frequency (MHz). The spur
   * frequency per channel/BW, from the vendor tables. */
  _device.phy_set_bb_reg(0x082c, 0xff000, 0x97);
  _device.phy_set_bb_reg(0x082c, 0xf000, 0x7);
  uint32_t f_int = 0;
  const uint32_t bw_mhz = bw == 1 ? 40 : bw == 2 ? 80 : 20;
  if (bw == 0) {
    if (channel == 153) f_int = 5760;
    else if (channel == 161) f_int = 5800;
    else if (channel >= 5 && channel <= 8) f_int = 2440;
    else if (channel == 13) f_int = 2480;
  } else if (bw == 1) {
    if (channel == 54) f_int = 5280;
    else if (channel == 118) f_int = 5600;
    else if (channel == 151) f_int = 5760;
    else if (channel == 159) f_int = 5800;
    else if (channel >= 4 && channel <= 6) f_int = 2440;
    else if (channel == 11) f_int = 2480;
  } else if (bw == 2) {
    if (channel == 58) f_int = 5280;
    else if (channel == 122) f_int = 5600;
    else if (channel == 155) f_int = 5760;
  }
  if (f_int == 0)
    return;
  const uint32_t fc = devourer::chan_to_freq(cch);
  devourer::tonemask::nbi_apply_11ac(_device,
                                     devourer::tonemask::Family::AC2_8822B, fc,
                                     bw_mhz, f_int, r2t2r);
  /* phydm_dsde_csi: mask the tones around the spur (±600 kHz ~ the vendor's
   * tone+neighbor weights). */
  devourer::tonemask::CsiMaskSpec spec{};
  spec.valid = true;
  spec.f_lo_khz = static_cast<int32_t>(f_int) * 1000 - 600;
  spec.f_hi_khz = static_cast<int32_t>(f_int) * 1000 + 600;
  devourer::tonemask::csi_mask_apply_11ac(_device, fc, bw_mhz, spec);
  _logger->info("Jaguar2 spur-cal: ch{} bw{} psd A={} B={} — NBI+CSI notch "
                "at {} MHz",
                channel, bw, max_a, max_b, f_int);
}

void HalJaguar2::bb_reset() {
  /* MAC 0x0 BIT16 (SYS_FUNC_EN FEN_BBRSTB) 0->1 — the _iqk_bb_reset_8822b /
   * _8821c mechanism; relatches the BB DAC/DFE clock tree. */
  uint32_t r0 = _device.rtw_read32(0x0);
  _device.rtw_write32(0x0, r0 & ~(1u << 16));
  _device.rtw_write32(0x0, r0 | (1u << 16));
}

/* Central channel of the wide channel; `channel` is the primary 20 MHz channel
 * and primary_ch_idx its position. 20 MHz: central = primary. 40 MHz:
 * primary_ch_idx 1(lower)/2(upper) -> ±2. 80 MHz: 1..4 -> +6/+2/-2/-6. (The
 * vendor's higher layer passes the central channel to
 * config_phydm_switch_channel_* and primary_ch_idx to switch_bandwidth.) */
uint8_t HalJaguar2::central_ch(uint8_t channel, uint8_t bw,
                               uint8_t primary_ch_idx) {
  if (bw == 1)
    return static_cast<uint8_t>(primary_ch_idx == 2 ? channel - 2
                                                    : channel + 2);
  if (bw == 2) {
    static const int off80[5] = {0, 6, 2, -2, -6};
    return static_cast<uint8_t>(
        channel + off80[primary_ch_idx <= 4 ? primary_ch_idx : 0]);
  }
  return channel;
}

void HalJaguar2::set_channel_bw(uint8_t channel, uint8_t bw, uint8_t rfe_type,
                               uint8_t primary_ch_idx) {
  /* Any full channel set rewrites what fast_retune caches (RF18 band/BW bits,
   * AGC/fc buckets, RF 0xBE, spur/CCK-filter regs) — invalidate; the caches
   * only ever mirror fast-path writes. */
  _cw_primed = false;
  _last_agc_bucket = -1;
  _last_fc = 0xffffffff;
  _last_rf_be = -1;
  _last_df18 = -1;
  _last_cck_key = -1;

  /* Extended-synth channels (below-band 15..35 / above 177, freq =
   * 5000 + 5*ch up to ch 253): the RF tunes them, but the power tables and
   * per-channel constants are clamped from the nearest characterized
   * channel. Warn once so an unexpected retune there is visible. */
  if (!_warned_uncharacterized &&
      ((channel >= 15 && channel <= 35) || channel > 177)) {
    _warned_uncharacterized = true;
    _logger->warn("channel {} is outside the characterized range — TX power "
                  "and per-channel constants extrapolated from the nearest "
                  "table entry",
                  channel);
  }

  if (_variant == ChipVariant::C8821C) {
    set_channel_bw_8821c(channel, bw, rfe_type, primary_ch_idx);
    _last_tuned_ch = channel;
    if (_cfg.debug.dump_canary)
      DumpCanary();
    return;
  }
  const uint8_t cch = central_ch(channel, bw, primary_ch_idx);
  const bool g2 = cch <= 14;
  const bool r2t2r = _ver.rf_2t2r != 0;

  /* --- config_phydm_switch_channel_8822b (on the central channel) --- */
  rfe_ifem(cch); /* RFE pins for the band (rfe_type 0) */
  (void)rfe_type;

  uint32_t rf18 = rf_read(0, 0x18);
  rf18 &= ~((1u << 18) | (1u << 17) | 0xffu); /* clear band/MASKBYTE0 */
  rf18 |= cch;

  if (g2) {
    _device.phy_set_bb_reg(0x0958, 0x1f, 0x0);       /* AGC table idx 0 */
    _device.phy_set_bb_reg(0x0860, 0x1ffe0000, 0x96a); /* fc for CFO tracking */
    if (cch == 14) {
      _device.phy_set_bb_reg(0x0a24, 0xffffffff, 0x00006577);
      _device.phy_set_bb_reg(0x0a28, 0x0000ffff, 0x0000);
    } else {
      _device.phy_set_bb_reg(0x0a24, 0xffffffff, 0x384f6577);
      _device.phy_set_bb_reg(0x0a28, 0x0000ffff, 0x1525);
    }
  } else {
    /* 5G AGC table + fc (config_phydm_switch_channel_8822b 5G branch).
     * Extended-range clamps: below-band ch 15..35 rides the low bucket
     * (mirrors Jaguar1's explicit 15..35 case) and ch >177 keeps the top
     * bucket — the vendor tables end at 177 but the synth tunes past it. */
    if (cch <= 64)
      _device.phy_set_bb_reg(0x0958, 0x1f, 0x1);
    else if (cch >= 100 && cch <= 144)
      _device.phy_set_bb_reg(0x0958, 0x1f, 0x2);
    else if (cch >= 149)
      _device.phy_set_bb_reg(0x0958, 0x1f, 0x3);
    if (cch <= 48)
      _device.phy_set_bb_reg(0x0860, 0x1ffe0000, 0x494);
    else if (cch >= 52 && cch <= 64)
      _device.phy_set_bb_reg(0x0860, 0x1ffe0000, 0x453);
    else if (cch >= 100 && cch <= 116)
      _device.phy_set_bb_reg(0x0860, 0x1ffe0000, 0x452);
    else if (cch >= 118)
      _device.phy_set_bb_reg(0x0860, 0x1ffe0000, 0x412);
  }

  /* config_phydm_switch_band_8822b RxHP-with-SoML block (SoML is enabled at
   * config_trx_mode): 5G SoML-on -> 0x8cc=0x08108000, 0x8d8[27]=0,
   * 0xc04/0xe04[21|18]=0. 2.4G SoML-on with rfe_type 3/5/8/17 keeps the
   * 0x08108492 defaults (vendor branch), other RFEs take the same values as
   * 5G. Kernel-parity state read back from the working driver at ch44. */
  _device.phy_set_bb_reg(0x0c04, (1u << 21) | (1u << 18), 0x0);
  _device.phy_set_bb_reg(0x0e04, (1u << 21) | (1u << 18), 0x0);
  if (!g2 || !(rfe_type == 3 || rfe_type == 5 || rfe_type == 8 ||
               rfe_type == 17)) {
    _device.phy_set_bb_reg(0x08cc, 0xffffffff, 0x08108000);
    _device.phy_set_bb_reg(0x08d8, (1u << 27), 0x0);
  } else {
    _device.phy_set_bb_reg(0x08cc, 0xffffffff, 0x08108492);
    _device.phy_set_bb_reg(0x08d8, (1u << 27), 0x1);
  }

  /* RF 0xBE[17:15] per-channel phase-noise / VCO-band setting (shared with
   * fast_retune). Omitting it (writing 0 on 5G) leaves the 5G VCO mis-tuned so
   * 5G RX/TX fail entirely — this was the sole gap blocking 5G. Path A only
   * (matches the vendor). */
  const uint8_t rf_be = rf_be_for_8822b(cch);
  if (rf_be != 0xff)
    rf_set(0, 0xbe, (1u << 17) | (1u << 16) | (1u << 15), rf_be);

  if (cch == 144) {
    rf_set(0, 0xdf, (1u << 18), 0x1);
    rf18 |= (1u << 17);
  } else {
    rf_set(0, 0xdf, (1u << 18), 0x0);
    if (cch > 144)
      rf18 |= (1u << 18);
    else if (cch >= 80)
      rf18 |= (1u << 17);
  }

  /* --- config_phydm_switch_bandwidth_8822b (20/40/80 + 5/10 MHz) --- */
  uint32_t v8ac = _device.rtw_read32(0x08ac);
  const uint8_t sub = static_cast<uint8_t>((primary_ch_idx & 0xf) << 2);
  if (bw == 1) { /* 40 MHz */
    _device.phy_set_bb_reg(0x0a00, 1u << 4, primary_ch_idx == 1 ? 1 : 0);
    v8ac &= 0xFF3FF300;
    v8ac |= (sub | 0x1u); /* CHANNEL_WIDTH_40 = 1 */
    _device.phy_set_bb_reg(0x08ac, 0xffffffff, v8ac);
    _device.phy_set_bb_reg(0x08c4, (1u << 30), 0x1);
    rf18 &= ~((1u << 11) | (1u << 10));
    rf18 |= (1u << 11); /* RF BW 40M */
  } else if (bw == 2) { /* 80 MHz */
    v8ac &= 0xFCEFCF00;
    v8ac |= (sub | 0x2u); /* CHANNEL_WIDTH_80 = 2 */
    _device.phy_set_bb_reg(0x08ac, 0xffffffff, v8ac);
    _device.phy_set_bb_reg(0x08c4, (1u << 30), 0x1);
    rf18 &= ~((1u << 11) | (1u << 10));
    rf18 |= (1u << 10); /* RF BW 80M */
    if (rfe_type == 2 || rfe_type == 3 || rfe_type == 17) {
      _device.phy_set_bb_reg(0x0840, 0x0000f000, 0x6);
      _device.phy_set_bb_reg(0x08c8, (1u << 10), 0x1);
    }
  } else if (bw == 5 || bw == 6) {
    /* CHANNEL_WIDTH_5 / _10 — narrowband baseband re-clock: small-BW
     * 0x8ac[7:6] = 1/2, rf mode [1:0] = 20M, ADC clock [9:8]+[16], DAC clock
     * [21:20]+[28]; the RF synth stays in its 20 MHz mode (vendor
     * config_phydm_switch_bandwidth_8822b CHANNEL_WIDTH_5/10 — verbatim,
     * incl. its masks keeping the ADC/DAC fields from the previous BW set).
     *
     * The 8822B additionally requires an RF18 RE-LATCH EDGE after the
     * re-clock — see the comment at the RF18 write below. (Found by
     * golden-init replay: usbmon-capture the working kernel driver's full
     * post-DLFW write stream, replay it verbatim from devourer via
     * DEVOURER_REPLAY_WSEQ, delta-debug the 3630-write stream down to the
     * causal pair. Every readable-register hypothesis had been eliminated
     * first by A/B against reference/rtl88x2bu/88x2bu_ohd.ko: kernel NB
     * survives devourer's entire differing MAC+BB+RF state and the removal
     * of its only H2C traffic.) */
    const bool is5 = (bw == 5);
    v8ac &= is5 ? 0xEFEEFE00 : 0xEFFEFF00;
    v8ac |= is5 ? (1u << 6) : (1u << 7);
    /* DEVOURER_NB_ADC / DEVOURER_NB_DAC — divider-mapping experiment knobs:
     * force the ADC ([9:8] + bit16) / DAC ([21:20] + bit28) clock fields the
     * vendor comments describe but its code never writes. */
    if (_cfg.tuning.nb_adc) {
      v8ac &= ~((0x3u << 8) | (1u << 16));
      v8ac |= (static_cast<uint32_t>(*_cfg.tuning.nb_adc & 0x3) << 8) |
              ((*_cfg.tuning.nb_adc & 0x4) ? (1u << 16) : 0u);
      _logger->info("Jaguar2: tuning.nb_adc override — ADC code {:#x}",
                    *_cfg.tuning.nb_adc);
    }
    if (_cfg.tuning.nb_dac) {
      v8ac &= ~((0x3u << 20) | (1u << 28));
      v8ac |= (static_cast<uint32_t>(*_cfg.tuning.nb_dac & 0x3) << 20) |
              ((*_cfg.tuning.nb_dac & 0x4) ? (1u << 28) : 0u);
      _logger->info("Jaguar2: tuning.nb_dac override — DAC code {:#x}",
                    *_cfg.tuning.nb_dac);
    }
    _device.phy_set_bb_reg(0x08ac, 0xffffffff, v8ac);
    _device.phy_set_bb_reg(0x08c4, (1u << 30), 0x0); /* ADC buffer clock */
    _device.phy_set_bb_reg(0x08c8, (1u << 31), 0x1);
    rf18 |= (1u << 11) | (1u << 10); /* RF stays 20M */
    /* config_phydm_switch_band_8822b CCK-block trio (the 8821C path has it in
     * its band block; the 8822B port never carried it). At 5G the CCK block
     * must be OFF — the table-apply POST bracket leaves it on, harmless at
     * 20 MHz but under the NB re-clock the mis-clocked CCK engine breaks the
     * demod. NB-gated pending 20/40/80 regression of the full band block. */
    if (g2) {
      _device.phy_set_bb_reg(0x0808, (1u << 28), 0x1); /* CCK block on */
      _device.phy_set_bb_reg(0x0454, (1u << 7), 0x0);  /* MAC CCK check off */
      _device.phy_set_bb_reg(0x0a80, (1u << 18), 0x0); /* BB CCK check off */
    } else {
      _device.phy_set_bb_reg(0x0a80, (1u << 18), 0x1);
      _device.phy_set_bb_reg(0x0454, (1u << 7), 0x1);
      _device.phy_set_bb_reg(0x0808, (1u << 28), 0x0); /* CCK block off */
      _device.phy_set_bb_reg(0x0814, 0x0000FC00, 34);  /* CCA mask 13.6us */
    }
  } else { /* 20 MHz */
    v8ac &= 0xFFCFFC00;
    v8ac |= 0x0u; /* CHANNEL_WIDTH_20 = 0 */
    _device.phy_set_bb_reg(0x08ac, 0xffffffff, v8ac);
    _device.phy_set_bb_reg(0x08c4, (1u << 30), 0x1);
    rf18 |= (1u << 11) | (1u << 10); /* RF BW 20M */
  }

  /* RF18 final value. The vendor (config_phydm_switch_channel_8822b) preserves
   * bits 8-16 from the read and never masks — its RF18 comes out clean on 2.4G
   * (0x0c09) and with the 5G band bits (8+16) set on 5G (0x10d24). devourer's
   * direct RF18 read carries SPURIOUS bits 8/16 on 2.4G (an upstream RF-state
   * quirk), so on 2.4G we clear them to reproduce the vendor's clean value; on
   * 5G those bits are the real band indicator and must be kept (masking them was
   * what left the 5G synth in 2.4G mode -> dead 5G RX). Net: devourer's RF18 now
   * matches the vendor's per-band value. */
  if (g2)
    rf18 &= 0x00060cffu;

  /* 5/10 MHz: the RF synth only re-latches its internal channel state on an
   * RF18 VALUE EDGE — a same-value rewrite does nothing, and after the
   * baseband ADC/DAC re-clock the RF stays latched to pre-re-clock internals
   * (RX deaf, TX invalid). The kernel never hits this because its flow always
   * tunes through a channel/band change around a bandwidth switch; devourer's
   * end-of-bring-up narrowband re-clock rewrites RF18 with the identical
   * value. Force the edge: intermediate write with a different channel byte,
   * then the real value. Isolated by golden-init replay bisection down to
   * exactly this write pair (23300 vs 9 NB frames on the bench). */
  if (bw == 5 || bw == 6) {
    const uint32_t rf18_alt = (rf18 & ~0xffu) | (cch == 1 ? 2u : 1u);
    rf_write(0, 0x18, rf18_alt);
    if (r2t2r)
      rf_write(1, 0x18, rf18_alt);
  }

  rf_write(0, 0x18, rf18);
  if (r2t2r)
    rf_write(1, 0x18, rf18);

  /* RF read-error debug toggle (RF_A 0xb8[19] 0->1) */
  rf_set(0, 0xb8, (1u << 19), 0);
  rf_set(0, 0xb8, (1u << 19), 1);

  /* --- phydm_rxdfirpar_by_bw_8822b (RX digital filter, BW20 path) ---
   * The RX DFIR must match the bandwidth or the OFDM demod filter is wrong:
   * energy is detected (CCA/FA fire) but no PSDU ever completes and nothing
   * reaches the MAC RX FIFO. This was the missing piece for first RX. */
  _device.phy_set_bb_reg(0x0948, (1u << 29) | (1u << 28), 0x2);
  _device.phy_set_bb_reg(0x094c, (1u << 29) | (1u << 28), 0x2);
  _device.phy_set_bb_reg(0x0c20, (1u << 31), 0x1);
  _device.phy_set_bb_reg(0x0e20, (1u << 31), 0x1);

  /* --- phydm_ccapar_by_rfe_8822b (CCA thresholds) --- col: 2G/5G x 1R/2R.
   * RFE types 3/5/12/15/16/17/19 (external-FEM boards, incl. the T3U's
   * rfe_type 3) take the cca_ifem_ccut_rfe table — devourer previously
   * hardcoded the plain iFEM C-cut column for every board, leaving eFEM
   * boards with mis-set carrier-sense thresholds (kernel-parity write-
   * sequence diff). */
  {
    const int col = g2 ? (r2t2r ? 1 : 0) : (r2t2r ? 3 : 2);
    static const uint32_t cca_ifem[3][4] = {
        {0x75C97010, 0x75C97010, 0x75C97010, 0x75C97010}, /* 0x82c */
        {0x79a0eaaa, 0x79A0EAAC, 0x79a0eaaa, 0x79a0eaaa}, /* 0x830 */
        {0x87765541, 0x87746341, 0x87765541, 0x87746341}, /* 0x838 */
    };
    static const uint32_t cca_ifem_rfe[3][4] = {
        {0x75da8010, 0x75da8010, 0x75da8010, 0x75da8010}, /* 0x82c */
        {0x79a0eaaa, 0x97A0EAAC, 0x79a0eaaa, 0x79a0eaaa}, /* 0x830 */
        {0x87765541, 0x86666341, 0x87765561, 0x86666361}, /* 0x838 */
    };
    const bool rfe_tab = rfe_type == 3 || rfe_type == 5 || rfe_type == 12 ||
                         rfe_type == 15 || rfe_type == 16 || rfe_type == 17 ||
                         rfe_type == 19;
    const auto &cca = rfe_tab ? cca_ifem_rfe : cca_ifem;
    _device.phy_set_bb_reg(0x082c, 0xffffffff, cca[0][col]);
    _device.phy_set_bb_reg(0x0830, 0xffffffff, cca[1][col]);
    _device.phy_set_bb_reg(0x0838, 0xffffffff, cca[2][col]);
    /* 20 MHz DFS-channel CCA adjust (vendor tail). */
    if (bw == 0 && !g2 && ((cch >= 52 && cch <= 64) || (cch >= 100 && cch <= 144)))
      _device.phy_set_bb_reg(0x0838, 0xf0, 0x5);
  }

  /* RX-path toggle (0x808 MASKBYTE0) to leave RX dead-zone, then IGI. rx_ant
   * = A+B (0x3) for 2T2R => 0x33 (CCK+OFDM both paths). */
  uint8_t rx_ant = r2t2r ? 0x3 : 0x1;
  _device.phy_set_bb_reg(0x0808, 0xff, 0x0);
  _device.phy_set_bb_reg(0x0808, 0xff, rx_ant | (rx_ant << 4));
  igi_toggle();

  /* phydm_spur_calibration_8822b — reset the NBI/CSI notch state, and on the
   * chip's own spur channels PSD-detect + notch (vendor switch_channel tail). */
  spur_calibration(channel, cch, bw);

  /* 5/10 MHz: BB reset (MAC 0x0 BIT16 toggle — the same _iqk_bb_reset_8822b
   * mechanism) so the DAC/DFE relatch at the new sample rate. Hardware-taught
   * on Jaguar3 (RadioManagementJaguar3::set_bandwidth_dividers): without it
   * the re-clock doesn't take. */
  if (bw == 5 || bw == 6)
    bb_reset();

  /* phydm_config_kfree — per-channel-group efuse TX gain trim, applied by the
   * vendor after every channel/BW set. Group-keyed, so FastRetune (intra-band
   * hop) rides the last full set's trim. */
  kfree_apply(channel);

  _last_tuned_ch = channel;
  /* Cache the channel-keyed state a fast_set_bandwidth toggle replays. rf18 is
   * the 20 MHz-mode value (narrowband keeps it), cch/g2/r2t2r drive the RF18
   * edge + CCK trio. Capture the 20 MHz-state 0x8ac only on a 20 MHz set — the
   * base the narrowband/20 compose masks apply to. */
  _fbw_rf18 = rf18;
  _fbw_cch = cch;
  _fbw_g2 = g2;
  _fbw_r2t2r = r2t2r;
  _fbw_cur_bw = bw;
  _fbw_cache_valid = true;
  if (bw == 0) {
    _fbw_8ac_20m = v8ac;
    _fbw_8ac_valid = true;
  }
  _logger->info("Jaguar2: channel set ch={} bw={} (rf18=0x{:05x})", channel,
                (int)bw, rf18);
  if (_cfg.debug.dump_canary)
    DumpCanary();
}

void HalJaguar2::DumpCanary() {
  /* Channel/BW-relevant set (both variants; the control run in the parity
   * script classifies natural run-variance): RX path (0x808), CCA thresholds
   * (0x82c/0x830/0x838), fc (0x860), BW block (0x8ac/0x8c4/0x8c8/0x8f0 —
   * 0x8c8[31] is narrowband ADC-buffer state, set on 5/10 MHz), RX DFIR
   * (0x948/0x94c/0xc20/0xe20), AGC index (0x958 8822B / 0xc1c 8821C), CCK pri
   * (0xa00), spur / CCK filter (0xa24/0xa28/0xaac), 8821C band block
   * (0xa80/0xa84/0x814), RFE pins (0xcb0/0xcb4/0xcb8/0xca0/0xeb0/0xeb4/0xea0).
   * MAC: CCK check (0x454). RF via the direct read window: 0x18 channel,
   * 0xbe VCO band, 0xdf, 0xb8. IGI (0xc50/0xe50) excluded — live. */
  static const uint16_t bb_canary[] = {
      0x808, 0x814, 0x82c, 0x830, 0x838, 0x860, 0x8ac, 0x8c4, 0x8c8, 0x8f0,
      0x948, 0x94c, 0x958, 0xa00, 0xa24, 0xa28, 0xaac, 0xa80, 0xa84,
      0xc1c, 0xc20, 0xe20, 0xca0, 0xcb0, 0xcb4, 0xcb8, 0xea0, 0xeb0, 0xeb4};
  static const uint16_t mac_canary[] = {0x454};
  static const uint8_t rf_canary[] = {0x18, 0xb8, 0xbe, 0xdf};
  const bool r2t2r = _ver.rf_2t2r != 0;

  _logger->info("=== DEVOURER_DUMP_CANARY (post channel-set ch={}) ===",
                unsigned(_last_tuned_ch));
  for (uint16_t a : bb_canary)
    _logger->info("BB 0x{:03x} = 0x{:08X}", a, _device.rtw_read32(a));
  for (uint16_t a : mac_canary)
    _logger->info("MAC 0x{:03x} = 0x{:08X}", a, _device.rtw_read32(a));
  for (uint8_t a : rf_canary)
    _logger->info("RF[A] 0x{:02x} = 0x{:05X}", a, rf_read(0, a));
  if (r2t2r)
    for (uint8_t a : rf_canary)
      _logger->info("RF[B] 0x{:02x} = 0x{:05X}", a, rf_read(1, a));
  _logger->info("=== END DEVOURER_DUMP_CANARY ===");
}

/* RF 0xBE[17:15] per-5G-channel phase-noise / VCO-band value
 * (config_phydm_switch_channel_8822b low/middle/high-band tables). */
uint8_t HalJaguar2::rf_be_for_8822b(uint8_t cch) {
  static const uint8_t low_band[15] = {0x7, 0x6, 0x6, 0x5, 0x0, 0x0, 0x7, 0xff,
                                       0x6, 0x5, 0x0, 0x0, 0x7, 0x6, 0x6};
  static const uint8_t middle_band[23] = {
      0x6, 0x5, 0x0, 0x0, 0x7, 0x6, 0x6, 0xff, 0x0, 0x0, 0x7, 0x6,
      0x6, 0x5, 0x0, 0xff, 0x7, 0x6, 0x6, 0x5, 0x0, 0x0, 0x7};
  static const uint8_t high_band[15] = {0x5, 0x5, 0x0, 0x7, 0x7, 0x6, 0x5, 0xff,
                                        0x0, 0x7, 0x7, 0x6, 0x5, 0x5, 0x0};
  if (cch <= 14)
    return 0x0;
  /* Extended-range clamps: the vendor tables cover 36..64 / 100..144 /
   * 149..177; the synth tunes below 36 and above 177 (chan*5+5000 grid up to
   * 253). Clamp to the nearest table edge so the VCO band is still set —
   * extrapolated, not characterized. */
  if (cch < 36)
    return low_band[0];
  if (cch <= 64)
    return low_band[(cch - 36) >> 1];
  if (cch >= 100 && cch <= 144)
    return middle_band[(cch - 100) >> 1];
  if (cch >= 149 && cch <= 177)
    return high_band[(cch - 149) >> 1];
  if (cch > 177)
    return high_band[(177 - 149) >> 1];
  return 0xff;
}

bool HalJaguar2::fast_set_bandwidth(uint8_t bw) {
  /* 8822B only for now (the 8821C narrowband path differs — future work). */
  if (_variant != ChipVariant::C8822B)
    return false;
  auto in_set = [](uint8_t b) { return b == 0 || b == 5 || b == 6; };
  if (!in_set(_fbw_cur_bw) || !in_set(bw))
    return false;
  if (bw == _fbw_cur_bw)
    return true;
  if (!_fbw_cache_valid || !_fbw_8ac_valid)
    return false; /* need a full set (incl. a 20 MHz one) to prime the cache */

  devourer::HopProf prof(_logger->events(), _cfg.debug.hop_prof, "j2bw",
                         _last_tuned_ch);
  const bool g2 = _fbw_g2;
  const bool r2t2r = _fbw_r2t2r;

  if (bw == 5 || bw == 6) {
    /* 20 -> narrowband: the exact CHANNEL_WIDTH_5/10 delta from
     * set_channel_bw, composed onto the cached 20 MHz 0x8ac. */
    const bool is5 = (bw == 5);
    uint32_t v8ac = _fbw_8ac_20m;
    v8ac &= is5 ? 0xEFEEFE00u : 0xEFFEFF00u;
    v8ac |= is5 ? (1u << 6) : (1u << 7);
    if (_cfg.tuning.nb_adc) {
      v8ac &= ~((0x3u << 8) | (1u << 16));
      v8ac |= (static_cast<uint32_t>(*_cfg.tuning.nb_adc & 0x3) << 8) |
              ((*_cfg.tuning.nb_adc & 0x4) ? (1u << 16) : 0u);
    }
    if (_cfg.tuning.nb_dac) {
      v8ac &= ~((0x3u << 20) | (1u << 28));
      v8ac |= (static_cast<uint32_t>(*_cfg.tuning.nb_dac & 0x3) << 20) |
              ((*_cfg.tuning.nb_dac & 0x4) ? (1u << 28) : 0u);
    }
    _device.phy_set_bb_reg(0x08ac, 0xffffffff, v8ac);
    _device.phy_set_bb_reg(0x08c4, (1u << 30), 0x0);
    _device.phy_set_bb_reg(0x08c8, (1u << 31), 0x1);
    /* CCK trio (band-keyed, exactly as the full narrowband branch). */
    if (g2) {
      _device.phy_set_bb_reg(0x0808, (1u << 28), 0x1);
      _device.phy_set_bb_reg(0x0454, (1u << 7), 0x0);
      _device.phy_set_bb_reg(0x0a80, (1u << 18), 0x0);
    } else {
      _device.phy_set_bb_reg(0x0a80, (1u << 18), 0x1);
      _device.phy_set_bb_reg(0x0454, (1u << 7), 0x1);
      _device.phy_set_bb_reg(0x0808, (1u << 28), 0x0);
      _device.phy_set_bb_reg(0x0814, 0x0000FC00, 34);
    }
    /* RF18 re-latch edge (the 8822B narrowband requirement) then the real
     * value — both write-only from the cached rf18, no RF read. */
    const uint32_t rf18 = _fbw_rf18;
    const uint32_t rf18_alt = (rf18 & ~0xffu) | (_fbw_cch == 1 ? 2u : 1u);
    rf_write(0, 0x18, rf18_alt);
    if (r2t2r)
      rf_write(1, 0x18, rf18_alt);
    rf_write(0, 0x18, rf18);
    if (r2t2r)
      rf_write(1, 0x18, rf18);
    rf_set(0, 0xb8, (1u << 19), 0);
    rf_set(0, 0xb8, (1u << 19), 1);
    bb_reset(); /* relatch the DAC/DFE at the new sample rate */
  } else {
    /* narrowband -> 20 MHz: restore the cached 20 MHz 0x8ac + ADC buffer clock
     * (exactly the CHANNEL_WIDTH_20 branch; no edge / no bb_reset, matching the
     * full path). */
    _device.phy_set_bb_reg(0x08ac, 0xffffffff, _fbw_8ac_20m);
    _device.phy_set_bb_reg(0x08c4, (1u << 30), 0x1);
    const uint32_t rf18 = _fbw_rf18;
    rf_write(0, 0x18, rf18);
    if (r2t2r)
      rf_write(1, 0x18, rf18);
  }
  _fbw_cur_bw = bw;
  prof.mark("bb_reclock");
  _logger->info("Jaguar2: fast_bw {} MHz", bw == 5 ? 5 : bw == 6 ? 10 : 20);
  if (_cfg.debug.dump_canary)
    DumpCanary();
  return true;
}

bool HalJaguar2::fast_retune(uint8_t channel, uint8_t bw,
                             uint8_t primary_ch_idx, bool cache_rf) {
  if (_last_tuned_ch == 0)
    return false; /* never tuned — unknown band, cold BW/band state */
  const bool cur_2g = _last_tuned_ch <= 14;
  if (cur_2g != (channel <= 14))
    return false; /* band change needs RFE pins / band block — full path only */
  if (channel == _last_tuned_ch)
    return true; /* no-op hop */

  devourer::HopProf prof(_logger->events(), _cfg.debug.hop_prof, "j2",
                         channel);
  const uint8_t cch = central_ch(channel, bw, primary_ch_idx);
  const bool g2 = cch <= 14;
  const bool c8821 = (_variant == ChipVariant::C8821C);
  const bool r2t2r = _ver.rf_2t2r != 0;

  /* Compose-cache prime — one read per touched register per epoch; the hop
   * itself is then write-only. RF18's band bits ([16]/[8], band-keyed) and BW
   * bits ([11:10], bandwidth-keyed) ride along in the cached value; the AGC /
   * fc / RF 0xBE dwords carry their untouched neighbour bits the same way.
   * cache_rf=false re-primes every hop (the A/B read-penalty knob). */
  if (!cache_rf || !_cw_primed) {
    _rf18_cache = rf_read(0, 0x18);
    _cw_agc = _device.rtw_read32(c8821 ? 0x0c1c : 0x0958);
    _cw_fc = _device.rtw_read32(0x0860);
    if (!c8821)
      _cw_rfbe = rf_read(0, 0xbe);
    _cw_primed = true;
  }
  prof.mark("prime");
  uint32_t rf18 = _rf18_cache & ~((1u << 18) | (1u << 17) | 0xffu);
  rf18 |= cch;

  /* Channel-keyed constants, written only when their bucket moves (the caches
   * are invalidated by every full set, so the first fast hop writes them once).
   * AGC table index + CFO-tracking fc share buckets across the variants; only
   * the AGC register differs (0x958[4:0] on 8822B, 0xc1c[11:8] on 8821C). The
   * bucket gaps (e.g. cch 65..99) match the full path, which leaves the
   * registers untouched there. */
  int agc_bucket = -1;
  if (g2)
    agc_bucket = 0;
  else if (cch <= 64) /* incl. below-band 15..35 (extended-range clamp) */
    agc_bucket = 1;
  else if (cch >= 100 && cch <= 144)
    agc_bucket = 2;
  else if (cch >= 149)
    agc_bucket = 3;
  if (agc_bucket >= 0 && agc_bucket != _last_agc_bucket) {
    if (c8821) {
      _cw_agc = (_cw_agc & ~0x00000F00u) |
                (static_cast<uint32_t>(agc_bucket) << 8);
      _device.rtw_write32(0x0c1c, _cw_agc);
    } else {
      _cw_agc = (_cw_agc & ~0x1Fu) | static_cast<uint32_t>(agc_bucket);
      _device.rtw_write32(0x0958, _cw_agc);
    }
    _last_agc_bucket = agc_bucket;
  }

  uint32_t fc = 0xffffffff;
  if (g2)
    fc = 0x96a;
  else if (cch <= 48) /* incl. below-band 15..35 (extended-range clamp) */
    fc = 0x494;
  else if (cch >= 52 && cch <= 64)
    fc = 0x453;
  else if (cch >= 100 && cch <= 116)
    fc = 0x452;
  else if (cch >= 118) /* open-ended: extended channels >177 keep 0x412 */
    fc = 0x412;
  if (fc != 0xffffffff && fc != _last_fc) {
    _cw_fc = (_cw_fc & ~0x1ffe0000u) | (fc << 17);
    _device.rtw_write32(0x0860, _cw_fc);
    _last_fc = fc;
  }

  if (!c8821) {
    /* 8822B: RF 0xBE VCO band (per-5G-channel — omitting it detunes the 5G
     * VCO), the ch144 RF 0xDF[18] flag, and the 2G spur registers. */
    const uint8_t rf_be = rf_be_for_8822b(cch);
    if (rf_be != 0xff && rf_be != _last_rf_be) {
      _cw_rfbe = (_cw_rfbe & ~0x38000u) |
                 (static_cast<uint32_t>(rf_be) << 15);
      rf_write(0, 0xbe, _cw_rfbe);
      _last_rf_be = rf_be;
    }
    const int df18 = (cch == 144) ? 1 : 0;
    if (df18 != _last_df18) {
      rf_set(0, 0xdf, (1u << 18), static_cast<uint32_t>(df18));
      _last_df18 = df18;
    }
    if (cch == 144)
      rf18 |= (1u << 17);
    else if (cch > 144)
      rf18 |= (1u << 18);
    else if (cch >= 80)
      rf18 |= (1u << 17);
    if (g2) {
      const int key = (cch == 14) ? 1 : 0;
      if (key != _last_cck_key) {
        if (cch == 14) {
          _device.phy_set_bb_reg(0x0a24, 0xffffffff, 0x00006577);
          _device.phy_set_bb_reg(0x0a28, 0x0000ffff, 0x0000);
        } else {
          _device.phy_set_bb_reg(0x0a24, 0xffffffff, 0x384f6577);
          _device.phy_set_bb_reg(0x0a28, 0x0000ffff, 0x1525);
        }
        _last_cck_key = key;
      }
      /* Reproduce the vendor's clean 2G RF18 (the primed read can carry the
       * spurious bits 8/16 — see set_channel_bw). */
      rf18 &= 0x00060cffu;
    }
  } else {
    /* 8821C: 5G sub-band bits use its own buckets; 2G CCK filter is ch14 vs
     * the BB-table snapshot. The band bits [16]/[8] and the BTG/WLG/WLA RF-set
     * are band-keyed (switch_band) and stay untouched. */
    if (!g2) {
      if (cch >= 100 && cch <= 140)
        rf18 |= (1u << 17);
      else if (cch > 140)
        rf18 |= (1u << 18);
    } else {
      const int key = (cch == 14) ? 1 : 0;
      if (key != _last_cck_key) {
        if (cch == 14) {
          _device.phy_set_bb_reg(0x0a24, 0xffffffff, 0x0000b81c);
          _device.phy_set_bb_reg(0x0a28, 0x0000ffff, 0x0000);
          _device.phy_set_bb_reg(0x0aac, 0xffffffff, 0x00003667);
        } else if (_cck_saved_8821c) {
          _device.phy_set_bb_reg(0x0a24, 0xffffffff, _cck_a24_8821c);
          _device.phy_set_bb_reg(0x0a28, 0x0000ffff, _cck_a28_8821c & 0xffff);
          _device.phy_set_bb_reg(0x0aac, 0xffffffff, _cck_aac_8821c);
        }
        _last_cck_key = key;
      }
    }
  }

  prof.mark("consts");
  rf_write(0, 0x18, rf18);
  if (!c8821 && r2t2r)
    rf_write(1, 0x18, rf18);
  _rf18_cache = rf18; /* refresh so the next hop merges into what we wrote */
  prof.mark("rf18");

  /* No per-hop RX kick. The vendor switch_channel tail (8822B RF 0xb8 +
   * RX-path toggles, IGI toggle) stays in the full path, but a hop does not
   * need it — hardware-measured on both variants, both directions: a hopping
   * receiver catches a parked beacon at the same per-dwell rate with and
   * without the kick (8822BU/8821CU: identical medians, no decay over ~70
   * kickless retunes), and hopping-TX delivery is unchanged. The kick was
   * >80% of the hop's USB round-trips. */

  _last_tuned_ch = channel;
  DVR_DEBUG(_logger, "Jaguar2: fast retune -> ch {} (central {}, RF18=0x{:05x})",
                 channel, cch, rf18);
  /* The fast path must emit the canary itself (it does not pass through the
   * full path) or the parity diff compares a stale full-set dump. */
  if (_cfg.debug.dump_canary)
    DumpCanary();
  return true;
}

void HalJaguar2::config_trx_mode() {
  /* config_phydm_trx_mode_8821c is a no-op in the vendor (return true): the
   * 8821C RF path/mode comes from the radioa table + switch_rf_set (BTG/WLG) +
   * the 0x808 block-enable, NOT an 8822B-style RF mode-table poke. Running the
   * 8822B sequence below (RF 0xc08/0x3e/0x3f mode table, 0x808/0x940 path map)
   * on the 8821C RF drives it into a wrong mode. Skip for C8821C. */
  if (_variant == ChipVariant::C8821C)
    return;
  const uint8_t path = _ver.rf_2t2r ? 0x3 : 0x1; /* BB_PATH_AB or A */

  /* RF mode table (3-wire state per path: 0 shutdown/1 standby/2 TX/3 RX). */
  _device.phy_set_bb_reg(0x0c08, 0x0000ffff, 0x3231);
  if (_ver.rf_2t2r)
    _device.phy_set_bb_reg(0x0e08, 0x0000ffff, 0x3231);

  /* --- phydm_config_tx_path_8822b (antenna-path HW-block enable + TX logic
   * map). tx_path = AB (2T2R) or A (1T1R); tx_path_sel_1ss = A. --- */
  _device.phy_set_bb_reg(0x093c, (1u << 19) | (1u << 18), 0x3);
  _device.phy_set_bb_reg(0x080c, (1u << 29) | (1u << 28), 0x1);
  _device.phy_set_bb_reg(0x080c, (1u << 30), 0x1); /* CCK TX path via 0xa07[7] */
  _device.phy_set_bb_reg(0x080c, 0xff, (path << 4) | path);
  /* CCK TX path (phydm_config_cck_tx_path_8822b). The vendor calls
   * phydm_config_tx_path_8822b(dm, tx_path, tx_path_sel_1ss, tx_path_sel_1ss) —
   * i.e. tx_path_sel_cck == tx_path_sel_1ss == BB_PATH_A, so CCK is single-path A
   * (0xa04[31:28]=0x8) even on a 2T2R part. (devourer previously used the full
   * AB path 0xc here; the kernel reads 0x8.) */
  _device.phy_set_bb_reg(0x0a04, 0xf0000000, 0x8);
  /* OFDM TX path (phydm_config_ofdm_tx_path_8822b, 1ss=A). */
  _device.phy_set_bb_reg(0x093c, 0xfff00000, 0x001);
  if (_ver.rf_2t2r) {
    _device.phy_set_bb_reg(0x0940, 0xfff0, 0x043);
  } else {
    _device.phy_set_bb_reg(0x0940, 0xf0, 0x1);
    _device.phy_set_bb_reg(0x0940, 0xff00, 0x0);
  }

  /* phydm_somlrxhp_setting(true) — SoML (adaptive ML detection) ON, the
   * kernel's steady state on the 8822B (0x19a8 = 0xd90a0000; devourer
   * previously left the table default 0 = SoML off, a state the vendor only
   * uses for specific RFE types). The band-specific RxHP companion values
   * (0x8cc/0x8d8) are set in set_channel_bw's band block. */
  _device.phy_set_bb_reg(0x19a8, 0xffffffff, 0xd90a0000);

  /* --- phydm_config_rx_path_8822b(AB) --- */
  _device.phy_set_bb_reg(0x0a2c, (1u << 22), 0x0); /* disable MRC CCK CCA */
  _device.phy_set_bb_reg(0x0a2c, (1u << 18), 0x0); /* disable MRC CCK barker */
  _device.phy_set_bb_reg(0x0a04, 0x0f000000, 0x0); /* CCK RX path A first */
  _device.phy_set_bb_reg(0x0808, 0xff, (path << 4) | path); /* RX path enable */
  if (_ver.rf_2t2r) {
    _device.phy_set_bb_reg(0x1904, (1u << 16), 0x1); /* antenna weighting */
    _device.phy_set_bb_reg(0x0800, (1u << 28), 0x1); /* htstf ant-wgt */
    _device.phy_set_bb_reg(0x0850, (1u << 23), 0x1); /* MRC modified-ZF */
  } else {
    _device.phy_set_bb_reg(0x1904, (1u << 16), 0x0);
    _device.phy_set_bb_reg(0x0800, (1u << 28), 0x0);
    _device.phy_set_bb_reg(0x0850, (1u << 23), 0x0);
  }

  /* RF mode-table sync: poll RF_A 0x33 until it reads back 0x1. */
  for (int i = 0; i < 100; i++) {
    rf_write(0, 0xef, 0x80000);
    rf_write(0, 0x33, 0x00001);
    std::this_thread::sleep_for(std::chrono::microseconds(2));
    if ((rf_read(0, 0x33) & 0xfffff) == 0x00001)
      break;
  }
  /* Normal mode (no path-B-only TRX): 0xef/0x33/0x3e/0x3f then 0xef=0. */
  rf_write(0, 0xef, 0x80000);
  rf_write(0, 0x33, 0x00001);
  rf_write(0, 0x3e, 0x00034);
  rf_write(0, 0x3f, 0x4080c);
  rf_write(0, 0xef, 0x00000);
  _logger->info("Jaguar2: trx_mode configured (path=0x{:x})", path);
}

void HalJaguar2::do_lck() {
  if (_variant == ChipVariant::C8821C) {
    do_lck_8821c();
    return;
  }
  /* aac_check_8822b (once): fix the AAC code if out of range. */
  if (!_aac_checked) {
    uint32_t temp = (rf_read(0, 0xc9) & 0xf8) >> 3;
    if (temp < 4 || temp > 7) {
      rf_set(0, 0xca, (1u << 19), 0x0);
      rf_set(0, 0xb2, 0x7c000, 0x6);
    }
    _aac_checked = true;
  }

  uint32_t c00 = _device.rtw_read32(0x0c00);
  uint32_t e00 = _device.rtw_read32(0x0e00);
  _device.rtw_write32(0x0c00, 0x4);
  _device.rtw_write32(0x0e00, 0x4);
  rf_write(0, 0x0, 0x10000);
  if (_ver.rf_2t2r)
    rf_write(1, 0x0, 0x10000);

  /* Backup RF CHNLBW using the FULL read, exactly like the vendor
   * _phy_lc_calibrate_8822b (odm_get_rf_reg RF_CHNLBW, RFREGOFFSETMASK). An
   * earlier version masked lc_cal to 0x00060cff — but on 5G that clears the band
   * bits (8/16), so the LC calibration ran against a 2.4G-band RF18 (mis-tuning
   * the 5G VCO) AND the recover-write left RF18 masked, overriding the correct
   * 5G channel from set_channel_bw. The LCK-done poll may "time out" (the direct
   * RF read returns transient status bits) but the LO does lock. */
  uint32_t lc_cal = rf_read(0, 0x18); /* backup RF CHNLBW (full, no mask) */
  rf_write(0, 0xc4, 0x01402);         /* disable RTK */
  rf_write(0, 0x18, lc_cal | 0x08000); /* start LCK */
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  bool locked = false;
  for (int cnt = 0; cnt < 5; cnt++) {
    if ((rf_read(0, 0x18) & 0x8000) == 0) { locked = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  rf_write(0, 0x18, lc_cal);  /* recover channel */
  rf_write(0, 0xc4, 0x81402); /* enable RTK */
  _device.rtw_write32(0x0c00, c00);
  _device.rtw_write32(0x0e00, e00);
  rf_write(0, 0x0, 0x3ffff);
  if (_ver.rf_2t2r)
    rf_write(1, 0x0, 0x3ffff);
  _logger->info("Jaguar2: LCK {} (RF_A 0x18={:05x} 0x00={:05x})",
                locked ? "LOCKED" : "TIMEOUT (LO not locked!)",
                rf_read(0, 0x18), rf_read(0, 0x0));
}

/* _phy_lc_calibrate_8821c: the 8821C LCK is an RF-firmware poke sequence (no
 * host poll) — completely unlike the 8822B RF 0x18[15] toggle+poll (which just
 * times out on 8821C). aac_check first, then RF 0xcc/0xc4/0xcc. */
void HalJaguar2::do_lck_8821c() {
  if (!_aac_checked) {
    uint32_t temp = (rf_read(0, 0xc9) & 0xf8) >> 3;
    if (temp < 4 || temp > 7) {
      rf_set(0, 0xca, (1u << 19), 0x0);
      rf_set(0, 0xb2, 0x7c000, 0x6);
    }
    _aac_checked = true;
  }
  rf_write(0, 0xcc, 0x2018);
  rf_write(0, 0xc4, 0x8f602);
  rf_write(0, 0xcc, 0x201c);
  _logger->info("Jaguar2/8821C: LCK applied (RF-fw poke)");
}

/* config_phydm_switch_rf_set_8821c: select the 2.4G RX front-end path. BTG
 * (BT-shared, path B) vs WLG (WiFi-only, path A) is chosen from rfe_type_expand.
 * The base mux writes (0xcb8/0xa84/0xa80) apply in normal (non-MP) operation;
 * the agc_tab_diff overlay + IGI toggle are MP-mode-only and omitted. */
void HalJaguar2::switch_rf_set_8821c(uint8_t rf_set) {
  /* rf_set: 0=BTG (2.4G BT-shared, path B), 1=WLG (2.4G WiFi, path A),
   * 2=WLA (5G). Transcribed from config_phydm_switch_rf_set_8821c. */
  _device.phy_set_bb_reg(0x1080, (1u << 16), 0x1);
  _device.phy_set_bb_reg(0x0000, (1u << 26), 0x1);
  uint32_t cb8 = _device.rtw_read32(0x0cb8);
  if (rf_set == 0) { /* BTG */
    cb8 |= (1u << 16);
    cb8 &= ~((1u << 18) | (1u << 20) | (1u << 21) | (1u << 22) | (1u << 23));
    _device.phy_set_bb_reg(0x0a84, 0x00ff0000, 0x0e);
    _device.phy_set_bb_reg(0x0a80, 0x0000ffff, 0xfc84);
  } else if (rf_set == 1) { /* WLG */
    cb8 |= (1u << 20) | (1u << 21) | (1u << 22);
    cb8 &= ~((1u << 16) | (1u << 18) | (1u << 23));
    _device.phy_set_bb_reg(0x0a84, 0x00ff0000, 0x12);
    _device.phy_set_bb_reg(0x0a80, 0x0000ffff, 0x7532);
  } else { /* WLA (5G) — cb8 bits only, no 0xa84/0xa80 override */
    cb8 |= (1u << 20) | (1u << 22) | (1u << 23);
    cb8 &= ~((1u << 16) | (1u << 18) | (1u << 21));
  }
  _device.phy_set_bb_reg(0x0cb8, 0xffffffff, cb8);
}

/* config_phydm_switch_band/channel/bandwidth_8821c, transcribed from
 * reference/8821cu phydm_hal_api8821c.c. 1T1R, RF path A; the 2.4G RX path is
 * routed by switch_rf_set (BTG/WLG). Covers 2.4G and 5G at 20/40/80 MHz. */
void HalJaguar2::set_channel_bw_8821c(uint8_t channel, uint8_t bw,
                                      uint8_t rfe_raw, uint8_t primary_ch_idx) {
  const uint8_t cch = central_ch(channel, bw, primary_ch_idx);
  const bool g2 = cch <= 14;
  /* rfe_type_expand -> BTG (else WLG) per config_phydm_parameter_init_8821c. */
  const bool btg = rfe_raw == 2 || rfe_raw == 4 || rfe_raw == 7 ||
                   rfe_raw == 0x22 || rfe_raw == 0x24 || rfe_raw == 0x27 ||
                   rfe_raw == 0x2a || rfe_raw == 0x2c || rfe_raw == 0x2f;

  /* --- config_phydm_switch_band_8821c --- */
  uint32_t rf18 = rf_read(0, 0x18);
  if (g2) {
    _device.phy_set_bb_reg(0x0808, (1u << 28), 0x1); /* enable CCK block */
    _device.phy_set_bb_reg(0x0454, (1u << 7), 0x0);  /* MAC CCK check off */
    _device.phy_set_bb_reg(0x0a80, (1u << 18), 0x0); /* BB CCK check off */
    _device.phy_set_bb_reg(0x0814, 0x0000FC00, 15);  /* CCA mask default */
    rf18 &= ~((1u << 16) | (1u << 9) | (1u << 8) | 0xffu);
    rf18 |= cch;
    switch_rf_set_8821c(btg ? 0 : 1); /* BTG or WLG */
    rf_set(0, 0xdf, (1u << 6), 0x1);   /* RF TXA_TANK LUT mode */
    rf_set(0, 0x64, 0x0000f, 0xf);     /* RF TXA_PA_TANK */
  } else {
    _device.phy_set_bb_reg(0x0a80, (1u << 18), 0x1);
    _device.phy_set_bb_reg(0x0454, (1u << 7), 0x1);
    _device.phy_set_bb_reg(0x0808, (1u << 28), 0x0);
    _device.phy_set_bb_reg(0x0814, 0x0000FC00, 15);
    rf18 &= ~((1u << 16) | (1u << 9) | (1u << 8) | 0xffu);
    rf18 |= (1u << 8) | (1u << 16) | cch;
    switch_rf_set_8821c(2); /* WLA (5G) */
    rf_set(0, 0xdf, (1u << 6), 0x0);
  }
  rf_write(0, 0x18, rf18);

  /* --- config_phydm_switch_channel_8821c --- */
  rf18 = rf_read(0, 0x18);
  rf18 &= ~((1u << 18) | (1u << 17) | 0xffu);
  rf18 |= cch;
  if (g2) {
    _device.phy_set_bb_reg(0x0c1c, 0x00000F00, 0x0);      /* AGC table idx 0 */
    _device.phy_set_bb_reg(0x0860, 0x1ffe0000, 0x96a);    /* fc for CFO track */
    if (cch == 14) {
      _device.phy_set_bb_reg(0x0a24, 0xffffffff, 0x0000b81c);
      _device.phy_set_bb_reg(0x0a28, 0x0000ffff, 0x0000);
      _device.phy_set_bb_reg(0x0aac, 0xffffffff, 0x00003667);
    } else if (_cck_saved_8821c) {
      _device.phy_set_bb_reg(0x0a24, 0xffffffff, _cck_a24_8821c);
      _device.phy_set_bb_reg(0x0a28, 0x0000ffff, _cck_a28_8821c & 0xffff);
      _device.phy_set_bb_reg(0x0aac, 0xffffffff, _cck_aac_8821c);
    }
  } else {
    /* Extended-range clamps (see the 8822B branch): ch 15..35 rides the low
     * bucket, ch >177 keeps the top bucket. */
    if (cch <= 64)
      _device.phy_set_bb_reg(0x0c1c, 0x00000F00, 0x1);
    else if (cch >= 100 && cch <= 144)
      _device.phy_set_bb_reg(0x0c1c, 0x00000F00, 0x2);
    else if (cch >= 149)
      _device.phy_set_bb_reg(0x0c1c, 0x00000F00, 0x3);
    if (cch <= 48)
      _device.phy_set_bb_reg(0x0860, 0x1ffe0000, 0x494);
    else if (cch >= 52 && cch <= 64)
      _device.phy_set_bb_reg(0x0860, 0x1ffe0000, 0x453);
    else if (cch >= 100 && cch <= 116)
      _device.phy_set_bb_reg(0x0860, 0x1ffe0000, 0x452);
    else if (cch >= 118)
      _device.phy_set_bb_reg(0x0860, 0x1ffe0000, 0x412);
    if (cch >= 100 && cch <= 140)
      rf18 |= (1u << 17);
    else if (cch > 140)
      rf18 |= (1u << 18);
  }
  rf_write(0, 0x18, rf18);

  /* --- config_phydm_switch_bandwidth_8821c --- */
  rf18 = rf_read(0, 0x18);
  if (bw == 1) { /* 40 MHz */
    _device.phy_set_bb_reg(0x0a00, (1u << 4), primary_ch_idx == 1 ? 1 : 0);
    uint32_t v8ac = _device.rtw_read32(0x08ac);
    v8ac &= 0xff3ff300;
    v8ac |= (static_cast<uint32_t>((primary_ch_idx & 0xf) << 2) | 0x20020000u |
             0x1u);
    _device.phy_set_bb_reg(0x08ac, 0xffffffff, v8ac);
    _device.phy_set_bb_reg(0x08c4, (1u << 30), 0x1);
    rf18 &= ~((1u << 11) | (1u << 10));
    rf18 |= (1u << 11);
  } else if (bw == 2) { /* 80 MHz */
    uint32_t v8ac = _device.rtw_read32(0x08ac);
    v8ac &= 0xfcffcf00;
    v8ac |= (static_cast<uint32_t>((primary_ch_idx & 0xf) << 2) | 0x40040000u |
             0x2u);
    _device.phy_set_bb_reg(0x08ac, 0xffffffff, v8ac);
    _device.phy_set_bb_reg(0x08c4, (1u << 30), 0x1);
    rf18 &= ~((1u << 11) | (1u << 10));
    rf18 |= (1u << 10);
  } else if (bw == 5) { /* CHANNEL_WIDTH_5 — narrowband baseband re-clock:
    * small-BW 0x8ac[7:6]=1, ADC clock 40M ([9:8]=0x2, [16]=0), DAC field
    * ([21:20]=0x2, [28]=0); RF synth stays in 20 MHz mode (vendor
    * config_phydm_switch_bandwidth_8821c CHANNEL_WIDTH_5). */
    uint32_t v8ac = _device.rtw_read32(0x08ac);
    v8ac &= 0xefcefc00;
    v8ac |= (0x2u << 20) | (0x2u << 8) | (1u << 6);
    _device.phy_set_bb_reg(0x08ac, 0xffffffff, v8ac);
    _device.phy_set_bb_reg(0x08c4, (1u << 30), 0x0); /* ADC buffer clock */
    _device.phy_set_bb_reg(0x08c8, (1u << 31), 0x1);
    rf18 |= (1u << 11) | (1u << 10); /* RF stays 20M */
  } else if (bw == 6) { /* CHANNEL_WIDTH_10 — small-BW=2, ADC 80M ([9:8]=0x3),
                         * DAC field 0x3 */
    uint32_t v8ac = _device.rtw_read32(0x08ac);
    v8ac &= 0xefcefc00;
    v8ac |= (0x3u << 20) | (0x3u << 8) | (1u << 7);
    _device.phy_set_bb_reg(0x08ac, 0xffffffff, v8ac);
    _device.phy_set_bb_reg(0x08c4, (1u << 30), 0x0); /* ADC buffer clock */
    _device.phy_set_bb_reg(0x08c8, (1u << 31), 0x1);
    rf18 |= (1u << 11) | (1u << 10); /* RF stays 20M */
  } else { /* 20 MHz */
    uint32_t v8ac = _device.rtw_read32(0x08ac);
    v8ac &= 0xffcffc00;
    v8ac |= 0x10010000;
    _device.phy_set_bb_reg(0x08ac, 0xffffffff, v8ac);
    _device.phy_set_bb_reg(0x08c4, (1u << 30), 0x1);
    rf18 |= (1u << 11) | (1u << 10);
  }
  rf_write(0, 0x18, rf18);

  /* phydm_rxdfirpar_by_bw_8821c: the RX digital filter must match the bandwidth
   * or the OFDM demod never completes a PSDU (energy detected, nothing reaches
   * the MAC RX FIFO) — the 8821C "first RX" piece. 1T1R: path A regs only
   * (0x948/0x94c/0xc20/0x8f0), distinct from the 8822B set. 5/10 MHz share the
   * BW20 filter values (the narrowing happens in the ADC/DAC re-clock, the
   * demod still runs a 20 MHz signal shape). */
  const bool nb = (bw == 5 || bw == 6);
  _device.phy_set_bb_reg(0x0948, (1u << 29) | (1u << 28), 0x2);
  _device.phy_set_bb_reg(0x094c, (1u << 29) | (1u << 28),
                         bw == 2 ? 0x1u : 0x2u);
  _device.phy_set_bb_reg(0x0c20, (1u << 31), (bw == 0 || nb) ? 0x1u : 0x0u);
  _device.phy_set_bb_reg(0x08f0, (1u << 31), bw == 2 ? 0x1u : 0x0u);

  igi_toggle();

  /* 5/10 MHz: BB reset so the DAC/DFE relatch at the new sample rate (see the
   * 8822B path). */
  if (nb)
    bb_reset();
  _logger->info("Jaguar2/8821C: channel set ch={} bw={} cch={} {} (rf18={:05x})",
                channel, (int)bw, cch, btg ? "BTG" : "WLG", rf18);
}

/* ex_hal8822b_wifi_only_hw_config + hal8822b_wifi_only_switch_antenna: grant the
 * antenna to WLAN. Without the hw_config block the combo chip leaves the antenna
 * switch owned by BT and the WL RX is deaf. The switch_antenna write (0xcbc[9:8])
 * selects which physical antenna path the WL side drives, per band — the kernel
 * fires it from rtw_btcoex_wifionly_switchband_notify() during every channel/band
 * set. Omitting it leaves the antenna-path select bits at their reset value, so
 * WL TX/RX runs through an unselected/mismatched path (weak, distorted on air). */
void HalJaguar2::coex_wlan_only(bool is_5g) {
  if (_variant == ChipVariant::C8821C) {
    coex_wlan_only_8821c(is_5g);
    return;
  }
  _device.phy_set_bb_reg(0x004c, 0x01800000, 0x2); /* BB control */
  _device.phy_set_bb_reg(0x0cb4, 0xff, 0x77);      /* SW control */
  _device.phy_set_bb_reg(0x0974, 0x300, 0x3);      /* antenna mux switch */
  _device.phy_set_bb_reg(0x1990, 0x300, 0x0);
  _device.phy_set_bb_reg(0x0cbc, 0x80000, 0x0);
  _device.phy_set_bb_reg(0x0070, 0xff000000, 0x0e); /* WL-side controller */
  _device.phy_set_bb_reg(0x1704, 0xffffffff, 0x7700);     /* gnt_wl=1 gnt_bt=0 */
  _device.phy_set_bb_reg(0x1700, 0xffffffff, 0xc00f0038);
  /* hal8822b_wifi_only_switch_antenna(is_5g): 0xcbc[9:8] = 1 (5G) / 2 (2.4G) */
  _device.phy_set_bb_reg(0x0cbc, 0x300, is_5g ? 0x1 : 0x2);
  _logger->info("Jaguar2: coex WL-only antenna grant applied (is_5g={})",
                is_5g ? 1 : 0);
}

/* ex_hal8821c_wifi_only_hw_config + hal8821c_wifi_only_switch_antenna
 * (reference/8821cu/hal/btc/halbtc8821cwifionly.c). The 8821C wifi-only coex
 * differs from the 8822B: the GNT/owner and the SPDT/DPDT external-antenna
 * switch use different registers (0x70[26], 0x1700/0x1704, 0x6c0/0x6c4, and the
 * antenna switch on 0xcb4[29:28] driven by rfe_module_type — NOT the 8822B
 * 0xcbc[9:8]). */
void HalJaguar2::coex_wlan_only_8821c(bool is_5g) {
  /* hw_config: GNT owner -> WL, gnt_wl=1/gnt_bt=0. */
  _device.phy_set_bb_reg(0x0070, 0x04000000, 0x1);
  _device.phy_set_bb_reg(0x1704, 0xffffffff, 0x7700);
  _device.phy_set_bb_reg(0x1700, 0xffffffff, 0xc00f0038);
  _device.phy_set_bb_reg(0x06c0, 0xffffffff, 0xaaaaaaaa);
  _device.phy_set_bb_reg(0x06c4, 0xffffffff, 0xaaaaaaaa);

  /* switch_antenna: rfe_module_type = efuse RFE & 0x1f decides the ext-ant
   * switch existence + polarity. */
  const uint8_t rfe_mod =
      static_cast<uint8_t>((_efuse_valid ? _efuse_map[0xCA] : 0) & 0x1f);
  const bool exist = !(rfe_mod == 5 || rfe_mod == 6);
  if (!exist) {
    _logger->info("Jaguar2/8821C: coex WL-only (rfe_mod={} no ext-ant switch)",
                  rfe_mod);
    return;
  }
  const bool wlg_at_btg = (rfe_mod == 2 || rfe_mod == 4 || rfe_mod == 7);
  const bool ant_at_main = !(rfe_mod == 3 || rfe_mod == 4);
  bool inv = false; /* ext_ant_switch_ctrl_polarity = 0 */
  if (!ant_at_main)
    inv = !inv;
  if (!is_5g && !wlg_at_btg) /* WLG: swap if WLG not at BTG */
    inv = !inv;

  /* BBSW control (no antenna-diversity): 0x4c[24:23]=2, 0xcb4[7:0]=0x77,
   * 0xcb4[29:28] = 1 (no inverse) / 2 (inverse). */
  _device.phy_set_bb_reg(0x004c, 0x01800000, 0x2);
  _device.phy_set_bb_reg(0x0cb4, 0x000000ff, 0x77);
  _device.phy_set_bb_reg(0x0cb4, 0x30000000, inv ? 0x2 : 0x1);
  _logger->info("Jaguar2/8821C: coex WL-only antenna (rfe_mod={} wlg@btg={} "
                "main={} inv={} is_5g={})",
                rfe_mod, wlg_at_btg, ant_at_main, inv, is_5g ? 1 : 0);
}

/* rtl8821c_phy_bf_init (reference/8821cu/hal/rtl8821c/rtl8821c_phy.c): MU-MIMO /
 * TXBF MAC setup the vendor runs every init. The 8822B bf_init devourer used
 * only wrote 0x1c94 (the grouping bitmap, the LAST write here) and missed the
 * MU/TXBF MAC registers — so the 8821C gets its own faithful bf_init. */
void HalJaguar2::bf_init_8821c() {
  /* REG_MU_TX_CTL (0x14C0): P1 wait-state EN, MU RL=0xA, disable MU-MIMO until
   * sounding done, clear MU-STA table valid. */
  uint32_t v32 = _device.rtw_read32(0x14C0);
  v32 |= (1u << 16);                          /* BIT_R_MU_P1_WAIT_STATE_EN */
  v32 = (v32 & ~(0xfu << 12)) | (0xAu << 12); /* MU RL = 0xA */
  v32 &= ~(1u << 7);                          /* disable MU-MIMO */
  v32 &= ~(0x3fu << 0);                       /* MU table valid = 0 */
  _device.rtw_write32(0x14C0, v32);

  /* REG_MU_BF_OPTION (0x167C): TXMU ACKPOLICY=3 + EN = 0x70. */
  _device.rtw_write8(0x167C, static_cast<uint8_t>((3u << 4) | (1u << 6)));
  /* REG_WMAC_MU_BF_CTL (0x1680) = 0. */
  _device.rtw_write16(0x1680, 0x0);

  /* REG_TXBF_CTRL+3 (0x042F): use NDPA parameter from 0x45F (BIT30>>24=0x40). */
  _device.rtw_write8(0x042F, static_cast<uint8_t>(
                                 _device.rtw_read8(0x042F) | 0x40));
  /* REG_NDPA_OPT_CTRL (0x045F) = 0x10 (NDPA rate OFDM_6M, BW20). */
  _device.rtw_write8(0x045F, 0x10);

  /* STA2 CSI rate fixed at 6M (0x6DF[5:0]=0x4, keep [7:6]). */
  _device.rtw_write8(0x06DF, static_cast<uint8_t>(
                                 (_device.rtw_read8(0x06DF) & 0xC0) | 0x4));
  /* Grouping bitmap (0x1C94) — same value as the 8822B bf_init. */
  _device.rtw_write32(0x1C94, 0xAFFFAFFFu);

  /* rfe_type-2 (1212 module) 5G-RX hardware fix from rtl8821c_hal_init_misc:
   * REG_PAD_CTRL1+3 (0x0067) = 0x36. Only for rfe_type 2 (efuse RFE & 0x1f). */
  if (((_efuse_valid ? _efuse_map[0xCA] : 0) & 0x1f) == 2)
    _device.rtw_write8(0x0067, 0x36);

  _logger->info("Jaguar2/8821C: bf_init (MU/TXBF MAC setup + 0x1c94)");
}

void HalJaguar2::rfe_init() {
  /* phydm_rfe_8822b_init (verbatim): chip-top mux + RFE s0/s1 source select.
   * The kernel runs this from odm_dm_init->phydm_rfe_init after BB/RF config. */
  _device.phy_set_bb_reg(0x0064, (1u << 29) | (1u << 28), 0x3); /* chip top mux */
  _device.phy_set_bb_reg(0x004c, (1u << 26) | (1u << 25), 0x0);
  _device.phy_set_bb_reg(0x0040, (1u << 2), 0x1);
  _device.phy_set_bb_reg(0x1990, 0x3f, 0x30);                   /* from s0/s1 */
  _device.phy_set_bb_reg(0x1990, (1u << 11) | (1u << 10), 0x3);
  _device.phy_set_bb_reg(0x0974, 0x3f, 0x3f);                   /* in / out */
  _device.phy_set_bb_reg(0x0974, (1u << 11) | (1u << 10), 0x3);
  _logger->info("Jaguar2: RFE init (chip-top mux, 0x1990=0xc30)");
}

void HalJaguar2::bf_init() {
  /* rtl8822b_phy_bf_init (verbatim): MU-MIMO / TXBF default setup. Register
   * offsets/bits from halmac_reg_8822b.h + rtl8822b_phy.c. */
  uint32_t v32 = _device.rtw_read32(0x14c0);        /* REG_MU_TX_CTL */
  v32 |= (1u << 16);                                /* MU P1 wait-state en */
  v32 = (v32 & ~(0xFu << 12)) | (0xAu << 12);       /* MU retry limit = 0xA */
  v32 &= ~(1u << 7);                                /* disable MU-MIMO */
  v32 &= ~0x3fu;                                    /* clear MU table valid */
  _device.rtw_write32(0x14c0, v32);

  _device.rtw_write8(0x167c, (3u << 4) | (1u << 6)); /* MU BF ACKPOLICY=3, EN */
  _device.rtw_write16(0x1680, 0);                    /* MU BF ctl default */

  uint8_t v8 = _device.rtw_read8(0x042f);            /* REG_TXBF_CTRL+3 */
  v8 |= (1u << 6);                                   /* USE_NDPA_PARAMETER>>24 */
  _device.rtw_write8(0x042f, v8);
  _device.rtw_write8(0x045f, 0x10);                  /* NDPA rate OFDM_6M/BW20 */

  v8 = _device.rtw_read8(0x06df);
  v8 = (v8 & 0xC0) | 0x4;                            /* STA2 CSI rate = 6M */
  _device.rtw_write8(0x06df, v8);
  _device.rtw_write32(0x1c94, 0xAFFFAFFFu);          /* grouping bitmap */
  _logger->info("Jaguar2: BF init (0x1c94=0xafffafff)");
}

void HalJaguar2::set_tx_power_flat(uint8_t idx) {
  idx &= 0x3f;
  const uint32_t v = static_cast<uint32_t>(idx) * 0x01010101u;
  for (uint16_t off = 0; off <= 0x54; off += 4) {
    _device.rtw_write32(static_cast<uint16_t>(0x1d00 + off), v); /* path A */
    _device.rtw_write32(static_cast<uint16_t>(0x1d80 + off), v); /* path B */
  }
  set_txagc_shadow_flat(idx); /* the block is write-only — see txagc_shadow */
  _logger->info("Jaguar2: TXAGC flat index 0x{:02x} applied", idx);
}

void HalJaguar2::read_thermal(uint8_t &raw, uint8_t &baseline) {
  /* Vendor 8822B/8821C halrf reads the meter with a plain
   * odm_get_rf_reg(RF_PATH_A, 0x42, 0xfc00) — no trigger needed. */
  raw = static_cast<uint8_t>((rf_read(0, 0x42) >> 10) & 0x3f);
  if (!_efuse_valid) {
    read_efuse_logical_map(_efuse_map, sizeof(_efuse_map), /*dump=*/false);
    _efuse_valid = true;
  }
  baseline = _efuse_map[0xBA]; /* EEPROM_THERMAL_METER_8822B/_8821C */
}

void HalJaguar2::apply_tx_power(uint8_t channel, uint8_t bw, uint8_t rfe_type,
                                int offset_steps) {
  /* Port of phy_get_pg_txpwr_idx (hal_com_phycfg.c) + the 4-byte TXAGC write
   * (config_phydm_write_txagc_8822b): TXAGC[rate] = pg-base(rate-section) +
   * per-Nss diff, clamped to the txpwr_lmt regulatory limit, written to 0x1d00
   * (path A) / 0x1d80 (path B). CONFIG_TXPWR_BY_RATE_EN is off upstream, so the
   * rate-section base is the value. Replaces the hot BB-table default that
   * overdrives high-order QAM into PA compression. Handles 2.4G and 5G (20 MHz);
   * BW40/80 is future work (uses BW40/BW80 diffs). */
  if (channel == 0)
    return;

  /* 5/10 MHz narrowband (CHANNEL_WIDTH_5=5 / _10=6) folds to the 20 MHz power
   * column: the RF runs in its 20 MHz mode and the txpwr_lmt tables carry no
   * narrowband rows (a raw 5/6 here would read as HT40 and miss the regulatory
   * lookup entirely -> unclamped 63). Mirrors the Jaguar3 fold. */
  if (bw >= 5)
    bw = 0;

  /* Fresh rail-hit snapshot for this apply (SetTxPowerOffsetQdb's "knob out
   * of travel" signal). */
  _txpwr_sat_low = false;
  _txpwr_sat_high = false;

  /* 8821C: the vendor hal_com_get_txpwr_idx computes per-rate power
   *   idx[rate] = clamp(base + (min(by_rate[rate], lmt) - rs_target), 0, 63)
   * where base = efuse per-channel section index (phy_get_pg_txpwr_idx),
   * by_rate = the phy_reg_pg value for the rate (array_mp_8821c_phy_reg_pg),
   * rs_target = by_rate of the section reference rate (rate_sec_base: CCK->11M,
   * OFDM->54M, HT->MCS7, VHT1SS->VHT1SS_MCS7), and lmt = worldwide-min
   * txpwr_lmt. This differs from the shared 8822B flat-per-section clamp; the
   * 8821C is 1T1R so only path A (0x1d00) is written. On an unprogrammed EFUSE
   * fall through to the shared path. */
  if (_variant == ChipVariant::C8821C) {
    if (!_efuse_valid) {
      read_efuse_logical_map(_efuse_map, sizeof(_efuse_map), /*dump=*/false);
      _efuse_valid = true;
    }
    const uint8_t *ef = _efuse_map;
    const bool g5c = channel > 14;
    const bool prog = g5c ? (ef[0x22] != 0xff)
                          : (ef[0x10] != 0xff && ef[0x16] != 0xff);
    if (prog) {
      const bool ht40 = bw >= 1;
      auto s4v = [](int x) { int v = x & 0xf; return (v & 8) ? v - 16 : v; };
      int g, bw40b, dpg;
      if (!g5c) {
        g = channel <= 2 ? 0 : channel <= 5 ? 1 : channel <= 8 ? 2
            : channel <= 11 ? 3 : 4;
        bw40b = ef[0x16 + g];
        dpg = ef[0x1b];
      } else {
        static const uint8_t lo[] = {36,  44,  50,  60,  100, 108, 116,
                                     124, 132, 140, 149, 157, 165, 173};
        static const uint8_t hi[] = {42,  48,  58,  64,  106, 114, 122,
                                     130, 138, 144, 155, 161, 171, 177};
        g = 0;
        for (int i = 0; i < 14; i++)
          if (channel >= lo[i] && channel <= hi[i]) { g = i; break; }
        bw40b = ef[0x22 + g];
        dpg = ef[0x30];
      }
      const int ofdm_diff = s4v(dpg & 0xf), bw20_diff = s4v((dpg >> 4) & 0xf);
      const int cg = (!g5c && channel == 14) ? 5 : g;
      const int cck_base = g5c ? 0 : ef[0x10 + cg];
      const int ofdm_base = bw40b + ofdm_diff;
      const int ht_base = bw40b + (ht40 ? 0 : bw20_diff);
      auto lmtc = [&](uint8_t sec) -> int {
#if defined(DEVOURER_HAVE_JAGUAR2_8821C)
        return hal8821c_txpwr_lmt(g5c ? 1 : 0, bw, sec, 1, channel);
#else
        (void)sec;
        return 63;
#endif
      };
      /* write a rate-section: per byte (rate) in each dword,
       * idx = clamp(base + (min(by_rate, lmt) - rs) + offset, 0, 63) — the
       * runtime offset folds after the regulatory min() (headroom above the
       * worldwide-min table up to the hw rail, same permissiveness as the
       * flat override). */
      auto wsec = [&](uint16_t off, int base, int rs, int lm,
                      const uint32_t *dw, int ndw) {
        for (int w = 0; w < ndw; w++) {
          uint32_t out = 0;
          for (int b = 0; b < 4; b++) {
            int byr = static_cast<int>((dw[w] >> (b * 8)) & 0xff);
            int rt = byr < lm ? byr : lm;
            int idx = base + (rt - rs) + offset_steps;
            if (idx < 0) { idx = 0; _txpwr_sat_low = true; }
            if (idx > 63) { idx = 63; _txpwr_sat_high = true; }
            out |= static_cast<uint32_t>(idx & 0xff) << (b * 8);
          }
          _device.rtw_write32(static_cast<uint16_t>(0x1d00 + off + w * 4), out);
        }
      };
      /* by_rate dwords (array_mp_8821c_phy_reg_pg) + rs = ref-rate by_rate.
       * Representative-rate shadow (the TXAGC block is write-only): the value
       * wsec computes for hw_rate 0x00 (CCK 1M) / 0x04 (OFDM 6M) / 0x13
       * (MCS7) — byte 0 of the section's first dword resp. byte 3 of the HT
       * section's second dword. */
      auto sec_idx = [&](int base, int rs, int lm, uint32_t dw, int b) -> int {
        int byr = static_cast<int>((dw >> (b * 8)) & 0xff);
        int rt = byr < lm ? byr : lm;
        int idx = base + (rt - rs) + offset_steps;
        return idx < 0 ? 0 : (idx > 63 ? 63 : idx);
      };
      if (!g5c) {
        const uint32_t cck[] = {0x32343638};
        const uint32_t ofdm[] = {0x36363636, 0x28303234};
        const uint32_t ht[] = {0x34363636, 0x26283032};
        const uint32_t vht[] = {0x34363636, 0x26283032, 0x22222224};
        wsec(0x00, cck_base, 50, lmtc(0), cck, 1);
        wsec(0x04, ofdm_base, 40, lmtc(1), ofdm, 2);
        wsec(0x0c, ht_base, 38, lmtc(2), ht, 2);
        wsec(0x2c, ht_base, 38, lmtc(2), vht, 3); /* VHT1SS: HT base/lmt */
        _txagc_shadow_cck = sec_idx(cck_base, 50, lmtc(0), cck[0], 0);
        _txagc_shadow_ofdm = sec_idx(ofdm_base, 40, lmtc(1), ofdm[0], 0);
        _txagc_shadow_mcs7 = sec_idx(ht_base, 38, lmtc(2), ht[1], 3);
      } else {
        const uint32_t ofdm[] = {0x34343434, 0x26283032};
        const uint32_t ht[] = {0x32343434, 0x24262830};
        const uint32_t vht[] = {0x32343434, 0x24262830, 0x20202022};
        wsec(0x04, ofdm_base, 38, lmtc(1), ofdm, 2);
        wsec(0x0c, ht_base, 36, lmtc(2), ht, 2);
        wsec(0x2c, ht_base, 36, lmtc(2), vht, 3);
        _txagc_shadow_cck = -1; /* no CCK at 5G */
        _txagc_shadow_ofdm = sec_idx(ofdm_base, 38, lmtc(1), ofdm[0], 0);
        _txagc_shadow_mcs7 = sec_idx(ht_base, 36, lmtc(2), ht[1], 3);
      }
      _logger->info("Jaguar2/8821C: per-rate TXAGC (vendor formula) ch{} {} "
                    "cck_base={} ofdm_base={} ht_base={} lmt cck/ofdm/ht={}/{}/{}",
                    channel, g5c ? "5G" : "2.4G", cck_base, ofdm_base, ht_base,
                    lmtc(0), lmtc(1), lmtc(2));
      return;
    }
    /* unprogrammed efuse -> shared flat path below */
  }
  /* Both variants use the same EFUSE power-by-rate layout at pg_txpwr_saddr=0x10
   * (rtl8821c_halinit.c:61 == the 8822B value). The 8821C is 1T1R, so its path-1
   * EFUSE base reads 0xff and is skipped by the per-path loop below — the shared
   * code reads the real path-A per-channel base, adds the section BW/rate diffs,
   * and clamps to the per-chip txpwr_lmt (8821C table wired into `lmt` above).
   * This replaces the earlier 8821C-only branch that ignored the EFUSE base. */
  const bool g5 = channel > 14;

  /* EFUSE power-by-rate from logical 0x10 (pg_txpwr_saddr). Per path: 2.4G block
   * 18 B then 5G block 24 B (42 B/path). Path p: 2G @ 0x10+p*42, 5G @ 0x22+p*42.
   * Reuse the map read once by read_efuse_rfe earlier in bring_up. */
  if (!_efuse_valid) {
    read_efuse_logical_map(_efuse_map, sizeof(_efuse_map), /*dump=*/false);
    _efuse_valid = true;
  }
  const uint8_t *map = _efuse_map;

  /* channel -> rate group (rtw_get_ch_group). */
  auto group_2g = [](uint8_t ch) -> uint8_t {
    if (ch <= 2) return 0; if (ch <= 5) return 1; if (ch <= 8) return 2;
    if (ch <= 11) return 3; return 4;
  };
  auto group_5g = [](uint8_t ch) -> uint8_t {
    if (ch <= 42) return 0; if (ch <= 48) return 1; if (ch <= 58) return 2;
    if (ch <= 80) return 3; if (ch <= 106) return 4; if (ch <= 114) return 5;
    if (ch <= 122) return 6; if (ch <= 130) return 7; if (ch <= 138) return 8;
    if (ch <= 144) return 9; if (ch <= 155) return 10; if (ch <= 161) return 11;
    if (ch <= 171) return 12; return 13;
  };
  const uint8_t group = g5 ? group_5g(channel) : group_2g(channel);
  const uint8_t cck_group = (channel == 14) ? 5 : group;

  auto s4 = [](uint8_t nib) -> int { /* s4bit -> s8 sign-extend */
    int v = nib & 0x0f; return (v & 0x8) ? (v - 16) : v;
  };
  /* Regulatory limits — exact per (rfe_type, band, bw, section, ntx, channel)
   * from the generated txpwr_lmt table (worldwide-min across FCC/ETSI/MKK; the
   * rfe_type-3 table for the T3U). Keeps high-order QAM out of PA compression.
   * The demo path is 1Tx; the ht2 (2SS) clamp reuses the 1Tx limit (conservative
   * for the rare 2SS injection). sec: 0=CCK 1=OFDM 2=HT. */
  const uint8_t band_i = g5 ? 1 : 0;
  /* Per-chip regulatory limit lookup (worldwide-min across FCC/ETSI/MKK, in
   * TXAGC-index units). 8822B uses the rfe_type-aware table; 8821C the packed
   * txpwr_lmt_t_8821c[] (rfe-independent). sec: 0=CCK 1=OFDM 2=HT. */
  auto lmt = [&](uint8_t sec) -> int {
#if defined(DEVOURER_HAVE_JAGUAR2_8822B)
    if (_variant == ChipVariant::C8822B)
      return hal8822b_txpwr_lmt(rfe_type, band_i, bw, sec, 1, channel);
#endif
#if defined(DEVOURER_HAVE_JAGUAR2_8821C)
    if (_variant == ChipVariant::C8821C)
      return hal8821c_txpwr_lmt(band_i, bw, sec, 1, channel);
#endif
    (void)sec;
    return 63;
  };
  const int lmt_cck = g5 ? 63 : lmt(0);
  const int lmt_ofdm = lmt(1);
  const int lmt_ht = lmt(2);
  auto clamp63 = [&](int v) -> uint8_t {
    if (v < 0) { v = 0; _txpwr_sat_low = true; }
    if (v > 63) { v = 63; _txpwr_sat_high = true; }
    return static_cast<uint8_t>(v);
  };
  /* min() against the regulatory table FIRST, then the runtime offset, then
   * the hw rail — a positive offset may exceed the worldwide-min table
   * (operator's call, same as the flat override), a negative one always
   * takes effect. */
  auto clamp_lmt = [&](int v, int lmt) -> uint8_t {
    if (v > lmt) v = lmt; return clamp63(v + offset_steps);
  };

  /* 8821C is 1T1R — only path A has a valid EFUSE base + a real RF path (its
   * path-1 block bytes are not tx-power data and 0x1d80 has no path-B PA). */
  const uint8_t npath = _variant == ChipVariant::C8821C ? 1 : 2;
  for (uint8_t path = 0; path < npath; path++) {
    /* base offset of this path's band block, and the BW40-base / first-diff
     * byte offsets within it. 2.4G: 6 CCK-base + 5 BW40-base + diffs (diff@11).
     * 5G: 14 BW40-base + diffs (diff@14), no CCK. */
    const uint16_t blk = static_cast<uint16_t>(0x10 + path * 42 + (g5 ? 18 : 0));
    const uint8_t cck_base = g5 ? 0 : map[blk + cck_group];
    const uint8_t bw40_base = g5 ? map[blk + group] : map[blk + 6 + group];
    if (bw40_base == 0xFF || (!g5 && cck_base == 0xFF)) {
      if (offset_steps != 0)
        _logger->warn("Jaguar2: TX-power offset has no EFUSE baseline to fold "
                      "into on path {} (unprogrammed) — TXAGC left at BB-table "
                      "default", path);
      else
        _logger->info("Jaguar2: apply_tx_power path {} ch{} EFUSE base unprogrammed "
                      "— leaving TXAGC default", path, channel);
      continue;
    }
    const uint8_t d0 = map[blk + (g5 ? 14 : 11)]; /* MSB BW20_Diff0 / LSB OFDM_Diff0 */
    const uint8_t d1 = map[blk + (g5 ? 15 : 12)]; /* LSB BW20_Diff1 (2nd Tx) */
    const int ofdm_diff0 = s4(d0 & 0x0f);
    const int bw20_diff0 = s4((d0 >> 4) & 0x0f);
    const int bw20_diff1 = s4(d1 & 0x0f);

    /* HT bandwidth diff (phy_get_pg_txpwr_idx): BW20 = BW40_base + BW20_Diff;
     * BW40 = BW40_base + BW40_Diff (BW40_Diff[1Tx] is the reference = 0, i.e. the
     * base is the BW40 value); BW80 (5G) = BW40_base + BW80_Diff. Only BW20 adds
     * a diff for the 1Tx path — so BW40/80 use the raw base. (BW80's exact 5G
     * BW80_Diff is a further refinement; the base is a close conservative value.) */
    const int ht_diff0 = (bw == 0) ? bw20_diff0 : 0;
    const int ht_diff1 = (bw == 0) ? bw20_diff1 : 0;
    const uint8_t cck_idx = clamp_lmt(cck_base, lmt_cck);
    const uint8_t ofdm_idx = clamp_lmt(bw40_base + ofdm_diff0, lmt_ofdm);
    const uint8_t ht1_idx = clamp_lmt(bw40_base + ht_diff0, lmt_ht);
    const uint8_t ht2_idx = clamp_lmt(bw40_base + ht_diff0 + ht_diff1, lmt_ht);

    /* Write TXAGC (4 rates / dword). hw_rate: 0x00-03 CCK, 0x04-0b OFDM,
     * 0x0c-13 HT MCS0-7 (1SS), 0x14-1b HT MCS8-15 (2SS). 5G has no CCK. */
    const uint16_t p = static_cast<uint16_t>(0x1d00 + path * 0x80);
    auto wr = [&](uint16_t off, uint8_t idx) {
      _device.rtw_write32(static_cast<uint16_t>(p + off), idx * 0x01010101u);
    };
    if (!g5)
      wr(0x00, cck_idx);
    wr(0x04, ofdm_idx); wr(0x08, ofdm_idx);
    wr(0x0c, ht1_idx);  wr(0x10, ht1_idx);
    wr(0x14, ht2_idx);  wr(0x18, ht2_idx);
    if (path == 0) { /* representative shadow — the block is write-only */
      _txagc_shadow_cck = g5 ? -1 : cck_idx;
      _txagc_shadow_ofdm = ofdm_idx;
      _txagc_shadow_mcs7 = ht1_idx; /* MCS7 = hw_rate 0x13, HT1SS section */
    }
    /* VHT1SS — the 8821C's AC mode — uses the same 1SS BW base as HT1SS, and the
     * vendor efuse-calibrates it (config_phydm_write_txagc_8821c writes hw_rate
     * up to VHT; 0x2c-0x35 = VHT1SS MCS0-9 -> regs 0x1d2c/0x1d30/0x1d34). Without
     * this VHT TXAGC stays at the BB-table default — uncalibrated and
     * inconsistent with the CCK/OFDM/HT rates above. */
    if (_variant == ChipVariant::C8821C) {
      wr(0x2c, ht1_idx); wr(0x30, ht1_idx); wr(0x34, ht1_idx);
    }
    /* VHT (8822B, 2SS): vendor phy_get_pg_txpwr_idx computes the PG index by
     * rate-section STREAM COUNT only — VHT1SS rides the same base+BW20-diff as
     * HT1SS, VHT2SS the same as HT2SS — clamped to the VHT regulatory section
     * (sec 3). hw_rate 0x2c-0x35 = VHT1SS MCS0-9, 0x36-0x3f = VHT2SS MCS0-9;
     * dword 0x34 straddles the sections (bytes 0-1 = 1SS MCS8/9, 2-3 = 2SS
     * MCS0/1). Before this, 8822B VHT rates rode the hot BB-table default —
     * uncalibrated, regulatory-unclamped, and out of reach of the runtime
     * TX-power offset. */
    if (_variant == ChipVariant::C8822B) {
      /* The worldwide-min table has no 2.4G VHT rows (non-standard mode), so
       * the sec-3 lookup falls back to 63 there — which would let 2.4G VHT
       * ride hotter than the HT clamp. Bound it by the HT limit instead;
       * 5G has real VHT rows and uses them. */
      int lmt_vht = lmt(3);
      if (!g5 && lmt_vht > lmt_ht)
        lmt_vht = lmt_ht;
      const uint8_t v1 = clamp_lmt(bw40_base + ht_diff0, lmt_vht);
      const uint8_t v2 = clamp_lmt(bw40_base + ht_diff0 + ht_diff1, lmt_vht);
      wr(0x2c, v1); wr(0x30, v1);
      _device.rtw_write32(static_cast<uint16_t>(p + 0x34),
                          static_cast<uint32_t>(v1) |
                              (static_cast<uint32_t>(v1) << 8) |
                              (static_cast<uint32_t>(v2) << 16) |
                              (static_cast<uint32_t>(v2) << 24));
      wr(0x38, v2); wr(0x3c, v2);
      _logger->info("Jaguar2/8822B: VHT TXAGC path {}: VHT1SS={:#x} VHT2SS={:#x} "
                    "(lmt_vht={})", path, v1, v2, lmt_vht);
    }

    _logger->info("Jaguar2: TXAGC path {} ch{} {} (g{}): CCK={:#x} OFDM={:#x} "
                  "HT1SS={:#x} HT2SS={:#x} (bw40_base={})", path, channel,
                  g5 ? "5G" : "2G", group, cck_idx, ofdm_idx, ht1_idx, ht2_idx,
                  bw40_base);
  }
}

void HalJaguar2::dig_step() {
  /* Per-window false-alarm counts (accumulated since the last reset below). */
  uint32_t ofdm_fa = _device.rtw_read32(0x0f48) & 0xffff;
  uint32_t cck_fa = _device.rtw_read32(0x0a5c) & 0xffff;
  uint32_t fa = ofdm_fa + cck_fa;
  _last_fa = fa;

  /* Frame-free energy snapshot for GetRxEnergy() — the CCA (channel-busy) count
   * (0x0f08[31:16]=OFDM, [15:0]=CCK) is the primary in-band-energy signal (a CW
   * tone spikes OFDM CCA), captured over the same window as the FA counts just
   * before the reset. IGI is stored below. */
  uint32_t cca = _device.rtw_read32(0x0f08);
  _energy.fa_ofdm = ofdm_fa;
  _energy.fa_cck = cck_fa;
  _energy.cca_ofdm = (cca >> 16) & 0xffff;
  _energy.cca_cck = cca & 0xffff;
  _energy.valid_fa = true;

  /* Reset the hold-type FA/CCA counters so the next window is a fresh delta
   * (11AC phydm_reset_bb_hw_cnt path): OFDM-FA 0x9a4[17] (1->0 = reset->enable),
   * CCK-FA 0xa2c[15] (0->1), CCA 0xb58[0] (1->0). */
  _device.phy_set_bb_reg(0x09a4, 1u << 17, 1);
  _device.phy_set_bb_reg(0x09a4, 1u << 17, 0);
  _device.phy_set_bb_reg(0x0a2c, 1u << 15, 0);
  _device.phy_set_bb_reg(0x0a2c, 1u << 15, 1);
  _device.phy_set_bb_reg(0x0b58, 1u << 0, 1);
  _device.phy_set_bb_reg(0x0b58, 1u << 0, 0);

  uint8_t igi = static_cast<uint8_t>(_device.rtw_read8(0x0c50) & 0x7f);
  _energy.igi = igi;
  _energy.valid_igi = true;
  uint8_t ni = igi;
  if (fa > 750)
    ni = static_cast<uint8_t>(igi + 2);
  else if (fa > 500)
    ni = static_cast<uint8_t>(igi + 1);
  else if (fa < 250)
    ni = static_cast<uint8_t>(igi >= 2 ? igi - 2 : igi);
  if (ni < 0x1c)
    ni = 0x1c;
  if (ni > 0x3e)
    ni = 0x3e;
  if (ni != igi) {
    _device.phy_set_bb_reg(0x0c50, 0x7f, ni);
    _device.phy_set_bb_reg(0x0e50, 0x7f, ni);
  }
}

void HalJaguar2::enable_rx() {
  /* CR (0x100) full MAC enable: TRX-DMA | PROTOCOL | SCHEDULE | MACTX | MACRX
   * (+ENSWBCN), matching the jaguar3 RX-enable value 0x06FF. init_mac_cfg only
   * set the DMA bits; without MACRXEN (BIT7) the MAC RX engine never runs. */
  _device.rtw_write16(0x0100, 0x06FF);
  /* Promiscuous RX for monitor: the vendor monitor RCR (hal_com.c:
   * RCR_AAP|APM|AM|AB|APWRMGT|APP_PHYST_RXFF|APP_MIC|APP_ICV = 0x7000002F)
   * WITHOUT the legacy "accept" trio at bits 11/12/13. On this MAC generation
   * those are NOT the Jaguar1 ADF/ACF/AMF accept bits — rtw88 reg.h names
   * BIT(11) BIT_TA_BCN (TA-gated beacon accept) and BIT(12)
   * BIT_RPFM_CAM_ENABLE — and with BIT(11) set and no TA programmed the WMAC
   * drops EVERY ambient beacon / management frame. Hardware-bisected on all
   * three Jaguar2 parts (2026-07-07): 0x7000382F -> 0 ambient beacons on the
   * RTL8821CE (PCIe), RTL8811CU and RTL8822BU (USB); with the bits cleared
   * each receives ambient beacons at the kernel driver's rate, and ctrl/data
   * delivery is unchanged — so the bits gate nothing monitor RX needs
   * (rtw88's own default RCR never sets them). Injected canonical-SA beacons
   * passed even WITH the bits set, which is how regress.py stayed green while
   * every real AP's beacons were silently dropped. */
  uint32_t rcr = 0xF000002Fu;
  /* 8821C: drop APP_PHYST_RXFF (BIT28). The 8821C appends a 32-byte PHY-status
   * block before each RX frame in the RXFF, but its RX descriptor reports
   * drv_info_size=0 (unlike the 8822B, which counts it) — so the shared parser
   * puts the frame pointer 32 bytes early (onto the phy-status) and mis-advances
   * the aggregation, garbling every frame. Monitor RX doesn't need the phy-
   * status, so clearing the append bit makes the frame sit cleanly at
   * descriptor+drvinfo(0)+shift and decode. */
  if (_variant == ChipVariant::C8821C) {
    if (_cfg.rx.phy_status_8821c) {
      /* cfg_drv_info_8821c(HALMAC_DRV_INFO_PHY_STATUS): keep RCR APP_PHYSTS
       * (bit28, like the vendor monitor path) and make the chip prepend a
       * 32-byte PHY-status (jgr2 type0/type1) as drvinfo AND — crucially — count
       * it in the RX descriptor's drv_info_size. REG_TRXFF_BNDY+1 low-nibble =
       * 0xF is the vendor's "rxdesc len = 0 issue" fix, without which the
       * descriptor reports drv_info_size=0 despite PHYST=1 and the shared parser
       * mis-locates the body (garbling every frame). With it, drv_info_size
       * reads 32, the body sits cleanly at desc+32+shift, and the phy-status
       * carries per-frame RSSI/SNR/EVM (FrameParserJaguar2 fills RxAtrib).
       * DEVOURER_8821C_NO_PHYST drops the phy-status for the leanest RX. */
      _device.rtw_write8(0x060F, 4); /* REG_RX_DRVINFO_SZ = 4 units = 32 B */
      uint8_t v = _device.rtw_read8(0x0115);
      _device.rtw_write8(0x0115, static_cast<uint8_t>((v & 0xF0) | 0x0F));
    } else {
      rcr &= ~(1u << 28);
    }
  }
  /* DEVOURER_RX_KEEP_CORRUPTED: also accept CRC32/ICV-error frames (ACRC32 BIT8,
   * AICV BIT9) so the BB's demodulated-but-failed frames still reach the host.
   * Doubles as a bring-up discriminator: if reads>0 with this set but 0 without,
   * MAC->USB delivery works and only clean-decode is marginal. */
  if (_cfg.rx.keep_corrupted)
    rcr |= (1u << 8) | (1u << 9);
  _device.rtw_write32(0x0608, rcr);

  /* Interim fixed IGI (initial gain) until the phydm DIG thread is ported: the
   * BB/AGC table default leaves IGI too low, so the RX drowns in false alarms
   * (~4000/s) and rarely decodes a clean frame. IGI 0x40 drops the FA rate ~7x
   * and yields clean OFDM CRC-OK frames. */
  const uint8_t igi = _cfg.rx.igi.value_or(0x40) & 0x7f;
  _device.phy_set_bb_reg(0x0c50, 0x7f, igi);
  _device.phy_set_bb_reg(0x0e50, 0x7f, igi);
  _logger->info("Jaguar2: RX enabled (CR=0x06ff, RCR=0x{:08x}, IGI=0x{:02x})",
                rcr, igi);
}

} /* namespace jaguar2 */
