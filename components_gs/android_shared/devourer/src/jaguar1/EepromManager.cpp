#include "EepromManager.h"

#include "InitTimer.h"
#include "phydm_pre_define.h"
#include "registry_priv.h"
#include "rtl8812a_hal.h"
#include "rtw_efuse.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

/* Cross-platform case-insensitive compare. POSIX has `strcasecmp` in
 * <strings.h>; MSVC has `_stricmp`; doing it by hand keeps the include
 * surface portable. */
static int devourer_strcaseeq(const char *a, const char *b) {
  while (*a && *b) {
    if (std::tolower(static_cast<unsigned char>(*a)) !=
        std::tolower(static_cast<unsigned char>(*b)))
      return 0;
    a++;
    b++;
  }
  return *a == *b;
}

EepromManager::EepromManager(RtlAdapter device, Logger_t logger,
                             const devourer::DeviceConfig &cfg)
    : _device{device}, _logger{logger}, _tuning{cfg.tuning},
      _log_txpwr{cfg.debug.log_txpwr} {
  InitTimer timer(logger, "eeprom");
  read_chip_version_8812a(device);
  timer.stage("chip_version");

  /* On 8814AU, defer all EFUSE access until AFTER firmware download. rtw88's
   * usbmon shows zero touches to EFUSE_CTRL (0x0031-0x0033) and EFUSE_ACCESS
   * (0x00CF) before fwdl — these are post-fw-boot operations. Reading EFUSE
   * pre-fwdl appears to put the chip in a state where it never sets BIT15 of
   * REG_FIFOPAGE_CTRL_2 (BCN_VALID) after a beacon-queue bulk OUT, which
   * blocks the IDDMA copy that loads fw into 8051 DMEM/IMEM. For the
   * experiment, pretend the EFUSE autoload failed so the parsers fall through
   * to safe defaults; the parsers still run against in-memory shadow data
   * (no chip access). */
  if (version_id.ICType == CHIP_8814A) {
    std::memset(efuse_eeprom_data, 0xFF, sizeof(efuse_eeprom_data));
    _device.AutoloadFailFlag = true;
  } else {
    hal_InitPGData_8812A();
  }
  timer.stage("efuse_read");

  Hal_EfuseParseIDCode8812A();
  EEPROMVersion = Hal_ReadPROMVersion8812A(_device, efuse_eeprom_data);
  EEPROMRegulatory = Hal_ReadTxPowerInfo8812A(_device, efuse_eeprom_data);
  /* T1: populate the per-channel per-path TX-power tables from EFUSE so
   * RadioManagementModule can compute per-rate TX power instead of using
   * the uniform `SetTxPower(N)` shortcut. */
  LoadTxPowerInfo();
  LoadTxPowerByRate();
  LoadTxPowerLimit();

  /*  */
  /* Read Bluetooth co-exist and initialize */
  /*  */
  Hal_EfuseParseBTCoexistInfo8812A();

  Hal_EfuseParseXtal_8812A();
  Hal_ReadThermalMeter_8812A();

  Hal_ReadAmplifierType_8812A();
  Hal_ReadRFEType_8812A();

#if 0
  // EEPROMUsbSwitch not used in our code
  pHalData.EEPROMUsbSwitch = ReadUsbModeSwitch8812AU(pHalData.efuse_eeprom_data,
                                                     pHalData.AutoloadFailFlag);
  RTW_INFO("Usb Switch: %d", pHalData.EEPROMUsbSwitch);
#endif

  /* 2013/04/15 MH Add for different board type recognize. */
  if (version_id.ICType != CHIP_8814A) {
    hal_ReadUsbType_8812AU();
  }
  timer.stage("parse");
  timer.total();
}

void EepromManager::LateInitFor8814A() {
  /* Re-read EFUSE properly now that firmware is running. The constructor's
   * 8814 branch sets AutoloadFailFlag=true + 0xFF-fills the shadow map, so
   * the parsers ran against defaults (rfe_type=0, all PA/LNA=0). With fw up
   * the chip will accept EFUSE reads without breaking, and we can use the
   * real RFE/PA/LNA values for the BB/AGC/RF tables which gate on them. */
  if (version_id.ICType != CHIP_8814A) {
    return;
  }
  InitTimer timer(_logger, "eeprom_late");
  _device.AutoloadFailFlag = false;
  hal_InitPGData_8812A();
  timer.stage("efuse_read");
  Hal_EfuseParseIDCode8812A();
  EEPROMVersion = Hal_ReadPROMVersion8812A(_device, efuse_eeprom_data);
  EEPROMRegulatory = Hal_ReadTxPowerInfo8812A(_device, efuse_eeprom_data);
  /* T1: populate the per-channel per-path TX-power tables from EFUSE so
   * RadioManagementModule can compute per-rate TX power instead of using
   * the uniform `SetTxPower(N)` shortcut. */
  LoadTxPowerInfo();
  Hal_EfuseParseBTCoexistInfo8812A();
  Hal_EfuseParseXtal_8812A();
  Hal_ReadThermalMeter_8812A();
  /* 8814 derives amplifier state from rfe_type (kernel
   * hal_ReadRFEType_8814A -> hal_ReadAmplifierType_8814A) — the 8812
   * pair (EFUSE-parsed amplifier + 8812 RFE heuristic) resolved the
   * wrong fallback (0 instead of 1) on unburnt 0xCA boards. */
  Hal_ReadRFEType_8814A();
  _logger->info("8814A LateInit: rfe_type={} crystal_cap=0x{:X} "
                "PA_2G/5G=0x{:X}/0x{:X} LNA_2G/5G=0x{:X}/0x{:X}",
                rfe_type, crystal_cap, PAType_2G, PAType_5G,
                LNAType_2G, LNAType_5G);
  timer.stage("parse");
  timer.total();
}

/* RTL8821AU is Jaguar-family silicon (CHIP_8821 = 7 in Realtek's HalVerDef),
 * 1T1R AC + BT combo. It responds to REG_SYS_CFG with an 8812-shaped value
 * (low byte 0x35), so plain SYS_CFG-bit inspection misidentifies it as 8812.
 * Distinguish by VID:PID at probe time. Genuine 8821AU SKUs we know about:
 *  - TP-Link Archer T2U Plus / T4U / etc:  2357:0120 / 011E / 0122
 *  - Realtek reference PIDs:               0bda:0820 / 0821 / 0823 / 8822
 *  - OEM rebadges (Senao, ASUS, Edimax, D-Link, Hawking, …):
 *    0411:0242 / 029B   (Senao)
 *    0846:9052          (ASUS)
 *    0e66:0023          (Hawking)
 *    2001:3314 / 3318   (D-Link)
 *    20f4:804b          (TRENDnet)
 *    7392:a811/a812/a813/b611 (Edimax)
 *    3823:6249          (HP/MaxNet)
 *    2019:ab32          (Planex)
 *    04bb:0953          (I-O Data)
 *    056e:4007 / 400e / 400f (ELECOM)
 *
 * NOT included on purpose:
 *  - 0bda:0811 / a811 / b811  — those are 8811AU (1T1R cut of 8812 silicon);
 *    they ride on CHIP_8812 + RFType=1T1R, NOT through the 8821 HAL.
 *  - 0bda:c811  — RTL8811CU, unrelated silicon, not supported here. */
static bool is_rtl8821a_pid(uint16_t vid, uint16_t pid) {
  const uint32_t key = (static_cast<uint32_t>(vid) << 16) | pid;
  switch (key) {
  case 0x0BDA0820:
  case 0x0BDA0821:
  case 0x0BDA0823:
  case 0x0BDA8822:
  case 0x0411'0242:
  case 0x0411'029B:
  case 0x04BB'0953:
  case 0x056E'4007:
  case 0x056E'400E:
  case 0x056E'400F:
  case 0x0846'9052:
  case 0x0E66'0023:
  case 0x2001'3314:
  case 0x2001'3318:
  case 0x2019'AB32:
  case 0x20F4'804B:
  case 0x2357'011E:
  case 0x2357'0120:
  case 0x2357'0122:
  case 0x3823'6249:
  case 0x7392'A811:
  case 0x7392'A812:
  case 0x7392'A813:
  case 0x7392'B611:
    return true;
  default:
    return false;
  }
}

void EepromManager::read_chip_version_8812a(RtlAdapter device) {
  uint32_t value32 = device.rtw_read32(REG_SYS_CFG);
  _logger->info("read_chip_version_8812a SYS_CFG(0x{:X})=0x{:08X}", REG_SYS_CFG,
                value32);

  const uint16_t pid = device.idProduct();
  const uint16_t vid = device.idVendor();
  const bool is_8814a = (pid == 0x8813);
  const bool is_8821 = !is_8814a && is_rtl8821a_pid(vid, pid);

  HAL_IC_TYPE_E ic_type;
  if (is_8814a) {
    ic_type = CHIP_8814A;
  } else if (is_8821) {
    ic_type = CHIP_8821;
  } else {
    ic_type = CHIP_8812;
  }

  /* RFType:
   *   8814AU      → 4T4R RF (baseband caps at 3 spatial streams)
   *   8821AU      → always 1T1R (Jaguar-family AC+BT combo, single chain)
   *   8812/8811AU → SYS_CFG bit 27 (RF_TYPE_ID): set on 1T1R, clear on 2T2R */
  HAL_RF_TYPE_E rf_type;
  if (is_8814a) {
    rf_type = RF_TYPE_4T4R;
  } else if (is_8821) {
    rf_type = RF_TYPE_1T1R;
  } else {
    rf_type =
        (value32 & RF_TYPE_ID) ? RF_TYPE_1T1R : RF_TYPE_2T2R;
  }

  version_id = {
      .ICType = ic_type,
      .ChipType = (value32 & RTL_ID) ? TEST_CHIP : NORMAL_CHIP,
      .VendorType = (value32 & VENDOR_ID) ? CHIP_VENDOR_UMC : CHIP_VENDOR_TSMC,
      .RFType = rf_type,
  };

  /* Manual override: force 1T1R even when SYS_CFG reports 2T2R
   * (useful if EFUSE/strap is mis-burnt on a 1T1R board). Only meaningful
   * for the 8812-family; 8814AU keeps its 4T4R RFType, 8821AU is already 1T1R. */
  if (!is_8814a && !is_8821 && registry_priv::special_rf_path == 1) {
    version_id.RFType = RF_TYPE_1T1R;
  }

  /* IC version (CUT). Upstream rtw88's rtw88xxa_read_efuse does `cut_version
   * += 1` ONLY for 8812A (chip->id == RTW_CHIP_TYPE_8812A) — for 8814A the
   * raw chip-reported value is used as-is. Our previous unconditional +1
   * was breaking PhyTableLoader's check_positive matching on 8814 (the
   * "cut_for_para" byte at c1[27:24] never matched the table's expected
   * value), which is why AGC_TBL_081C reads back as 0 (not loaded) on our
   * chip while rtw88's same chip shows 0x017e0303. */
  const uint32_t raw_cut =
      (value32 & CHIP_VER_RTL_MASK) >> CHIP_VER_RTL_SHIFT;
  version_id.CUTVersion =
      (HAL_CUT_VERSION_E)(is_8814a ? raw_cut : (raw_cut + 1));

  version_id.ROMVer = 0; /* ROM code version. */

  rtw_hal_config_rftype();

  dump_chip_info(version_id);
}

/* Includes for the embedded upstream tables. */
#include "../hal/Hal8812a_PhyRegPg.h"
#include "../hal/Hal8812a_TxpwrLmt.h"

namespace {

/* Rate → flat-index map for TxPwrByRateOffset. Mirrors upstream
 * `PHY_GetRateIndexOfTxPowerByRate`. -1 = rate not in table. */
int8_t rate_to_idx(uint8_t rate) {
  switch (rate) {
  /* CCK */
  case 0x02: return 0;  /* MGN_1M */
  case 0x04: return 1;  /* MGN_2M */
  case 0x0B: return 2;  /* MGN_5_5M */
  case 0x16: return 3;  /* MGN_11M */
  /* OFDM */
  case 0x0C: return 4;  /* MGN_6M */
  case 0x12: return 5;  /* MGN_9M */
  case 0x18: return 6;  /* MGN_12M */
  case 0x24: return 7;  /* MGN_18M */
  case 0x30: return 8;  /* MGN_24M */
  case 0x48: return 9;  /* MGN_36M */
  case 0x60: return 10; /* MGN_48M */
  case 0x6C: return 11; /* MGN_54M */
  default:
    /* MGN_MCS0..31 = 0x80..0x9F → idx 12..43. */
    if (rate >= 0x80 && rate <= 0x9F)
      return static_cast<int8_t>(12 + (rate - 0x80));
    /* MGN_VHT1SS_MCS0..VHT4SS_MCS9 = 0xA0..0xC7 → idx 44..83. */
    if (rate >= 0xA0 && rate <= 0xC7)
      return static_cast<int8_t>(44 + (rate - 0xA0));
    return -1;
  }
}

/* RATE_SECTION enum from upstream `phydm_pre_define.h`. */
enum { RS_CCK = 0, RS_OFDM, RS_HT_1SS, RS_HT_2SS, RS_HT_3SS, RS_HT_4SS,
       RS_VHT_1SS, RS_VHT_2SS, RS_VHT_3SS, RS_VHT_4SS };

/* (RegAddr → 4 rates) port of upstream `PHY_GetRateValuesOfTxPowerByRate`.
 * Only the regs that appear in `kHal8812aPhyRegPg` for 8812 are mapped. */
struct RateRow {
  uint16_t reg;
  uint8_t rates[4];
  uint8_t count;
};
static const RateRow kRateMap[] = {
    /* path-A */
    {0xc20, {0x02, 0x04, 0x0B, 0x16}, 4}, /* CCK 1/2/5.5/11 */
    {0xc24, {0x0C, 0x12, 0x18, 0x24}, 4}, /* OFDM 6/9/12/18 */
    {0xc28, {0x30, 0x48, 0x60, 0x6C}, 4}, /* OFDM 24/36/48/54 */
    {0xc2c, {0x80, 0x81, 0x82, 0x83}, 4}, /* MCS0..3 */
    {0xc30, {0x84, 0x85, 0x86, 0x87}, 4}, /* MCS4..7 */
    {0xc34, {0x88, 0x89, 0x8A, 0x8B}, 4}, /* MCS8..11 */
    {0xc38, {0x8C, 0x8D, 0x8E, 0x8F}, 4}, /* MCS12..15 */
    {0xc3c, {0xA0, 0xA1, 0xA2, 0xA3}, 4}, /* VHT1SS_MCS0..3 */
    {0xc40, {0xA4, 0xA5, 0xA6, 0xA7}, 4}, /* VHT1SS_MCS4..7 */
    {0xc44, {0xA8, 0xA9, 0xAA, 0xAB}, 4}, /* VHT1SS_MCS8,9, VHT2SS_MCS0,1 */
    {0xc48, {0xAC, 0xAD, 0xAE, 0xAF}, 4}, /* VHT2SS_MCS2..5 */
    {0xc4c, {0xB0, 0xB1, 0xB2, 0xB3}, 4}, /* VHT2SS_MCS6..9 */
    /* path-B */
    {0xe20, {0x02, 0x04, 0x0B, 0x16}, 4},
    {0xe24, {0x0C, 0x12, 0x18, 0x24}, 4},
    {0xe28, {0x30, 0x48, 0x60, 0x6C}, 4},
    {0xe2c, {0x80, 0x81, 0x82, 0x83}, 4},
    {0xe30, {0x84, 0x85, 0x86, 0x87}, 4},
    {0xe34, {0x88, 0x89, 0x8A, 0x8B}, 4},
    {0xe38, {0x8C, 0x8D, 0x8E, 0x8F}, 4},
    {0xe3c, {0xA0, 0xA1, 0xA2, 0xA3}, 4},
    {0xe40, {0xA4, 0xA5, 0xA6, 0xA7}, 4},
    {0xe44, {0xA8, 0xA9, 0xAA, 0xAB}, 4},
    {0xe48, {0xAC, 0xAD, 0xAE, 0xAF}, 4},
    {0xe4c, {0xB0, 0xB1, 0xB2, 0xB3}, 4},
};

/* Section base rate index — port of upstream `rate_sec_base[RATE_SECTION_NUM]`
 * (the rate whose value becomes the section's base for offset normalization). */
static const uint8_t kSectionBaseRate[10] = {
    0x16, /* RS_CCK   → MGN_11M */
    0x6C, /* RS_OFDM  → MGN_54M */
    0x87, /* RS_HT_1SS → MGN_MCS7 */
    0x8F, /* RS_HT_2SS → MGN_MCS15 */
    0x97, /* RS_HT_3SS → MGN_MCS23 */
    0x9F, /* RS_HT_4SS → MGN_MCS31 */
    0xA7, /* RS_VHT_1SS → MGN_VHT1SS_MCS7 */
    0xB1, /* RS_VHT_2SS → MGN_VHT2SS_MCS7 */
    0xBB, /* RS_VHT_3SS → MGN_VHT3SS_MCS7 */
    0xC5, /* RS_VHT_4SS → MGN_VHT4SS_MCS7 */
};

int8_t rate_to_section(uint8_t rate) {
  if (rate == 0x02 || rate == 0x04 || rate == 0x0B || rate == 0x16) return RS_CCK;
  if (rate >= 0x0C && rate <= 0x6C) return RS_OFDM;
  if (rate >= 0x80 && rate <= 0x87) return RS_HT_1SS;
  if (rate >= 0x88 && rate <= 0x8F) return RS_HT_2SS;
  if (rate >= 0x90 && rate <= 0x97) return RS_HT_3SS;
  if (rate >= 0x98 && rate <= 0x9F) return RS_HT_4SS;
  if (rate >= 0xA0 && rate <= 0xA9) return RS_VHT_1SS;
  if (rate >= 0xAA && rate <= 0xB3) return RS_VHT_2SS;
  if (rate >= 0xB4 && rate <= 0xBD) return RS_VHT_3SS;
  if (rate >= 0xBE && rate <= 0xC7) return RS_VHT_4SS;
  return -1;
}

/* Channel → index in the per-band TX-power-limit table (port of upstream
 * `phy_GetChannelIndexOfTxPowerLimit`). For 2.4G it's channel-1 (1..14).
 * For 5G it's the index in the 65-entry center_ch_5g_all table. */
int8_t lmt_ch_idx_2g(uint8_t ch) {
  if (ch >= 1 && ch <= 14) return static_cast<int8_t>(ch - 1);
  return -1;
}
/* The 5G channel index table is defined as a file-scope static below
 * (`kCenterCh5gAll`). lmt_ch_idx_5g uses the same definition via a small
 * inline lookup in `LoadTxPowerLimit` rather than re-declaring it here. */

uint8_t parse_decimal(const char *s) {
  unsigned r = 0;
  while (*s >= '0' && *s <= '9') { r = r * 10 + (*s - '0'); s++; }
  return static_cast<uint8_t>(r);
}

}  /* namespace */

/* Helper: 4-bit signed nibble (Realtek's PG diff encoding) to int8_t. */
static inline int8_t pg_msb_diff(uint8_t v) {
  uint8_t n = (v >> 4) & 0x0f;
  return static_cast<int8_t>((n & 0x08) ? (n | 0xf0) : n);
}
static inline int8_t pg_lsb_diff(uint8_t v) {
  uint8_t n = v & 0x0f;
  return static_cast<int8_t>((n & 0x08) ? (n | 0xf0) : n);
}

/* Per-channel group classifier — port of upstream `rtw_get_ch_group` in
 * `core/rtw_rf.c`. Returns 0 = 2.4G, 1 = 5G, 0xFF on invalid channel.
 * `group` is the index into the per-band EFUSE PG table; `cck_group` is
 * the 2.4G CCK sub-group (mostly == group, except ch14 → 5). */
static uint8_t classify_channel(uint8_t ch, uint8_t *group, uint8_t *cck_group) {
  if (ch <= 14) {
    int gp = -1, cck_gp = -1;
    if (1 <= ch && ch <= 2)        gp = 0;
    else if (3  <= ch && ch <= 5)  gp = 1;
    else if (6  <= ch && ch <= 8)  gp = 2;
    else if (9  <= ch && ch <= 11) gp = 3;
    else if (12 <= ch && ch <= 14) gp = 4;
    cck_gp = (ch == 14) ? 5 : gp;
    if (gp < 0) return 0xFF;
    if (group) *group = static_cast<uint8_t>(gp);
    if (cck_group) *cck_group = static_cast<uint8_t>(cck_gp);
    return 0; /* 2.4G */
  }
  /* 5G group boundaries — verbatim from upstream `rtw_get_ch_group`
   * (core/rtw_rf.c). NB the gp3/gp4 split is at 98/100 (not 80/82) and the
   * top group runs to 253: channels 82..98 belong to gp3, not gp4. */
  int gp = -1;
  if      (15  <= ch && ch <=  42) gp = 0; /* 15: below-band edge (kCenterCh5gAll[0]) */
  else if (44  <= ch && ch <=  48) gp = 1;
  else if (50  <= ch && ch <=  58) gp = 2;
  else if (60  <= ch && ch <=  98) gp = 3;
  else if (100 <= ch && ch <= 106) gp = 4;
  else if (108 <= ch && ch <= 114) gp = 5;
  else if (116 <= ch && ch <= 122) gp = 6;
  else if (124 <= ch && ch <= 130) gp = 7;
  else if (132 <= ch && ch <= 138) gp = 8;
  else if (140 <= ch && ch <= 144) gp = 9;
  else if (149 <= ch && ch <= 155) gp = 10;
  else if (157 <= ch && ch <= 161) gp = 11;
  else if (165 <= ch && ch <= 171) gp = 12;
  else if (173 <= ch && ch <= 253) gp = 13;
  if (gp < 0) return 0xFF;
  if (group) *group = static_cast<uint8_t>(gp);
  return 1; /* 5G */
}

/* Upstream `core/rtw_rf.c:center_ch_5g_all[CENTER_CH_5G_ALL_NUM]` — the
 * canonical 5G channel index used to populate `Index5G_BW40_Base`. Mirrors
 * the upstream table verbatim. */
static const uint8_t kCenterCh5gAll[65] = {
    15, 16, 17, 18, 20, 24, 28, 32, 36, 38, 40, 42, 44, 46, 48,
    52, 54, 56, 58, 60, 62, 64, 68, 72, 76, 80, 84, 88, 92, 96,
    100, 102, 104, 106, 108, 110, 112, 116, 118, 120, 122, 124, 126, 128,
    132, 134, 136, 138, 140, 142, 144, 149, 151, 153, 155, 157, 159, 161,
    165, 167, 169, 171, 173, 175, 177,
};

/* Vendor default PG TX-power maps — verbatim from upstream `hal_com_phycfg.c`
 * (`pg_txpwr_def_info` + per-chip `rtl88{12,21}a_pg_txpwr_def_info`). Each map
 * is the per-path byte stream that starts at PG offset `pg_txpwr_saddr` (0x10),
 * laid out identically to the EFUSE PG block (2.4G 18 B + 5G 24 B per path).
 * The kernel's `hal_load_pg_txpwr_info` uses these as fallback sources
 * (PG_DATA -> IC_DEF -> DEF): a base cell whose EFUSE byte is invalid
 * (> txgi_max = 63, e.g. an unprogrammed 0xFF) is filled from the IC map, then
 * the generic map. 0xEE/0xFF bytes at base positions are themselves invalid and
 * fall through to the next source. */
static const uint8_t kPgTxpwrDefGeneric[168] = {
    0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x24, 0xEE, 0xEE, 0xEE, 0xEE,
    0xEE, 0xEE, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A,
    0x04, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D,
    0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x24, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0x2A, 0x2A, 0x2A, 0x2A,
    0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x04, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE,
    0xEE, 0xEE, 0xEE, 0xEE, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x24,
    0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A,
    0x2A, 0x2A, 0x2A, 0x2A, 0x04, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0x2D, 0x2D,
    0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x24, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE,
    0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x04, 0xEE,
    0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};

static const uint8_t kPgTxpwrDef8814a[168] = {
    0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x02, 0xEE, 0xEE, 0xEE, 0xEE,
    0xEE, 0xEE, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A,
    0x02, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0x00, 0xEE, 0xEE, 0xEE, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D,
    0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x02, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0x2A, 0x2A, 0x2A, 0x2A,
    0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x02, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE,
    0x00, 0xEE, 0xEE, 0xEE, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x02,
    0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A,
    0x2A, 0x2A, 0x2A, 0x2A, 0x02, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0x00, 0xEE, 0xEE, 0xEE, 0x2D, 0x2D,
    0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x02, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE,
    0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x02, 0xEE,
    0xEE, 0xEE, 0xEE, 0xEE, 0x00, 0xEE, 0xEE, 0xEE};

static const uint8_t kPgTxpwrDef8812a[82] = {
    0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x02, 0xEE, 0xEE, 0xFF, 0xFF,
    0xFF, 0xFF, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A,
    0x02, 0xEE, 0xFF, 0xFF, 0xEE, 0xFF, 0x00, 0xEE, 0xFF, 0xFF, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D,
    0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x02, 0xEE, 0xEE, 0xFF, 0xFF, 0xFF, 0xFF, 0x2A, 0x2A, 0x2A, 0x2A,
    0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x02, 0xEE, 0xFF, 0xFF, 0xEE, 0xFF,
    0x00, 0xEE};

static const uint8_t kPgTxpwrDef8821a[39] = {
    0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x24, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A,
    0x04, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

void EepromManager::LoadTxPowerInfo() {
  /* EFUSE PG TX-power block layout — port of
   * `hal_load_pg_txpwr_info_path_{2,5}g` from upstream
   * `hal/hal_com_phycfg.c`. Per-path, 2.4G uses 18 bytes, 5G uses 24
   * bytes, both paths laid out contiguously starting at PG offset 0x10
   * (8812/8814 share this layout per `pg_txpwr_saddr=0x10`).
   *
   * Sequence per path:
   *   2.4G:
   *     6 bytes CCK base   (one per channel group, MAX_CHNL_GROUP_24G=6)
   *     5 bytes BW40 base  (MAX_CHNL_GROUP_24G-1; last group folds in)
   *     1 byte  Ntx=1 BW20+OFDM diffs (MSB nibble=BW20, LSB=OFDM)
   *     2 bytes Ntx=2 (BW40+BW20, then OFDM+CCK)
   *     2 bytes Ntx=3
   *     2 bytes Ntx=4
   *   5G:
   *     14 bytes BW40 base (MAX_CHNL_GROUP_5G)
   *     1 byte  Ntx=1 BW20+OFDM
   *     2 bytes Ntx=2 (BW40+BW20, then OFDM)
   *     2 bytes Ntx=3
   *     2 bytes Ntx=4
   *     plus 3 bytes BW80 diffs (Ntx=1..3 in nibble pairs)
   *
   * Diff nibbles are signed 4-bit. Helpers `pg_msb_diff` / `pg_lsb_diff`
   * sign-extend to int8_t. */
  constexpr uint16_t kPgSaddr = 0x10;
  uint16_t off = kPgSaddr;

  /* Stage 1: per-path EFUSE byte stream → per-group base + per-Ntx diff.
   * Iterates rfpath=0..numTotalRfPath-1 because that's what's compiled
   * into the EFUSE for this chip variant. The kernel iterates MAX_RF_PATH
   * unconditionally and skips per `HAL_SPEC_CHK_RF_PATH_*` — devourer
   * already has `numTotalRfPath` set by `rtw_hal_config_rftype` post-EFUSE
   * read. */
  /* Per-path-per-group base arrays (intermediate, before per-channel
   * scattering). Layout: [path][group]. */
  uint8_t cck_base_2g[kMaxRfPath][6] = {};
  uint8_t bw40_base_2g[kMaxRfPath][6] = {};
  uint8_t bw40_base_5g[kMaxRfPath][14] = {};

  /* Fallback sources for invalid (unprogrammed) base cells — port of the
   * kernel's PG_DATA -> IC_DEF -> DEF chain in `hal_load_pg_txpwr_info`. Pick
   * the IC default map by chip; the generic map is the final fallback. */
  const uint8_t *ic_def = nullptr;
  size_t ic_def_len = 0;
  switch (version_id.ICType) {
  case CHIP_8814A: ic_def = kPgTxpwrDef8814a; ic_def_len = sizeof(kPgTxpwrDef8814a); break;
  case CHIP_8812:  ic_def = kPgTxpwrDef8812a; ic_def_len = sizeof(kPgTxpwrDef8812a); break;
  case CHIP_8821:  ic_def = kPgTxpwrDef8821a; ic_def_len = sizeof(kPgTxpwrDef8821a); break;
  default: break;
  }
  /* Read a base byte with the vendor fallback chain. A base is invalid when it
   * exceeds txgi_max (63) — the same test the kernel uses
   * (IS_PG_TXPWR_BASE_INVALID). First valid of {EFUSE, IC-def, generic-def}
   * wins; if all are invalid the (invalid) EFUSE byte is returned and gets
   * clamped downstream, matching the kernel leaving the cell invalid. */
  auto def_at = [](const uint8_t *map, size_t len, uint16_t o) -> uint8_t {
    return (o >= kPgSaddr && (size_t)(o - kPgSaddr) < len) ? map[o - kPgSaddr]
                                                           : 0xFF;
  };
  auto read_base = [&](uint16_t o) -> uint8_t {
    uint8_t v = efuse_eeprom_data[o];
    if (v <= 63) return v;
    if (ic_def) { uint8_t d = def_at(ic_def, ic_def_len, o); if (d <= 63) return d; }
    uint8_t d = def_at(kPgTxpwrDefGeneric, sizeof(kPgTxpwrDefGeneric), o);
    if (d <= 63) return d;
    return v;
  };

  for (int path = 0; path < numTotalRfPath && path < kMaxRfPath; path++) {
    /* 2.4G section — 18 bytes per path. */
    for (int g = 0; g < 6; g++)
      cck_base_2g[path][g] = read_base(off++);
    for (int g = 0; g < 5; g++)
      bw40_base_2g[path][g] = read_base(off++);
    /* Ntx=1: 1 byte (MSB=BW20, LSB=OFDM) */
    {
      uint8_t v = efuse_eeprom_data[off++];
      BW20_24G_Diff[path][0] = pg_msb_diff(v);
      OFDM_24G_Diff[path][0] = pg_lsb_diff(v);
    }
    /* Ntx=2..4: 2 bytes each (BW40|BW20 then OFDM|CCK) */
    for (int t = 1; t < 4; t++) {
      uint8_t v = efuse_eeprom_data[off++];
      BW40_24G_Diff[path][t] = pg_msb_diff(v);
      BW20_24G_Diff[path][t] = pg_lsb_diff(v);
      v = efuse_eeprom_data[off++];
      OFDM_24G_Diff[path][t] = pg_msb_diff(v);
      CCK_24G_Diff[path][t]  = pg_lsb_diff(v);
    }

    /* 5G section — 24 bytes per path. */
    for (int g = 0; g < 14; g++)
      bw40_base_5g[path][g] = read_base(off++);
    /* Ntx=1: 1 byte (MSB=BW20, LSB=OFDM) */
    {
      uint8_t v = efuse_eeprom_data[off++];
      BW20_5G_Diff[path][0] = pg_msb_diff(v);
      OFDM_5G_Diff[path][0] = pg_lsb_diff(v);
    }
    /* Ntx=2..4: ONE byte each (MSB=BW40, LSB=BW20). Unlike the 2.4G
     * block, 5G packs the OFDM diffs separately below — the previous
     * two-bytes-per-Ntx parse reused the 2.4G shape and shifted every
     * field from byte 16 onward (kernel hal_load_pg_txpwr_info_path_5g,
     * hal_com_phycfg.c:848-953). */
    for (int t = 1; t < 4; t++) {
      uint8_t v = efuse_eeprom_data[off++];
      BW40_5G_Diff[path][t] = pg_msb_diff(v);
      BW20_5G_Diff[path][t] = pg_lsb_diff(v);
    }
    /* OFDM diff 2T~3T: one byte (MSB=2T, LSB=3T). */
    {
      uint8_t v = efuse_eeprom_data[off++];
      OFDM_5G_Diff[path][1] = pg_msb_diff(v);
      OFDM_5G_Diff[path][2] = pg_lsb_diff(v);
    }
    /* OFDM diff 4T: one byte, LSB nibble only. */
    {
      uint8_t v = efuse_eeprom_data[off++];
      OFDM_5G_Diff[path][3] = pg_lsb_diff(v);
    }
    /* BW80|BW160 diffs: four bytes, tx 0..3 (MSB=BW80, LSB=BW160 — no
     * 160MHz support here, the LSB nibble is consumed for layout only). */
    for (int t = 0; t < 4; t++) {
      uint8_t v = efuse_eeprom_data[off++];
      BW80_5G_Diff[path][t] = pg_msb_diff(v);
    }
  }

  /* Stage 2: per-group → per-channel. Mirrors upstream `hal_load_txpwr_info`. */
  for (int path = 0; path < numTotalRfPath && path < kMaxRfPath; path++) {
    /* 2.4G: 14 channels mapped via classify_channel. */
    for (int ch_idx = 0; ch_idx < kCenterCh2gNum; ch_idx++) {
      uint8_t group = 0, cck_group = 0;
      if (classify_channel(static_cast<uint8_t>(ch_idx + 1), &group,
                           &cck_group) != 0)
        continue;
      Index24G_CCK_Base[path][ch_idx] = cck_base_2g[path][cck_group];
      Index24G_BW40_Base[path][ch_idx] = bw40_base_2g[path][group];
    }
    /* 5G: 65 channels from kCenterCh5gAll. */
    for (int ch_idx = 0; ch_idx < kCenterCh5gAllNum; ch_idx++) {
      uint8_t group = 0;
      if (classify_channel(kCenterCh5gAll[ch_idx], &group, nullptr) != 1)
        continue;
      Index5G_BW40_Base[path][ch_idx] = bw40_base_5g[path][group];
    }
  }

  TxPowerInfoLoaded = true;
  _logger->info("LoadTxPowerInfo: parsed {} paths, 2.4G ch6 CCK_Base[0]=0x{:02x} "
                "BW40_Base[0]=0x{:02x}",
                unsigned(numTotalRfPath),
                unsigned(Index24G_CCK_Base[0][5]),
                unsigned(Index24G_BW40_Base[0][5]));
}

void EepromManager::LoadTxPowerByRate() {
  /* Reset to zero — invalid rates / unmapped registers stay 0 and the
   * lookup returns 0 (no offset). */
  std::memset(TxPwrByRateOffset, 0, sizeof(TxPwrByRateOffset));
  std::memset(TxPwrByRateBase, 0, sizeof(TxPwrByRateBase));

  /* Stage 1: parse kHal8812aPhyRegPg into per-(band,path,rate) raw values. */
  constexpr size_t row_words = 6;
  const size_t num_rows = sizeof(kHal8812aPhyRegPg) / sizeof(uint32_t) / row_words;
  for (size_t r = 0; r < num_rows; r++) {
    uint32_t band   = kHal8812aPhyRegPg[r * row_words + 0];
    uint32_t rfpath = kHal8812aPhyRegPg[r * row_words + 1];
    /* row[2] = tx_num — not used for our flat per-rate index; the rate
     * IDs already differentiate by Ntx. */
    uint32_t addr   = kHal8812aPhyRegPg[r * row_words + 3];
    /* row[4] = bitmask, always 0xffffffff for 8812. */
    uint32_t data   = kHal8812aPhyRegPg[r * row_words + 5];
    if (band >= 2 || rfpath >= kMaxRfPath) continue;
    const uint16_t reg = static_cast<uint16_t>(addr);
    const RateRow *rr = nullptr;
    for (const auto &m : kRateMap) {
      if (m.reg == reg) { rr = &m; break; }
    }
    if (!rr) continue;
    for (int i = 0; i < rr->count; i++) {
      int8_t idx = rate_to_idx(rr->rates[i]);
      if (idx < 0) continue;
      uint8_t val = static_cast<uint8_t>((data >> (i * 8)) & 0xff);
      TxPwrByRateOffset[band][rfpath][idx] = static_cast<int8_t>(val);
    }
  }

  /* Stage 2: compute per-section base (value at rate_sec_base[rs]). */
  for (int band = 0; band < 2; band++) {
    for (int path = 0; path < numTotalRfPath && path < kMaxRfPath; path++) {
      for (int rs = 0; rs < kNumRateSection; rs++) {
        int8_t base_idx = rate_to_idx(kSectionBaseRate[rs]);
        if (base_idx < 0) continue;
        TxPwrByRateBase[band][path][rs] =
            static_cast<uint8_t>(TxPwrByRateOffset[band][path][base_idx]);
      }
    }
  }

  /* Stage 3: normalize — replace each rate's raw value with
   * (raw - section_base), yielding a small signed offset. Mirrors
   * upstream `phy_ConvertTxPowerByRateInDbmToRelativeValues`. */
  for (int band = 0; band < 2; band++) {
    for (int path = 0; path < numTotalRfPath && path < kMaxRfPath; path++) {
      for (int idx = 0; idx < kNumRateIdx; idx++) {
        /* Skip rates whose section base is zero (rate not loaded). */
        /* Reconstruct the MGN_RATE from idx to find its section. */
        uint8_t rate;
        if (idx <= 3) {
          static const uint8_t cck[] = {0x02, 0x04, 0x0B, 0x16};
          rate = cck[idx];
        } else if (idx <= 11) {
          static const uint8_t ofdm[] = {0x0C, 0x12, 0x18, 0x24,
                                         0x30, 0x48, 0x60, 0x6C};
          rate = ofdm[idx - 4];
        } else if (idx <= 43) {
          rate = static_cast<uint8_t>(0x80 + (idx - 12));
        } else {
          rate = static_cast<uint8_t>(0xA0 + (idx - 44));
        }
        int8_t section = rate_to_section(rate);
        if (section < 0) continue;
        int raw = TxPwrByRateOffset[band][path][idx];
        int base = TxPwrByRateBase[band][path][section];
        TxPwrByRateOffset[band][path][idx] =
            static_cast<int8_t>(raw - base);
      }
    }
  }

  TxPwrByRateLoaded = true;
  _logger->info("LoadTxPowerByRate: 2.4G path-A OFDM-6M offset={} HT-MCS7 base={}",
                int(TxPwrByRateOffset[0][0][rate_to_idx(0x0C)]),
                unsigned(TxPwrByRateBase[0][0][RS_HT_1SS]));
}

void EepromManager::LoadTxPowerLimit() {
  /* Init to a sentinel "63" = txgi_max meaning "no limit" — entries not
   * touched by the parser end up unconstrained. */
  std::memset(TxPwrLimit2g, 63, sizeof(TxPwrLimit2g));
  std::memset(TxPwrLimit5g, 63, sizeof(TxPwrLimit5g));

  /* tuning.regulation override (FCC|ETSI|MKK|WW). */
  if (_tuning.regulation)
    CurrentRegulation = static_cast<int>(*_tuning.regulation);

  const size_t num_strings =
      sizeof(kHal8812aTxpwrLmt) / sizeof(kHal8812aTxpwrLmt[0]);
  for (size_t i = 0; i + 6 < num_strings; i += 7) {
    const char *reg_s   = kHal8812aTxpwrLmt[i + 0];
    const char *band_s  = kHal8812aTxpwrLmt[i + 1];
    const char *bw_s    = kHal8812aTxpwrLmt[i + 2];
    const char *rate_s  = kHal8812aTxpwrLmt[i + 3];
    const char *ntx_s   = kHal8812aTxpwrLmt[i + 4];
    const char *ch_s    = kHal8812aTxpwrLmt[i + 5];
    const char *val_s   = kHal8812aTxpwrLmt[i + 6];

    uint8_t reg;
    if      (!strcmp(reg_s, "FCC"))  reg = 0;
    else if (!strcmp(reg_s, "ETSI")) reg = 1;
    else if (!strcmp(reg_s, "MKK"))  reg = 2;
    else if (!strcmp(reg_s, "WW"))   reg = 3;
    else continue;

    uint8_t band;
    if      (!strcmp(band_s, "2.4G")) band = 0;
    else if (!strcmp(band_s, "5G"))   band = 1;
    else continue;

    uint8_t bw;
    if      (!strcmp(bw_s, "20M"))  bw = 0;
    else if (!strcmp(bw_s, "40M"))  bw = 1;
    else if (!strcmp(bw_s, "80M"))  bw = 2;
    else if (!strcmp(bw_s, "160M")) bw = 3;
    else continue;

    uint8_t rs;
    if      (!strcmp(rate_s, "CCK"))  rs = 0;
    else if (!strcmp(rate_s, "OFDM")) rs = 1;
    else if (!strcmp(rate_s, "HT"))   rs = 2;
    else if (!strcmp(rate_s, "VHT"))  rs = 3;
    else continue;

    uint8_t ntx;
    if      (!strcmp(ntx_s, "1T")) ntx = 0;
    else if (!strcmp(ntx_s, "2T")) ntx = 1;
    else if (!strcmp(ntx_s, "3T")) ntx = 2;
    else if (!strcmp(ntx_s, "4T")) ntx = 3;
    else continue;

    uint8_t ch = parse_decimal(ch_s);
    /* val_s may be signed (kept as-is via decimal parse, but no negatives
     * appear in this table). */
    uint8_t val = parse_decimal(val_s);

    if (band == 0) {
      if (bw >= kNum2gBw) continue;
      int8_t ch_idx = lmt_ch_idx_2g(ch);
      if (ch_idx < 0) continue;
      TxPwrLimit2g[reg][bw][rs][ntx][ch_idx] = static_cast<int8_t>(val);
    } else {
      if (bw >= kNum5gBw) continue;
      int8_t ch_idx = -1;
      for (int j = 0; j < kCenterCh5gAllNumLmt; j++)
        if (kCenterCh5gAll[j] == ch) { ch_idx = static_cast<int8_t>(j); break; }
      if (ch_idx < 0) continue;
      TxPwrLimit5g[reg][bw][rs][ntx][ch_idx] = static_cast<int8_t>(val);
    }
  }

  TxPwrLimitLoaded = true;
  static const char *kRegName[] = {"FCC", "ETSI", "MKK", "WW"};
  _logger->info("LoadTxPowerLimit: active regulation={} (FCC OFDM 1T ch6 = {})",
                kRegName[CurrentRegulation],
                int(TxPwrLimit2g[0][0][1][0][5]));
}

uint8_t EepromManager::GetTxPowerIndexBase(uint8_t path, uint8_t rate,
                                           uint8_t ntx_idx, uint8_t bandwidth,
                                           uint8_t channel) const {
  if (!TxPowerInfoLoaded || path >= kMaxRfPath)
    return 0;

  /* Port of `PHY_GetTxPowerIndexBase` from `hal_com_phycfg.c`. Rates are
   * the MGN_* values from upstream `phydm_types.h`; we mirror the macro
   * conditions inline. Bandwidth enum matches devourer's ChannelWidth_t
   * (20=0, 40=1, 80=2, 160=3). */
  /* MGN_* rate group classifiers — keep in sync with upstream. */
  auto is_cck = [](uint8_t r) {
    /* MGN_1M=0x02, MGN_2M=0x04, MGN_5_5M=0x0B, MGN_11M=0x16 */
    return r == 0x02 || r == 0x04 || r == 0x0B || r == 0x16;
  };
  auto is_ofdm = [](uint8_t r) {
    /* MGN_6M..MGN_54M: 0x0C, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6C */
    return r == 0x0C || r == 0x12 || r == 0x18 || r == 0x24 ||
           r == 0x30 || r == 0x48 || r == 0x60 || r == 0x6C;
  };
  /* MGN_MCS0=0x80, MCS31=0x9F (HT); MGN_VHT1SS_MCS0=0xA0..VHT4SS_MCS9=0xC9 */
  auto is_mcs0_7   = [](uint8_t r) { return r >= 0x80 && r <= 0x87; };
  auto is_mcs8_15  = [](uint8_t r) { return r >= 0x88 && r <= 0x8F; };
  auto is_mcs16_23 = [](uint8_t r) { return r >= 0x90 && r <= 0x97; };
  auto is_mcs24_31 = [](uint8_t r) { return r >= 0x98 && r <= 0x9F; };
  auto is_vht1ss   = [](uint8_t r) { return r >= 0xA0 && r <= 0xA9; };
  auto is_vht2ss   = [](uint8_t r) { return r >= 0xAA && r <= 0xB3; };
  auto is_vht3ss   = [](uint8_t r) { return r >= 0xB4 && r <= 0xBD; };
  auto is_vht4ss   = [](uint8_t r) { return r >= 0xBE && r <= 0xC7; };

  uint8_t group = 0, cck_group = 0;
  uint8_t band = classify_channel(channel, &group, &cck_group);
  if (band == 0xFF)
    return 0;
  /* Channel index into the per-band base array. For 2.4G it's
   * `channel - 1` (channels 1-14). For 5G it's the index in
   * kCenterCh5gAll. */
  uint8_t ch_idx = 0;
  if (band == 0) {
    if (channel == 0 || channel > kCenterCh2gNum)
      return 0;
    ch_idx = static_cast<uint8_t>(channel - 1);
  } else {
    /* Nearest-entry fallback for channels the table doesn't list (odd-grid or
     * extended-synth channels >177): returning 0 here would silently program
     * a near-zero TX power index. The table is sorted, so track the closest
     * entry — an exact hit still wins. Power on a non-listed channel is
     * extrapolated, not characterized. */
    uint8_t best = 0;
    int best_d = 0x7fffffff;
    for (uint8_t i = 0; i < kCenterCh5gAllNum; i++) {
      const int d = (kCenterCh5gAll[i] > channel)
                        ? kCenterCh5gAll[i] - channel
                        : channel - kCenterCh5gAll[i];
      if (d < best_d) {
        best_d = d;
        best = i;
      }
      if (d == 0)
        break;
    }
    ch_idx = best;
  }

  int txPower = 0;

  if (band == 0) {
    /* 2.4G */
    if (is_cck(rate)) {
      txPower = Index24G_CCK_Base[path][ch_idx];
      txPower += CCK_24G_Diff[path][0];
      if (ntx_idx >= 1) txPower += CCK_24G_Diff[path][1];
      if (ntx_idx >= 2) txPower += CCK_24G_Diff[path][2];
      if (ntx_idx >= 3) txPower += CCK_24G_Diff[path][3];
      goto clamp_and_return;
    }
    txPower = Index24G_BW40_Base[path][ch_idx];
    if (is_ofdm(rate)) {
      txPower += OFDM_24G_Diff[path][0];
      if (ntx_idx >= 1) txPower += OFDM_24G_Diff[path][1];
      if (ntx_idx >= 2) txPower += OFDM_24G_Diff[path][2];
      if (ntx_idx >= 3) txPower += OFDM_24G_Diff[path][3];
      goto clamp_and_return;
    }
    /* MCS / VHT — pick BW20 / BW40 (BW80 falls through to BW40 per upstream
     * comment "Willis suggest adopt BW 40M power index while in BW 80 mode").
     *
     * Kernel accumulation is CUMULATIVE over rate ranges
     * (hal_com_phycfg.c:2490-2496): MCS8-31 adds [0]+[1], MCS16-31 adds
     * [0]+[1]+[2], VHT2SS+ adds [1], VHT3SS+ adds [2], etc. The previous
     * exclusive windows gave e.g. MCS16-23 only [2]. */
    {
      const bool ge_1s = is_mcs0_7(rate) || is_mcs8_15(rate) ||
                         is_mcs16_23(rate) || is_mcs24_31(rate) ||
                         is_vht1ss(rate) || is_vht2ss(rate) ||
                         is_vht3ss(rate) || is_vht4ss(rate);
      const bool ge_2s = is_mcs8_15(rate) || is_mcs16_23(rate) ||
                         is_mcs24_31(rate) || is_vht2ss(rate) ||
                         is_vht3ss(rate) || is_vht4ss(rate);
      const bool ge_3s = is_mcs16_23(rate) || is_mcs24_31(rate) ||
                         is_vht3ss(rate) || is_vht4ss(rate);
      const bool ge_4s = is_mcs24_31(rate) || is_vht4ss(rate);
      const int8_t *diff =
          (bandwidth == 0) ? BW20_24G_Diff[path] : BW40_24G_Diff[path];
      if (ge_1s) txPower += diff[0];
      if (ge_2s) txPower += diff[1];
      if (ge_3s) txPower += diff[2];
      if (ge_4s) txPower += diff[3];
    }
  } else {
    /* 5G — no CCK */
    if (rate < 0x0C)
      return 0;
    txPower = Index5G_BW40_Base[path][ch_idx];
    if (is_ofdm(rate)) {
      txPower += OFDM_5G_Diff[path][0];
      if (ntx_idx >= 1) txPower += OFDM_5G_Diff[path][1];
      if (ntx_idx >= 2) txPower += OFDM_5G_Diff[path][2];
      if (ntx_idx >= 3) txPower += OFDM_5G_Diff[path][3];
      goto clamp_and_return;
    }
    /* MCS / VHT BW20 / BW40 / BW80 — cumulative over rate ranges, same
     * scheme as 2.4G (kernel hal_com_phycfg.c:2550-2601). The previous
     * code had no VHT clauses beyond [0] at all on 5G, so VHT2SS missed
     * [1] and VHT3SS missed [1]+[2]. */
    {
      const bool ge_1s = is_mcs0_7(rate) || is_mcs8_15(rate) ||
                         is_mcs16_23(rate) || is_mcs24_31(rate) ||
                         is_vht1ss(rate) || is_vht2ss(rate) ||
                         is_vht3ss(rate) || is_vht4ss(rate);
      const bool ge_2s = is_mcs8_15(rate) || is_mcs16_23(rate) ||
                         is_mcs24_31(rate) || is_vht2ss(rate) ||
                         is_vht3ss(rate) || is_vht4ss(rate);
      const bool ge_3s = is_mcs16_23(rate) || is_mcs24_31(rate) ||
                         is_vht3ss(rate) || is_vht4ss(rate);
      const bool ge_4s = is_mcs24_31(rate) || is_vht4ss(rate);
      const int8_t *diff = (bandwidth == 0)   ? BW20_5G_Diff[path]
                           : (bandwidth == 1) ? BW40_5G_Diff[path]
                                              : BW80_5G_Diff[path];
      if (ge_1s) txPower += diff[0];
      if (ge_2s) txPower += diff[1];
      if (ge_3s) txPower += diff[2];
      if (ge_4s) txPower += diff[3];
    }
  }

clamp_and_return:
  /* Layer 2: per-rate offset (port of upstream PHY_GetTxPowerByRate).
   *
   * Upstream's aircrack-ng/88XXau USB Makefile sets
   * `CONFIG_TXPWR_BY_RATE_EN = n` (line 48) → `RegEnableTxPowerByRate =
   * 0` → `phy_is_tx_power_by_rate_needed()` returns FALSE →
   * `PHY_GetTxPowerByRate()` returns 0. So kernel never applies the
   * per-rate offset on USB builds; all TX power = base + boost. The
   * 5G TX-AGC canary at ch36/ch100 confirmed this: devourer's headroom-
   * cap incidentally zeroed by_rate at 2.4G (base+boost > limit) but
   * exposed the divergence at 5G where headroom is positive.
   *
   * Default-off to match upstream's USB-build behaviour byte-for-byte.
   * `DEVOURER_ENABLE_TXPWR_BY_RATE=1` re-enables the layer for
   * deployments that want the PG-table offsets (mirrors upstream
   * CONFIG_TXPWR_BY_RATE_EN=y). */
  int by_rate_diff = 0;
  if (_tuning.txpwr_by_rate) {
    const int8_t section_classifier = rate_to_section(rate);
    const bool is_vht_24g =
        (band == 0) && section_classifier >= RS_VHT_1SS;
    if (TxPwrByRateLoaded && !is_vht_24g) {
      int8_t rate_idx = rate_to_idx(rate);
      if (rate_idx >= 0)
        by_rate_diff = TxPwrByRateOffset[band][path][rate_idx];
    }
  }

  /* Layer 3: regulatory limit (port of upstream PHY_GetTxPowerLimit). */
  int limit = 63;
  if (TxPwrLimitLoaded) {
    int8_t section = rate_to_section(rate);
    int rs_lmt;
    if      (section == RS_CCK)  rs_lmt = 0;
    else if (section == RS_OFDM) rs_lmt = 1;
    else if (section >= RS_HT_1SS && section <= RS_HT_4SS) rs_lmt = 2;
    else if (section >= RS_VHT_1SS) rs_lmt = 3;
    else rs_lmt = -1;
    if (rs_lmt >= 0) {
      if (band == 0) {
        if (bandwidth < kNum2gBw && ntx_idx < kMaxTxCount) {
          int8_t v = TxPwrLimit2g[CurrentRegulation][bandwidth][rs_lmt]
                                 [ntx_idx][ch_idx];
          if (v < 63 && v > -63) limit = v;
        }
      } else {
        if (bandwidth < kNum5gBw && ntx_idx < kMaxTxCount) {
          int8_t v = TxPwrLimit5g[CurrentRegulation][bandwidth][rs_lmt]
                                 [ntx_idx][ch_idx];
          if (v < 63 && v > -63) limit = v;
        }
      }
    }
  }

  /* Cap `by_rate_diff` such that the final power doesn't exceed the
   * regulatory `limit`. Upstream writes:
   *   by_rate_diff = (by_rate_diff > limit) ? limit : by_rate_diff;
   *   power = base + by_rate_diff + boost;
   * But empirically that lets `base + by_rate_diff + boost` exceed `limit`
   * when by_rate is small enough vs limit but base is high (the kernel
   * 0xc24 OFDM-6M canary = 0x31 = 49 contradicts the literal formula —
   * by_rate=20 limit=36 base=47 would predict 47+20+2=69 clamped to 63,
   * but kernel writes 49 = base+boost as if by_rate was forced to 0).
   * The effective rule is: by_rate caps at (limit - base - boost), with a
   * floor of 0. */
  constexpr int boost = 2;  /* upstream `transmit_power_boost` default */
  int headroom = limit - txPower - boost;
  if (headroom < 0) headroom = 0;
  if (by_rate_diff > headroom) by_rate_diff = headroom;
  int base_before_apply = txPower;
  txPower += by_rate_diff + boost;
  if (_log_txpwr) {
    _logger->info(
        "txpwr: ch={} path={} rate=0x{:02x} ntx={} bw={} -> "
        "base={} by_rate(capped)={} limit={} boost={} headroom={} final={}",
        unsigned(channel), unsigned(path), unsigned(rate), unsigned(ntx_idx),
        unsigned(bandwidth), base_before_apply, by_rate_diff, limit, boost,
        headroom, txPower);
  }

  if (txPower < 0) txPower = 0;
  if (txPower > 63) txPower = 63; /* txgi_max for 8812/8821/8814 */
  return static_cast<uint8_t>(txPower);
}

bool EepromManager::GetMacAddress(uint8_t out[6]) const {
  /* EFUSE MAC-address offsets per upstream `include/hal_pg.h`:
   *   EEPROM_MAC_ADDR_8812AU = 0xD7
   *   EEPROM_MAC_ADDR_8814AU = 0xD8
   *   EEPROM_MAC_ADDR_8821AU = 0x107  */
  uint16_t off;
  switch (version_id.ICType) {
    case CHIP_8814A: off = 0xD8; break;
    case CHIP_8821:  off = 0x107; break;
    default:         off = 0xD7; break; /* 8812AU / 8811AU (8812 silicon) */
  }
  for (int i = 0; i < 6; ++i)
    out[i] = efuse_eeprom_data[off + i];
  /* All-0xFF means EFUSE empty / unburnt; all-0x00 means we haven't read
   * EFUSE yet. Both are invalid. */
  bool all_ff = true, all_zero = true;
  for (int i = 0; i < 6; ++i) {
    if (out[i] != 0xFF) all_ff = false;
    if (out[i] != 0x00) all_zero = false;
  }
  return !all_ff && !all_zero;
}

JaguarPhyContext EepromManager::GetPhyContext() const {
  /* ODM_ITRF_USB and ODM_CE are phydm enum values (we don't pull the phydm
   * subsystem in, so hardcode the canonical numbers from upstream
   * include/phydm.h). */
  constexpr uint8_t kOdmItrfUsb = 0x02;
  constexpr uint8_t kOdmCe = 0x04;

  /* Pass the resolved rfe_type straight through. Hal_ReadRFEType_8814A now
   * mirrors the kernel decision tree (unburnt/BIT7 0xCA -> 1 on 8814AU,
   * incl. the autoload-fail branch), so the old 0->1 ctx patch-up is
   * redundant — and would mis-map a board whose EFUSE legitimately burns
   * rfe_type=0. */
  const uint8_t rfe_for_ctx = static_cast<uint8_t>(rfe_type);

  return JaguarPhyContext{
      .cut_version = static_cast<uint8_t>(version_id.CUTVersion),
      .support_interface = kOdmItrfUsb,
      .support_platform = kOdmCe,
      .package_type = 0,
      .rfe_type = rfe_for_ctx,
      .type_glna = TypeGLNA,
      .type_gpa = TypeGPA,
      .type_alna = TypeALNA,
      .type_apa = TypeAPA,
  };
}

void EepromManager::rtw_hal_config_rftype() {
  if (IS_1T1R(version_id)) {
    rf_type = RF_TYPE_1T1R;
    numTotalRfPath = 1;
  } else if (IS_2T2R(version_id)) {
    rf_type = RF_TYPE_2T2R;
    numTotalRfPath = 2;
  } else if (IS_1T2R(version_id)) {
    rf_type = RF_TYPE_1T2R;
    numTotalRfPath = 2;
  } else if (IS_3T3R(version_id)) {
    rf_type = RF_TYPE_3T3R;
    numTotalRfPath = 3;
  } else if (IS_4T4R(version_id)) {
    rf_type = RF_TYPE_4T4R;
    numTotalRfPath = 4;
  } else {
    rf_type = RF_TYPE_1T1R;
    numTotalRfPath = 1;
  }

  /* RTL8814AU has 4 RF paths but the baseband caps at 3 spatial streams.
   * For 8812/8811-family the cap equals the path count. */
  maxSpatialStreams = IS_8814A_SERIES(version_id) ? 3 : numTotalRfPath;

  _logger->info("RF_Type is {} TotalTxPath is {} MaxSS is {}", (int)rf_type,
                (int)numTotalRfPath, (int)maxSpatialStreams);
}

void EepromManager::dump_chip_info(HAL_VERSION ChipVersion) {
  int cnt = 0;
  char buf[128] = {0};

  if (IS_8188E(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8188E_");
  else if (IS_8188F(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8188F_");
  else if (IS_8812_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8812_");
  else if (IS_8192E(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8192E_");
  else if (IS_8821_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8821_");
  else if (IS_8723B_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8723B_");
  else if (IS_8703B_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8703B_");
  else if (IS_8723D_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8723D_");
  else if (IS_8814A_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8814A_");
  else if (IS_8822B_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8822B_");
  else if (IS_8821C_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8821C_");
  else
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_UNKNOWN_");

  cnt += sprintf((buf + cnt), "%s_",
                 IS_NORMAL_CHIP(ChipVersion) ? "Normal_Chip" : "Test_Chip");
  if (IS_CHIP_VENDOR_TSMC(ChipVersion))
    cnt += sprintf((buf + cnt), "%s_", "TSMC");
  else if (IS_CHIP_VENDOR_UMC(ChipVersion))
    cnt += sprintf((buf + cnt), "%s_", "UMC");
  else if (IS_CHIP_VENDOR_SMIC(ChipVersion))
    cnt += sprintf((buf + cnt), "%s_", "SMIC");

  if (IS_A_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "A_CUT_");
  else if (IS_B_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "B_CUT_");
  else if (IS_C_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "C_CUT_");
  else if (IS_D_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "D_CUT_");
  else if (IS_E_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "E_CUT_");
  else if (IS_F_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "F_CUT_");
  else if (IS_I_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "I_CUT_");
  else if (IS_J_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "J_CUT_");
  else if (IS_K_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "K_CUT_");
  else
    cnt += sprintf((buf + cnt), "UNKNOWN_CUT(%d)_", ChipVersion.CUTVersion);

  if (IS_1T1R(ChipVersion))
    cnt += sprintf((buf + cnt), "1T1R_");
  else if (IS_1T2R(ChipVersion))
    cnt += sprintf((buf + cnt), "1T2R_");
  else if (IS_2T2R(ChipVersion))
    cnt += sprintf((buf + cnt), "2T2R_");
  else if (IS_3T3R(ChipVersion))
    cnt += sprintf((buf + cnt), "3T3R_");
  else if (IS_3T4R(ChipVersion))
    cnt += sprintf((buf + cnt), "3T4R_");
  else if (IS_4T4R(ChipVersion))
    cnt += sprintf((buf + cnt), "4T4R_");
  else
    cnt += sprintf((buf + cnt), "UNKNOWN_RFTYPE(%d)_", ChipVersion.RFType);

  cnt += sprintf((buf + cnt), "RomVer(%d)", ChipVersion.ROMVer);

  _logger->info(buf);
}

uint8_t EepromManager::GetBoardType() {
  /* 1 ======= BoardType: ODM_CMNINFO_BOARD_TYPE ======= */
  uint32_t odm_board_type = ODM_BOARD_DEFAULT;

  if (ExternalLNA_2G) {
    odm_board_type |= ODM_BOARD_EXT_LNA;
  }

  if (external_lna_5g) {
    odm_board_type |= ODM_BOARD_EXT_LNA_5G;
  }

  if (ExternalPA_2G) {
    odm_board_type |= ODM_BOARD_EXT_PA;
  }

  if (external_pa_5g) {
    odm_board_type |= ODM_BOARD_EXT_PA_5G;
  }

  if (EEPROMBluetoothCoexist) {
    odm_board_type |= ODM_BOARD_BT;
  }

  return (uint8_t)odm_board_type;
}

void EepromManager::hal_InitPGData_8812A() {
  uint32_t i;

  if (false == _device.AutoloadFailFlag) {
    /* autoload OK. */
    if (_device.EepromOrEfuse) {
      /* Read all Content from EEPROM or EFUSE. */
      for (i = 0; i < HWSET_MAX_SIZE_JAGUAR; i += 2) {
        /* value16 = EF2Byte(ReadEEprom(pAdapterState, (u2Byte) (i>>1))); */
        /* *((UInt16*)(&PROMContent[i])) = value16; */
      }
    } else {
      /*  */
      /* 2013/03/08 MH Add for 8812A HW limitation, ROM code can only */
      /*  */
      uint8_t efuse_content[4] = {0};
      _device.efuse_OneByteRead(0x200, &efuse_content[0]);
      _device.efuse_OneByteRead(0x202, &efuse_content[1]);
      _device.efuse_OneByteRead(0x204, &efuse_content[2]);
      _device.efuse_OneByteRead(0x210, &efuse_content[3]);
      if (efuse_content[0] != 0xFF || efuse_content[1] != 0xFF ||
          efuse_content[2] != 0xFF || efuse_content[3] != 0xFF) {
        /* DbgPrint("Disable FW ofl load\n"); */
        /* pMgntInfo.RegFWOffload = FALSE; */
      }

      /* Read EFUSE real map to shadow. */
      EFUSE_ShadowMapUpdate(EFUSE_WIFI);
    }
  } else {
    /* autoload fail */
    /* pHalData.AutoloadFailFlag = true; */
    /*  */
    /* 2013/03/08 MH Add for 8812A HW limitation, ROM code can only */
    /*  */
    uint8_t efuse_content[4] = {0};
    _device.efuse_OneByteRead(0x200, &efuse_content[0]);
    _device.efuse_OneByteRead(0x202, &efuse_content[1]);
    _device.efuse_OneByteRead(0x204, &efuse_content[2]);
    _device.efuse_OneByteRead(0x210, &efuse_content[3]);
    if (efuse_content[0] != 0xFF || efuse_content[1] != 0xFF ||
        efuse_content[2] != 0xFF || efuse_content[3] != 0xFF) {
      _device.AutoloadFailFlag = false;
    }

    /* update to default value 0xFF */
    if (!_device.EepromOrEfuse) {
      EFUSE_ShadowMapUpdate(EFUSE_WIFI);
    }
  }

  /* The 11 TX-power-index PG bytes reading back all-0xFF means the EFUSE map
   * came up blank. On the 8812AU this is usually a transient EFUSE read glitch
   * during cold bring-up (the same fragility behind the intermittent
   * _FWFreeToGo8812 poll-fail), not a genuinely unprogrammed part — so re-read
   * the shadow map a few times before giving up. */
  for (int attempt = 0;
       attempt < 3 && IsEfuseTxPowerInfoValid(efuse_eeprom_data) == false;
       attempt++) {
    _logger->warn("EFUSE TX-power PG data blank (attempt {}/3) — re-reading map",
                  attempt + 1);
    Efuse_ReadAllMap(EFUSE_WIFI, efuse_eeprom_data);
  }

  if (IsEfuseTxPowerInfoValid(efuse_eeprom_data) == false) {
    /* Still blank after retries. The vendor reads PG data from a config file
     * here (Hal_readPGDataFromConfigFile); a userspace driver has no such file,
     * so — like the kernel driver on an autoload-fail — proceed with the map
     * left at its 0xFF default. LoadTxPowerInfo()'s per-cell fallback chain then
     * supplies the IC-default PG table (kPgTxpwrDef8812a / generic). TX power is
     * default rather than efuse-calibrated; logged loudly so a degraded read
     * isn't mistaken for a good one. */
    _logger->error("EFUSE TX-power PG data invalid after retries — using "
                   "default TX-power calibration (autoload-fail fallback)");
  }
}

void EepromManager::EFUSE_ShadowMapUpdate(uint8_t efuseType) {
  if (_device.AutoloadFailFlag) {
    for (int i = 0; i < sizeof(efuse_eeprom_data); i++) {
      efuse_eeprom_data[i] = 0xFF;
    }
  } else {
    Efuse_ReadAllMap(efuseType, efuse_eeprom_data);
  }

  rtw_dump_cur_efuse();
}

void EepromManager::Efuse_ReadAllMap(uint8_t efuseType, uint8_t *Efuse) {
  EfusePowerSwitch8812A(false, true);
  efuse_ReadEFuse(efuseType, 0, EFUSE_MAP_LEN_JAGUAR, Efuse);
  EfusePowerSwitch8812A(false, false);
}

bool EepromManager::ReadPhysicalEfuseMap(uint8_t *map, size_t len) {
  if (map == nullptr || len != EFUSE_MAP_LEN_JAGUAR)
    return false;
  Efuse_ReadAllMap(EFUSE_WIFI, map);
  return true;
}

enum {
  VOLTAGE_V25 = 0x03,
  LDOE25_SHIFT = 28,
};

void EepromManager::EfusePowerSwitch8812A(bool bWrite, bool pwrState) {
  uint16_t tmpV16;
  const uint8_t EFUSE_ACCESS_ON_JAGUAR = 0x69;
  const uint8_t EFUSE_ACCESS_OFF_JAGUAR = 0x00;
  if (pwrState) {
    _device.rtw_write8(REG_EFUSE_BURN_GNT_8812, EFUSE_ACCESS_ON_JAGUAR);

    /* 1.2V Power: From VDDON with Power Cut(0x0000h[15]), defualt valid */
    tmpV16 = _device.rtw_read16(REG_SYS_ISO_CTRL);
    if (!((tmpV16 & PWC_EV12V) == PWC_EV12V)) {
      tmpV16 |= PWC_EV12V;
      /* Write16(pAdapterState,REG_SYS_ISO_CTRL,tmpV16); */
    }

    /* Reset: 0x0000h[28], default valid */
    tmpV16 = _device.rtw_read16(REG_SYS_FUNC_EN);
    if (!((tmpV16 & FEN_ELDR) == FEN_ELDR)) {
      tmpV16 |= FEN_ELDR;
      _device.rtw_write16(REG_SYS_FUNC_EN, tmpV16);
    }

    /* Clock: Gated(0x0008h[5]) 8M(0x0008h[1]) clock from ANA, default valid */
    tmpV16 = _device.rtw_read16(REG_SYS_CLKR);
    if ((!((tmpV16 & LOADER_CLK_EN) == LOADER_CLK_EN)) ||
        (!((tmpV16 & ANA8M) == ANA8M))) {
      tmpV16 |= (LOADER_CLK_EN | ANA8M);
      _device.rtw_write16(REG_SYS_CLKR, tmpV16);
    }

    if (bWrite) {
      /* Enable LDO 2.5V before read/write action */
      auto tempval = _device.rtw_read8(REG_EFUSE_TEST + 3);
      // tempval &= ~(BIT3 | BIT4 | BIT5 | BIT6);
      // tempval &= (0b1111_0111 & 0b1110_1111 & 0b1101_1111 & 0b1011_1111);
      tempval &= 0b10000111;
      tempval |= (VOLTAGE_V25 << 3);
      tempval |= 0b10000000;
      _device.rtw_write8(REG_EFUSE_TEST + 3, tempval);
    }
  } else {
    _device.rtw_write8(REG_EFUSE_BURN_GNT_8812, EFUSE_ACCESS_OFF_JAGUAR);

    if (bWrite) {
      /* Disable LDO 2.5V after read/write action */
      auto tempval = _device.rtw_read8(REG_EFUSE_TEST + 3);
      _device.rtw_write8(REG_EFUSE_TEST + 3, (uint8_t)(tempval & 0x7F));
    }
  }
}

void EepromManager::efuse_ReadEFuse(uint8_t efuseType, uint16_t _offset,
                                    uint16_t _size_byte, uint8_t *pbuf) {
  if (efuseType == EFUSE_WIFI) {
    Hal_EfuseReadEFuse8812A(_offset, _size_byte, pbuf);
  } else {
    throw std::logic_error("hal_ReadEFuse_BT not yet implemented");
    // hal_ReadEFuse_BT(adapterState, _offset, _size_byte, pbuf, bPseudoTest);
  }
}

void EepromManager::Hal_EfuseReadEFuse8812A(uint16_t _offset,
                                            uint16_t _size_byte,
                                            uint8_t *pbuf) {
  uint16_t eFuse_Addr = 0;
  uint8_t offset, wren;
  uint16_t i, j;
  uint8_t u1temp = 0;

  /*  */
  /* Do NOT excess total size of EFuse table. Added by Roger, 2008.11.10. */
  /*  */
  if ((_offset + _size_byte) > EFUSE_MAP_LEN_JAGUAR) {
    /* total E-Fuse table is 512bytes */
    _logger->error("Hal_EfuseReadEFuse8812A(): Invalid offset({:x}) with read "
                   "bytes({:x})!!",
                   _offset, _size_byte);
    return;
  }

  uint8_t efuseTbl[EFUSE_MAP_LEN_JAGUAR] = {0};

  uint16_t eFuseWord[EFUSE_MAX_SECTION_JAGUAR][EFUSE_MAX_WORD_UNIT];
  /* 0. Refresh efuse init map as all oxFF. */
  memset(eFuseWord, 0xFF,
         EFUSE_MAX_SECTION_JAGUAR * EFUSE_MAX_WORD_UNIT * sizeof(uint16_t));

  /*  */
  /* 1. Read the first byte to check if efuse is empty!!! */
  /*  */
  /*  */
  uint8_t rtemp8;
  _device.ReadEFuseByte(eFuse_Addr, &rtemp8);
  if (rtemp8 != 0xFF) {
    _logger->info("efuse_Addr-{:x} efuse_data={:x}", eFuse_Addr, (int)rtemp8);
    eFuse_Addr++;
  } else {
    _logger->info("EFUSE is empty efuse_Addr-{:x} efuse_data={:x}", eFuse_Addr,
                  (int)rtemp8);
    return;
  }

  /*  */
  /* 2. Read real efuse content. Filter PG header and every section data. */
  /*  */
  while ((rtemp8 != 0xFF) && (eFuse_Addr < EFUSE_REAL_CONTENT_LEN_JAGUAR)) {
    /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("efuse_Addr-%d efuse_data=%x\n",
     * eFuse_Addr-1, *rtemp8)); */

    /* Check PG header for section num. */
    if ((rtemp8 & 0x1F) == 0x0F) {
      /* extended header */
      u1temp = (uint8_t)((rtemp8 & 0xE0) >> 5);
      /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("extended header u1temp=%x
       * *rtemp&0xE0 0x%x\n", u1temp, *rtemp8 & 0xE0)); */

      /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("extended header u1temp=%x\n",
       * u1temp)); */

      _device.ReadEFuseByte(eFuse_Addr, &rtemp8);

      /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("extended header efuse_Addr-%d
       * efuse_data=%x\n", eFuse_Addr, *rtemp8));	 */

      if ((rtemp8 & 0x0F) == 0x0F) {
        eFuse_Addr++;
        _device.ReadEFuseByte(eFuse_Addr, &rtemp8);

        if (rtemp8 != 0xFF && (eFuse_Addr < EFUSE_REAL_CONTENT_LEN_JAGUAR))
          eFuse_Addr++;
        continue;
      } else {
        offset = (uint8_t)(((rtemp8 & 0xF0) >> 1) | u1temp);
        wren = (uint8_t)(rtemp8 & 0x0F);
        eFuse_Addr++;
      }
    } else {
      offset = (uint8_t)((rtemp8 >> 4) & 0x0f);
      wren = (uint8_t)(rtemp8 & 0x0f);
    }

    if (offset < EFUSE_MAX_SECTION_JAGUAR) {
      /* Get word enable value from PG header */
      /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("Offset-%d Worden=%x\n", offset,
       * wren)); */

      for (i = 0; i < EFUSE_MAX_WORD_UNIT; i++) {
        /* Check word enable condition in the section */
        if (!((wren & 0x01) == 0x01)) {
          /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("Addr=%d\n", eFuse_Addr)); */
          _device.ReadEFuseByte(eFuse_Addr, &rtemp8);
          eFuse_Addr++;
          eFuseWord[offset][i] = (ushort)(rtemp8 & 0xff);

          if (eFuse_Addr >= EFUSE_REAL_CONTENT_LEN_JAGUAR)
            break;

          /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("Addr=%d", eFuse_Addr)); */
          _device.ReadEFuseByte(eFuse_Addr, &rtemp8);
          eFuse_Addr++;

          eFuseWord[offset][i] |= (ushort)(((rtemp8) << 8) & 0xff00);

          if (eFuse_Addr >= EFUSE_REAL_CONTENT_LEN_JAGUAR)
            break;
        }

        wren >>= 1;
      }
    } else {
      /* deal with error offset,skip error data		 */
      _logger->error("invalid offset:0x{:X}", offset);
      for (i = 0; i < EFUSE_MAX_WORD_UNIT; i++) {
        /* Check word enable condition in the section */
        if (!((wren & 0x01) == 0x01)) {
          eFuse_Addr++;
          if (eFuse_Addr >= EFUSE_REAL_CONTENT_LEN_JAGUAR)
            break;
          eFuse_Addr++;
          if (eFuse_Addr >= EFUSE_REAL_CONTENT_LEN_JAGUAR)
            break;
        }
      }
    }

    /* Read next PG header */
    _device.ReadEFuseByte(eFuse_Addr, &rtemp8);
    /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("Addr=%d rtemp 0x%x\n", eFuse_Addr,
     * *rtemp8)); */

    if (rtemp8 != 0xFF && (eFuse_Addr < EFUSE_REAL_CONTENT_LEN_JAGUAR)) {
      eFuse_Addr++;
    }
  }

  /*  */
  /* 3. Collect 16 sections and 4 word unit into Efuse map. */
  /*  */
  for (i = 0; i < EFUSE_MAX_SECTION_JAGUAR; i++) {
    for (j = 0; j < EFUSE_MAX_WORD_UNIT; j++) {
      efuseTbl[(i * 8) + (j * 2)] = (uint8_t)(eFuseWord[i][j] & 0xff);
      efuseTbl[(i * 8) + ((j * 2) + 1)] =
          (uint8_t)((eFuseWord[i][j] >> 8) & 0xff);
    }
  }

  /*  */
  /* 4. Copy from Efuse map to output pointer memory!!! */
  /*  */
  for (i = 0; i < _size_byte; i++) {
    pbuf[i] = efuseTbl[_offset + i];
  }

  /*  */
  /* 5. Calculate Efuse utilization. */
  /*  */
  // TODO: SetHwReg8812AU(HW_VARIABLES.HW_VAR_EFUSE_BYTES, (byte*)&eFuse_Addr);
  _logger->info("Hal_EfuseReadEFuse8812A: eFuse_Addr offset(0x{:X}) !!",
                eFuse_Addr);
}

#define EEPROM_TX_PWR_INX_8812 0x22

bool EepromManager::IsEfuseTxPowerInfoValid(uint8_t *efuseEepromData) {
  uint16_t tx_index_offset = EEPROM_TX_PWR_INX_8812;
  bool found= false;
  for (int index = 0; index < 11; index++) {
    if (efuseEepromData[tx_index_offset + index] != 0xFF) {
      found = true;
    }
  }

  return found;
}


void EepromManager::rtw_dump_cur_efuse() {
#if 0
#endif
}

#define RTL_EEPROM_ID 0x8129

void EepromManager::Hal_EfuseParseIDCode8812A() {
  u16 EEPROMId;

  /* Checl 0x8129 again for making sure autoload status!! */
  EEPROMId = ReadLE2Byte(&efuse_eeprom_data);
  if (EEPROMId != RTL_EEPROM_ID) {
    _logger->error("EEPROM ID({:x}) is invalid!!\n", EEPROMId);
    _device.AutoloadFailFlag = true;
  } else
    _device.AutoloadFailFlag = false;

  _logger->info("EEPROM ID=0x{:04x}", EEPROMId);
}

#define EEPROM_DEFAULT_VERSION 0
#define EEPROM_VERSION_8812 0xC4

uint8_t EepromManager::Hal_ReadPROMVersion8812A(RtlAdapter device,
                                                uint8_t *efuse_eeprom_data) {
  uint8_t EEPROMVersion;
  if (device.AutoloadFailFlag) {
    EEPROMVersion = EEPROM_DEFAULT_VERSION;
  } else {
    EEPROMVersion = efuse_eeprom_data[EEPROM_VERSION_8812];

    if (EEPROMVersion == 0xFF) {
      EEPROMVersion = EEPROM_DEFAULT_VERSION;
    }
  }

  _logger->info("pHalData.EEPROMVersion is 0x{:X}", EEPROMVersion);
  return EEPROMVersion;
}

#define EEPROM_RF_BOARD_OPTION_8812 0xC1
#define EEPROM_DEFAULT_BOARD_OPTION 0x00

uint8_t EepromManager::Hal_ReadTxPowerInfo8812A(RtlAdapter device,
                                                uint8_t *efuse_eeprom_data) {
  uint8_t EEPROMRegulatory;
  /* 2010/10/19 MH Add Regulator recognize for CU. */
  if (!device.AutoloadFailFlag) {

    if (efuse_eeprom_data[EEPROM_RF_BOARD_OPTION_8812] == 0xFF) {
      EEPROMRegulatory = (EEPROM_DEFAULT_BOARD_OPTION & 0x7); /* bit0~2 */
    } else {
      EEPROMRegulatory =
          (uint8_t)(efuse_eeprom_data[EEPROM_RF_BOARD_OPTION_8812] &
                    0x7); /* bit0~2 */
    }

  } else {
    EEPROMRegulatory = 0;
  }

  _logger->info("EEPROMRegulatory = 0x{:X}", (int)EEPROMRegulatory);

  return EEPROMRegulatory;
}

void EepromManager::Hal_EfuseParseBTCoexistInfo8812A() {
  if (!_device.AutoloadFailFlag) {
    auto tmp_u8 = efuse_eeprom_data[EEPROM_RF_BOARD_OPTION_8812];
    if (((tmp_u8 & 0xe0) >> 5) == 0x1) /* [7:5] */
    {
      EEPROMBluetoothCoexist = true;
    } else {
      EEPROMBluetoothCoexist = false;
    }
  } else {
    EEPROMBluetoothCoexist = false;
  }
}

#define EEPROM_XTAL_8812 0xB9
#define EEPROM_DEFAULT_CRYSTAL_CAP_8812 0x20

void EepromManager::Hal_EfuseParseXtal_8812A() {
  if (!_device.AutoloadFailFlag) {
    crystal_cap = efuse_eeprom_data[EEPROM_XTAL_8812];
    if (crystal_cap == 0xFF) {
      crystal_cap =
          EEPROM_DEFAULT_CRYSTAL_CAP_8812; /* what value should 8812 set? */
    }
  } else {
    crystal_cap = EEPROM_DEFAULT_CRYSTAL_CAP_8812;
  }

  _logger->info("crystal_cap: 0x{:X}", (int)crystal_cap);
}

#define EEPROM_THERMAL_METER_8812 0xBA
#define EEPROM_Default_ThermalMeter_8812 0x18

void EepromManager::Hal_ReadThermalMeter_8812A() {
  /*  */
  /* ThermalMeter from EEPROM */
  /*  */
  if (!_device.AutoloadFailFlag) {
    eeprom_thermal_meter = efuse_eeprom_data[EEPROM_THERMAL_METER_8812];
  } else {
    eeprom_thermal_meter = EEPROM_Default_ThermalMeter_8812;
  }
  /* pHalData.eeprom_thermal_meter = (tempval&0x1f);	 */ /* [4:0] */

  if (eeprom_thermal_meter == 0xff || _device.AutoloadFailFlag) {
    eeprom_thermal_meter = 0xFF;
  }

  /* pHalData.ThermalMeter[0] = pHalData.eeprom_thermal_meter;	 */
  _logger->info("ThermalMeter = 0x{:X}", (int)eeprom_thermal_meter);
}

void EepromManager::Hal_ReadAmplifierType_8812A() {
  uint8_t extTypePA_2G_A =
      ((efuse_eeprom_data[0xBD] & BIT2) >> 2); /* 0xBD[2] */
  uint8_t extTypePA_2G_B =
      ((efuse_eeprom_data[0xBD] & BIT6) >> 6); /* 0xBD[6] */
  uint8_t extTypePA_5G_A =
      ((efuse_eeprom_data[0xBF] & BIT2) >> 2); /* 0xBF[2] */
  uint8_t extTypePA_5G_B =
      ((efuse_eeprom_data[0xBF] & BIT6) >> 6); /* 0xBF[6] */
  uint8_t extTypeLNA_2G_A =
      ((efuse_eeprom_data[0xBD] & (BIT1 | BIT0)) >> 0); /* 0xBD[1:0] */
  uint8_t extTypeLNA_2G_B =
      ((efuse_eeprom_data[0xBD] & (BIT5 | BIT4)) >> 4); /* 0xBD[5:4] */
  uint8_t extTypeLNA_5G_A =
      ((efuse_eeprom_data[0xBF] & (BIT1 | BIT0)) >> 0); /* 0xBF[1:0] */
  uint8_t extTypeLNA_5G_B =
      ((efuse_eeprom_data[0xBF] & (BIT5 | BIT4)) >> 4); /* 0xBF[5:4] */

  hal_ReadPAType_8812A();

  if ((PAType_2G & (BIT5 | BIT4)) ==
      (BIT5 | BIT4)) /* [2.4G] Path A and B are both extPA */
  {
    TypeGPA = (ushort)(extTypePA_2G_B << 2 | extTypePA_2G_A);
  }

  if ((PAType_5G & (BIT1 | BIT0)) ==
      (BIT1 | BIT0)) /* [5G] Path A and B are both extPA */
  {
    TypeAPA = (ushort)(extTypePA_5G_B << 2 | extTypePA_5G_A);
  }

  if ((LNAType_2G & (BIT7 | BIT3)) ==
      (BIT7 | BIT3)) /* [2.4G] Path A and B are both extLNA */
  {
    TypeGLNA = (ushort)(extTypeLNA_2G_B << 2 | extTypeLNA_2G_A);
  }

  if ((LNAType_5G & (BIT7 | BIT3)) ==
      (BIT7 | BIT3)) /* [5G] Path A and B are both extLNA */
  {
    TypeALNA = (ushort)(extTypeLNA_5G_B << 2 | extTypeLNA_5G_A);
  }

  _logger->info("pHalData.TypeGPA = 0x{:X}", TypeGPA);
  _logger->info("pHalData.TypeAPA = 0x{:X}", TypeAPA);
  _logger->info("pHalData.TypeGLNA = 0x{:X}", TypeGLNA);
  _logger->info("pHalData.TypeALNA = 0x{:X}", TypeALNA);
}

#define EEPROM_PA_TYPE_8812AU 0xBC
#define EEPROM_LNA_TYPE_2G_8812AU 0xBD
#define EEPROM_LNA_TYPE_5G_8812AU 0xBF

void EepromManager::hal_ReadPAType_8812A() {
  if (!_device.AutoloadFailFlag) {
    if (registry_priv::AmplifierType_2G == 0) {
      /* AUTO */
      PAType_2G = efuse_eeprom_data[EEPROM_PA_TYPE_8812AU];
      LNAType_2G = efuse_eeprom_data[EEPROM_LNA_TYPE_2G_8812AU];
      if (PAType_2G == 0xFF) {
        PAType_2G = 0;
      }

      if (LNAType_2G == 0xFF) {
        LNAType_2G = 0;
      }

      ExternalPA_2G = ((PAType_2G & BIT5) != 0 && (PAType_2G & BIT4) != 0);
      ExternalLNA_2G = ((LNAType_2G & BIT7) != 0 && (LNAType_2G & BIT3) != 0);
    } else {
      ExternalPA_2G = (registry_priv::AmplifierType_2G & ODM_BOARD_EXT_PA) != 0;
      ExternalLNA_2G =
          (registry_priv::AmplifierType_2G & ODM_BOARD_EXT_LNA) != 0;
    }

    if (registry_priv::AmplifierType_5G == 0) {
      /* AUTO */
      PAType_5G = efuse_eeprom_data[EEPROM_PA_TYPE_8812AU];
      LNAType_5G = efuse_eeprom_data[EEPROM_LNA_TYPE_5G_8812AU];
      if (PAType_5G == 0xFF) {
        PAType_5G = 0;
      }

      if (LNAType_5G == 0xFF) {
        LNAType_5G = 0;
      }

      external_pa_5g = ((PAType_5G & BIT1) != 0 && (PAType_5G & BIT0) != 0);
      external_lna_5g = ((LNAType_5G & BIT7) != 0 && (LNAType_5G & BIT3) != 0);
    } else {
      external_pa_5g =
          (registry_priv::AmplifierType_5G & ODM_BOARD_EXT_PA_5G) != 0;
      external_lna_5g =
          (registry_priv::AmplifierType_5G & ODM_BOARD_EXT_LNA_5G) != 0;
    }
  } else {
    ExternalPA_2G = false;
    external_pa_5g = true;
    ExternalLNA_2G = false;
    external_lna_5g = true;

    if (registry_priv::AmplifierType_2G == 0) {
      /* AUTO */
      ExternalPA_2G = false;
      ExternalLNA_2G = false;
    } else {
      ExternalPA_2G = (registry_priv::AmplifierType_2G & ODM_BOARD_EXT_PA) != 0;
      ExternalLNA_2G =
          (registry_priv::AmplifierType_2G & ODM_BOARD_EXT_LNA) != 0;
    }

    if (registry_priv::AmplifierType_5G == 0) {
      /* AUTO */
      external_pa_5g = false;
      external_lna_5g = false;
    } else {
      external_pa_5g =
          (registry_priv::AmplifierType_5G & ODM_BOARD_EXT_PA_5G) != 0;
      external_lna_5g =
          (registry_priv::AmplifierType_5G & ODM_BOARD_EXT_LNA_5G) != 0;
    }
  }

  _logger->info("pHalData.PAType_2G is 0x{:X}, "
                "pHalData.ExternalPA_2G = {}",
                (int)PAType_2G, (int)ExternalPA_2G);
  _logger->info("pHalData.PAType_5G is 0x{:X}, "
                "pHalData.external_pa_5g = {}",
                (int)PAType_5G, (int)external_pa_5g);
  _logger->info("pHalData.LNAType_2G is 0x{:X}, "
                "pHalData.ExternalLNA_2G = {}",
                LNAType_2G, ExternalLNA_2G);
  _logger->info("pHalData.LNAType_5G is 0x{:X}, "
                "pHalData.external_lna_5g = {}",
                (int)LNAType_5G, (int)external_lna_5g);
}

#define EEPROM_RFE_OPTION_8812 0xCA

void EepromManager::Hal_ReadRFEType_8812A() {
  if (!_device.AutoloadFailFlag) {
    if ((registry_priv::RFE_Type != 64) ||
        0xFF == efuse_eeprom_data[EEPROM_RFE_OPTION_8812]) {
      if (registry_priv::RFE_Type != 64) {
        rfe_type = registry_priv::RFE_Type;
      } else {
        rfe_type = 0;
      }

    } else if ((efuse_eeprom_data[EEPROM_RFE_OPTION_8812] & BIT7) != 0) {
      if (external_lna_5g == true || external_lna_5g == 0 /*null*/) {
        if (external_pa_5g == true || external_pa_5g == 0 /*null*/) {
          if (ExternalLNA_2G && ExternalPA_2G) {
            rfe_type = 3;
          } else {
            rfe_type = 0;
          }
        } else {
          rfe_type = 2;
        }
      } else {
        rfe_type = 4;
      }
    } else {
      rfe_type = (ushort)(efuse_eeprom_data[EEPROM_RFE_OPTION_8812] & 0x3F);

      /* 2013/03/19 MH Due to othe customer already use incorrect EFUSE map */
      /* to for their product. We need to add workaround to prevent to modify */
      /* spec and notify all customer to revise the IC 0xca content. After */
      /* discussing with Willis an YN, revise driver code to prevent. */
      if (rfe_type == 4 &&
          (external_pa_5g == true || ExternalPA_2G == true ||
           external_lna_5g == true || ExternalLNA_2G == true)) {
        rfe_type = 0;
      }
    }
  } else {
    if (registry_priv::RFE_Type != 64) {
      rfe_type = registry_priv::RFE_Type;
    } else {
      rfe_type = 0;
    }
  }

  _logger->info("RFE Type: 0x{:X}", rfe_type);
}

/* Kernel hal_ReadAmplifierType_8814A (rtl8814a_hal_init.c:1474-1525): on
 * 8814 the PA/LNA state is DERIVED from rfe_type, not parsed from the
 * 0xBC-0xC0 EFUSE bytes (the byte-parsing hal_ReadPAType_8814A is dead
 * code upstream — no caller). Note: neither driver's 8814 table
 * check_positive consumes the Type* words (cond2 ignored both sides), so
 * these mostly matter for logging/diagnostics parity. */
void EepromManager::hal_ReadAmplifierType_8814A() {
  switch (rfe_type) {
  case 1: /* 8814AU */
    external_pa_5g = external_lna_5g = 1;
    TypeAPA = TypeALNA = 0;
    break;
  case 2: /* socket board 8814AR and 8194AR */
    ExternalPA_2G = true;
    ExternalLNA_2G = true;
    external_pa_5g = external_lna_5g = 1;
    TypeAPA = TypeALNA = 0x55;
    TypeGPA = TypeGLNA = 0x55;
    break;
  case 3: /* high power on-board 8814AR and 8194AR */
    ExternalPA_2G = true;
    ExternalLNA_2G = true;
    external_pa_5g = external_lna_5g = 1;
    TypeAPA = TypeALNA = 0xaa;
    TypeGPA = TypeGLNA = 0xaa;
    break;
  case 4: /* on-board 8814AR and 8194AR */
    ExternalPA_2G = true;
    ExternalLNA_2G = true;
    external_pa_5g = external_lna_5g = 1;
    TypeAPA = 0x55;
    TypeALNA = 0xff;
    TypeGPA = TypeGLNA = 0x55;
    break;
  case 5:
    ExternalPA_2G = true;
    ExternalLNA_2G = true;
    external_pa_5g = external_lna_5g = 1;
    TypeAPA = 0xaa;
    TypeALNA = 0x5500;
    TypeGPA = TypeGLNA = 0xaa;
    break;
  case 6:
    external_lna_5g = 1;
    TypeALNA = 0;
    break;
  case 0:
  default: /* 8814AE */
    break;
  }
}

/* Kernel hal_ReadRFEType_8814A (rtl8814a_hal_init.c:1527-1568). Differs
 * from the 8812 tree in three load-bearing ways: the fallback for an
 * unprogrammed/BIT7 EFUSE 0xCA is rfe_type=1 on 8814AU (8812: 0 or the
 * PA/LNA heuristic), the programmed-value mask is 0x7F (8812: 0x3F + the
 * rfe==4 customer workaround), and the amplifier state is derived from
 * the resolved rfe_type afterwards. EEPROM_RFE_OPTION is 0xCA on both
 * chips. (CF-938AC ground truth: 0xCA = 0x01 -> rfe_type 1 either way;
 * the fallback difference bites on unburnt boards.) */
void EepromManager::Hal_ReadRFEType_8814A() {
  if (!_device.AutoloadFailFlag) {
    if (registry_priv::RFE_Type != 64 ||
        0xFF == efuse_eeprom_data[EEPROM_RFE_OPTION_8812] ||
        (efuse_eeprom_data[EEPROM_RFE_OPTION_8812] & BIT7) != 0) {
      if (registry_priv::RFE_Type != 64) {
        rfe_type = registry_priv::RFE_Type;
      } else {
        /* IS_HARDWARE_TYPE_8814AU -> 1 (the AE/PCIe case is 0). */
        rfe_type = 1;
      }
    } else {
      /* bit7==0 means RFE type defined by 0xCA[6:0] */
      rfe_type =
          (uint16_t)(efuse_eeprom_data[EEPROM_RFE_OPTION_8812] & 0x7F);
    }
  } else {
    rfe_type = (registry_priv::RFE_Type != 64)
                   ? registry_priv::RFE_Type
                   : 1; /* 8814AU autoload-fail default */
  }
  hal_ReadAmplifierType_8814A();
  _logger->info("8814A RFE Type: 0x{:X} (ext PA_5G={} LNA_5G={})", rfe_type,
                (int)external_pa_5g, (int)external_lna_5g);
}

#define EEPROM_USB_MODE_8812 0x08

void EepromManager::hal_ReadUsbType_8812AU() {
  /* if (IS_HARDWARE_TYPE_8812AU(adapterState) &&
   * adapterState.UsbModeMechanism.RegForcedUsbMode == 5) */
  {
    uint8_t reg_tmp, i, j, antenna = 0, wmode = 0;
    /* Read anenna type from EFUSE 1019/1018 */
    for (i = 0; i < 2; i++) {
      /*
        Check efuse address 1019
        Check efuse address 1018
      */
      _device.efuse_OneByteRead((ushort)(1019 - i), &reg_tmp);
      /*
        CHeck bit 7-5
        Check bit 3-1
      */
      if (((reg_tmp >> 5) & 0x7) != 0) {
        antenna = (uint8_t)((reg_tmp >> 5) & 0x7);
        break;
      } else if ((reg_tmp >> 1 & 0x07) != 0) {
        antenna = (uint8_t)((reg_tmp >> 1) & 0x07);
        break;
      }
    }

    /* Read anenna type from EFUSE 1021/1020 */
    for (i = 0; i < 2; i++) {
      /*
        Check efuse address 1021
        Check efuse address 1020
      */
      _device.efuse_OneByteRead((ushort)(1021 - i), &reg_tmp);

      /* CHeck bit 3-2 */
      if (((reg_tmp >> 2) & 0x3) != 0) {
        wmode = (uint8_t)((reg_tmp >> 2) & 0x3);
        break;
      }
    }

    _logger->info("antenna={}, wmode={}", (int)antenna, (int)wmode);
    /* Antenna == 1 WMODE = 3 RTL8812AU-VL 11AC + USB2.0 Mode */
    if (antenna == 1) {
      /* Config 8812AU as 1*1 mode AC mode. */
      version_id.RFType = RF_TYPE_1T1R;
      /* UsbModeSwitch_SetUsbModeMechOn(adapterState, FALSE); */
      /* pHalData.EFUSEHidden = EFUSE_HIDDEN_812AU_VL; */
      _logger->info("EFUSE_HIDDEN_812AU_VL");
    } else if (antenna == 2) {
      if (wmode == 3) {
        if (efuse_eeprom_data[EEPROM_USB_MODE_8812] == 0x2) {
          /* RTL8812AU Normal Mode. No further action. */
          /* pHalData.EFUSEHidden = EFUSE_HIDDEN_812AU; */
          _logger->info("EFUSE_HIDDEN_812AU");
        } else {
          /* Antenna == 2 WMODE = 3 RTL8812AU-VS 11AC + USB2.0 Mode */
          /* Driver will not support USB automatic switch */
          /* UsbModeSwitch_SetUsbModeMechOn(adapterState, FALSE); */
          /* pHalData.EFUSEHidden = EFUSE_HIDDEN_812AU_VS; */
          _logger->info("EFUSE_HIDDEN_8812AU_VS");
        }
      } else if (wmode == 2) {
        /* Antenna == 2 WMODE = 2 RTL8812AU-VN 11N only + USB2.0 Mode */
        /* UsbModeSwitch_SetUsbModeMechOn(adapterState, FALSE); */
        /* pHalData.EFUSEHidden = EFUSE_HIDDEN_812AU_VN; */
        _logger->info("EFUSE_HIDDEN_8812AU_VN");
      }
    }
  }
}

void EepromManager::efuse_ShadowRead1Byte(uint16_t Offset, uint8_t *Value) {
  *Value = efuse_eeprom_data[Offset];
}
