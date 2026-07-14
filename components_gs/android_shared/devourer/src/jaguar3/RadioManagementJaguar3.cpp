#include "RadioManagementJaguar3.h"

#include <array>
#include <cstdlib>
#include <utility>

#include "HopProf.h"         /* DEVOURER_HOP_PROF fast-retune stage timing */
#include "RateDefinitions.h" /* MGN_* rate enum */
#if defined(DEVOURER_HAVE_JAGUAR3_8822E)
#include "Hal8822e_PhyTables.h"    /* array_mp_8822e_phy_reg_pg */
#endif

extern "C" {
#include "ieee80211_radiotap.h" /* MRateToHwRate */
}

namespace jaguar3 {

RadioManagementJaguar3::RadioManagementJaguar3(
    RtlAdapter device, Logger_t logger, ChipVariant variant,
    const devourer::DeviceConfig &cfg)
    : _device{device}, _cfg{cfg}, _logger{std::move(logger)},
      _variant{variant} {}

void RadioManagementJaguar3::set_channel_bwmode(uint8_t channel,
                                              uint8_t channel_offset,
                                              ChannelWidth_t bwmode) {
  /* HT40: the RF tunes the CENTRAL channel (primary ± 2) and the BB carries a
   * primary-sub-channel index. channel_offset uses the demo's HAL_PRIME
   * convention: 1 = HT40+ (primary is the LOWER 20, secondary above), 2 = HT40-
   * (primary is the UPPER 20). Vendor pri-ch index: primary-lower -> 2,
   * primary-upper -> 1 (get_pri_ch_id / VHT_DATA_SC_20_{LOWER,UPPER}).
   *
   * 80 MHz configures the wide channel; it also supports a 40-in-80 mode where
   * an 80 MHz-tuned channel carries a 40 MHz frame (send_packet sets the
   * DATA_SC sub-channel) that a standard HT40 receiver decodes — the userspace
   * equivalent of `iw 80MHz` + a 40 MHz radiotap. `channel` is the primary
   * 20 MHz and must be the LOWEST of the 80 MHz block (e.g. 149 for
   * 149/153/157/161); the RF tunes the 80 MHz centre (+6 ch = +30 MHz) and the
   * 40 MHz frame lands on the lower 40 of the block. */
  const bool is40 = (bwmode == CHANNEL_WIDTH_40);
  const bool is80 = (bwmode == CHANNEL_WIDTH_80);
  uint8_t central, pri;
  central_and_pri(channel, channel_offset, bwmode, central, pri);

  /* Any full channel set rewrites what fast_retune caches (RF18 BW/band bits,
   * SCO, TX DFIR, AGC tables, RXBB) — invalidate; the caches only ever mirror
   * fast-path writes (the J1 RadioManagementModule pattern). */
  invalidate_fast_caches();

  /* Extended-synth channels (below-band 15..35 / above 177, freq =
   * 5000 + 5*ch up to ch 253): the RF tunes them, but the power tables,
   * calibration bands and per-channel constants are clamped from the nearest
   * characterized channel. Warn once so an unexpected retune there is
   * visible. */
  if (!_warned_uncharacterized &&
      ((channel >= 15 && channel <= 35) || channel > 177)) {
    _warned_uncharacterized = true;
    _logger->warn("channel {} is outside the characterized range — TX power "
                  "and per-channel constants extrapolated from the nearest "
                  "table entry",
                  channel);
  }

  _last_channel = channel;

  /* --- bandwidth block (config_phydm_switch_bandwidth_8822c CH_20 / CH_40).
   * The 0x810/0x9b0[7:6]/0x9b4 clock+DFIR writes are the same for 20 and 40 MHz
   * (the vendor 40M case leaves them at the table/20M values); the RF-BW field
   * 0x9b0[3:0], pilot smoothing, CCK-source, CCK-PD and the primary-sub-channel
   * bits differ. --- */
  _device.phy_set_bb_reg(0x810, 0x3ff0, 0x19b);        /* RX DFIR */
  _device.phy_set_bb_reg(0x9b0, 0xc0, 0x0);            /* small-BW off */
  _device.phy_set_bb_reg(0x9b4, 0x00000700, 0x7);      /* DAC 480M */
  _device.phy_set_bb_reg(0x9b4, 0x00700000, 0x6);      /* ADC 160M */
  if (is80) {
    /* config_phydm_switch_bandwidth CH_80: TX/RX RF BW = 0b10/0b10 = 0xa,
     * pri-ch, pilot smoothing off, subtune 0x6. */
    _device.phy_set_bb_reg(0x9b0, 0xf, 0xa);           /* TX/RX RF BW = 80M */
    _device.phy_set_bb_reg(0x9b0, 0xff00,
                           static_cast<uint32_t>(pri | (pri << 4))); /* pri ch */
    _device.phy_set_bb_reg(0xcbc, 1u << 21, 0x1);      /* pilot smoothing off */
    _device.phy_set_bb_reg(0x88c, 0xf000, 0x6);        /* subtune 80M */
    /* 80 MHz decimation/DFIR (phydm 80M path, kernel-parity — matches a
     * register dump of `iw 149 80MHz`): RX DFIR 0x810[3:0]/[19:16]=7,
     * subtune-extra 0x88c[17:16]=3, decimation 0x1944/0x4044[9:8]=3, CCK
     * source [30]=0. */
    _device.phy_set_bb_reg(0x810, 0xf, 0x7);
    _device.phy_set_bb_reg(0x810, 0xf0000, 0x7);
    _device.phy_set_bb_reg(0x88c, 0x30000, 0x3);
    _device.phy_set_bb_reg(0x1944, 0x300, 0x3);
    _device.phy_set_bb_reg(0x4044, 0x300, 0x3);
    _device.phy_set_bb_reg(0x1abc, 1u << 30, 0x0);
  } else if (is40) {
    /* TX_RF_BW field 0x9b0[1:0] (RX_RF_BW [3:2]). Stock 40M = 0b01/0b01 = 0x5.
     * DEVOURER_TX_RF_BW overrides TX_RF_BW (debug/research knob): 0b10 uses the
     * 80M RF path (drains cleanly but shifts the emission -20 MHz), for studying
     * the kernel's 40 MHz-injection stall (libc0607/rtl88x2eu#7 — a kernel-side
     * issue devourer's own 40 MHz TX does not exhibit). */
    uint32_t rfbw_nib = 0x5;
    if (_cfg.tx.rf_bw) {
      uint32_t txbw = *_cfg.tx.rf_bw & 0x3;
      rfbw_nib = txbw | (0x1 << 2); /* keep RX_RF_BW = 0b01 */
      _logger->info("Jaguar3: tx.rf_bw override — 0x9b0[3:0]=0x{:x}",
                    rfbw_nib);
    }
    _device.phy_set_bb_reg(0x1a00, 1u << 4, pri == 1 ? 1 : 0); /* CCK pri ch */
    _device.phy_set_bb_reg(0x9b0, 0xf, rfbw_nib);      /* TX/RX RF BW = 40M */
    _device.phy_set_bb_reg(0x9b0, 0xff00,
                           static_cast<uint32_t>(pri | (pri << 4))); /* pri ch */
    _device.phy_set_bb_reg(0xcbc, 1u << 21, 0x1);      /* pilot smoothing off */
    _device.phy_set_bb_reg(0x1abc, 1u << 30, 0x1);     /* CCK source 5 */
    _device.phy_set_bb_reg(0x1ae8, 1u << 31, 0x0);     /* dynamic CCK PD */
    _device.phy_set_bb_reg(0x1aec, 0xf, 0x8);
    _device.phy_set_bb_reg(0x88c, 0xf000, 0x1);        /* subtune */
  } else {
    _device.phy_set_bb_reg(0x9b0, 0xff00, 0x0);        /* clear pri-ch (20M) */
    _device.phy_set_bb_reg(0x9b0, 0xf, 0x0);           /* RF BW = 20M */
    _device.phy_set_bb_reg(0xcbc, 1u << 21, 0x0);      /* pilot smoothing */
    _device.phy_set_bb_reg(0x1abc, 1u << 30, 0x0);     /* CCK source */
    _device.phy_set_bb_reg(0x1ae8, 1u << 31, 0x1);
    _device.phy_set_bb_reg(0x1aec, 0xf, 0x6);          /* dynamic CCK PD */
    _device.phy_set_bb_reg(0x88c, 0xf000, 0x1);        /* subtune */
  }

  /* --- channel tune (RF 0x18): tune the CENTRAL channel. clear
   * [18:17]/[16]/[9:8]/[7:0], set channel; for 5 GHz set BIT16|BIT8 ALWAYS plus
   * the sub-band bit (ch>144 -> BIT18, ch>=80 -> BIT17, else none). --- */
  const bool is_c = (_variant == ChipVariant::C8822C);
  const bool is_2g = central <= 14;
  /* RF 0x18 channel/band tune. 8822C is a read-modify-write preserving [15:10]
   * and setting the BW20 bits (config_phydm_switch_channel/bandwidth_8822c);
   * 8822E configures its RF band via its own phydm path (config_channel_8822e),
   * so it composes the value from scratch (base 0). */
  const uint32_t rf18 =
      is_c ? rf18_for(central, bwmode,
                      _device.rtw_read32(
                          static_cast<uint16_t>(0x3c00 + (0x18 << 2))))
           : rf18_for(central, bwmode, 0);
  _logger->info("Jaguar3: central ch {} (pri idx {}) RF18=0x{:05x}", central,
                pri, rf18);

  if (is_c) {
    /* RF writes are bracketed by phydm_rstb_3wire_8822c (HSSI/3-wire reset):
     * (false) 0x1c90[8]=0 before, (true) 0x1c90[8]=1 + 0x1830[29]/0x4130[29]=1
     * "force update anapar" after — the latter pushes the RF/analog shadow to
     * hardware, which is required for the per-channel RF (RXBB/RF18) to reach the
     * analog front-end. */
    _device.phy_set_bb_reg(0x1c90, 1u << 8, 0x0); /* rstb_3wire(false) */
    apply_rxbb(bwmode);
    rf_window_write(0x3c00, 0x18, rf18);
    rf_window_write(0x4c00, 0x18, rf18);
    _device.phy_set_bb_reg(static_cast<uint16_t>(0x3c00 + (0xdf << 2)), 1u << 18,
                           is_2g ? 1 : 0);
    _device.phy_set_bb_reg(0x1c90, 1u << 8, 0x1);   /* rstb_3wire(true) */
    _device.phy_set_bb_reg(0x1830, 1u << 29, 0x1);  /* force update anapar (A) */
    _device.phy_set_bb_reg(0x4130, 1u << 29, 0x1);  /* force update anapar (B) */
  } else {
    /* rstb_3wire(false) before the RF writes — see the force-update-anapar note
     * below (phydm_rstb_3wire_8822e). */
    _device.phy_set_bb_reg(0x1c90, 1u << 8, 0x0);
    rf_window_write(0x3c00, 0x18, rf18);
    rf_window_write(0x4c00, 0x18, rf18);
    apply_rxbb(bwmode); /* 8822e RF 0x1a RMW — see apply_rxbb */
    _device.phy_set_bb_reg(static_cast<uint16_t>(0x3c00 + (0xdf << 2)), 1u << 18,
                           is_2g ? 1 : 0);
    /* Force-update-anapar (phydm_rstb_3wire_8822e, enable=true): 0x1c90[8]=1 +
     * 0x1830[29]/0x4130[29]=1. devourer writes RF via the BB direct-write window
     * (0x3c00/0x4c00), which — exactly as on the 8822c (#138) — does NOT push the
     * per-channel RF/analog shadow (RF18/RXBB) to the 2.4 GHz analog front-end on
     * its own; the vendor's real 3-wire (odm_set_rf_reg) writes do. Without this
     * the EU's 2G front-end never re-tunes and the receiver is deaf on 2.4 GHz
     * (5 GHz survives on the init-table defaults). The vendor keeps the brackets
     * commented in switch_channel_8822e because it uses true 3-wire writes; the
     * BB-window port needs them. */
    _device.phy_set_bb_reg(0x1c90, 1u << 8, 0x1);
    _device.phy_set_bb_reg(0x1830, 1u << 29, 0x1);
    _device.phy_set_bb_reg(0x4130, 1u << 29, 0x1);
  }

  /* Per-band RX AGC-table selection (shared with fast_retune). */
  select_agc_tables(central, bwmode);

  /* --- 2G/5G band BB (phydm switch_channel "Other BB Settings") --- */
  if (is_2g) {
    /* Enable CCK Rx IQ (phydm_cck_rxiq_8822{c,e} SET): CCK source 5 + weighting
     * [1,1]. The 8822c and 8822e writes are register-identical (0x1a9c[20]=1,
     * 0x1a14[9:8]=0), so both variants take this path — the EU needs it too. */
    _device.phy_set_bb_reg(0x1a9c, 1u << 20, 0x1);
    _device.phy_set_bb_reg(0x1a14, 0x300, 0x0);
    _device.rtw_write8(0x454,
                       static_cast<uint8_t>(_device.rtw_read8(0x454) & ~0x80));
    _device.phy_set_bb_reg(0x1a80, 1u << 18, 0x0);
    _device.phy_set_bb_reg(0x1c80, 0x3F000000, 0xF);
  } else {
    /* Disable CCK Rx IQ (phydm_cck_rxiq_8822{c,e} REVERT). Register-identical
     * across variants (0x1a9c[20]=0, 0x1a14[9:8]=3). */
    _device.phy_set_bb_reg(0x1a9c, 1u << 20, 0x0);
    _device.phy_set_bb_reg(0x1a14, 0x300, 0x3);
    _device.rtw_write8(0x454,
                       static_cast<uint8_t>(_device.rtw_read8(0x454) | 0x80));
    _device.phy_set_bb_reg(0x1a80, 1u << 18, 0x1);
    _device.phy_set_bb_reg(0x1c80, 0x3F000000, 0x22);
  }

  /* SCO tracking f_c (phydm_sco_trk_fc_setting_8822c): BB 0xc30[11:0]. Keyed
   * on the central channel (shared with fast_retune). */
  _device.phy_set_bb_reg(0xc30, 0xfff, sco_for(central));

  /* TX DFIR (phydm_tx_dfir_setting_8822c, shared with fast_retune). */
  apply_tx_dfir(central);

  /* MAC-side bandwidth + TX sub-channel (halmac cfg_bw / cfg_pri_ch_idx). */
  set_mac_bw_txsc(bwmode, pri);

  /* phydm_bb_reset_8822c: toggle the BB reset (MAC reg 0x0 BIT16, 1->0->1) to
   * (re)start the receiver after channel/BW config — the kernel does this after
   * every switch_channel; without it the RX engine never runs. */
  bb_reset_toggle();

  /* halrf_ex_dac_fifo_rst — the vendor runs this after EVERY switch_bandwidth
   * ("fix dac fifo error after TXCK setting"): 40/80 MHz change the TX clock,
   * so reset the DAC FIFO for vendor parity. 8822e only (its AFE DACK-bank soft
   * reset); the 8822c has its own path. */
  if ((is40 || is80) && _variant == ChipVariant::C8822E)
    dack_soft_rst_8822e();

  _logger->info("Jaguar3: central ch {} / {} MHz set (RF18=0x{:05x}) + BB reset",
                central, is80 ? 80 : is40 ? 40 : 20, rf18);

  /* For 5/10 MHz: channel is now tuned at 20 MHz; re-clock the baseband. */
  if (bwmode == CHANNEL_WIDTH_5 || bwmode == CHANNEL_WIDTH_10)
    set_bandwidth_dividers(bwmode);

  if (_cfg.debug.dump_canary)
    DumpCanary();
}

void RadioManagementJaguar3::DumpCanary() {
  /* Channel/BW-relevant set: TX DFIR + block enables (0x808), RX DFIR (0x810),
   * AGC bound/tables (0x828/0x18ac/0x41ac), subtune (0x88c), small-BW/RF-BW/
   * pri-ch (0x9b0), clock dividers (0x9b4), SCO (0xc30), pilot smoothing
   * (0xcbc), CCK block regs (0x1a00/0x1a14/0x1a80/0x1a9c/0x1abc/0x1ae8/0x1aec),
   * AGC 5G bound (0x1c80), 80 MHz decimation (0x1944/0x4044). MAC: BB-reset
   * word (0x0), AFE clk (0x24, 8822e NB), CCK check (0x454), DATA_SC (0x483),
   * us-ticks (0x55c/0x638), TRXPTCL (0x668). RF (via the BB direct window,
   * both paths): 0x18 channel, 0x1a RXBB (8822e), 0x3f RXBB (8822c), 0xdf.
   * Live counters (IGI 0x1d70, FA/CCA) deliberately excluded. */
  static const uint16_t bb_canary[] = {
      0x808, 0x810, 0x828,  0x88c,  0x9b0,  0x9b4,  0xc30,  0xcbc,
      0x1a00, 0x1a14, 0x1a80, 0x1a9c, 0x1abc, 0x1ae8, 0x1aec, 0x1c80,
      0x18ac, 0x41ac, 0x1944, 0x4044,
      /* TXAGC refs (runtime TX-power API): OFDM 0x18e8/0x41e8, CCK
       * 0x18a0/0x41a0, + the first per-rate diff dword. The regcheck's
       * TX+RX cell asserts 0x41e8 stays at its table default (the 8822E
       * RX-desense quirk) while path A moves with the offset. */
      0x18e8, 0x41e8, 0x18a0, 0x41a0, 0x3a00};
  static const uint16_t mac_canary[] = {0x0, 0x24, 0x454, 0x483,
                                        0x55c, 0x638, 0x668};
  static const uint8_t rf_canary[] = {0x18, 0x1a, 0x3f, 0xdf};

  _logger->info("=== DEVOURER_DUMP_CANARY (post channel-set ch={}) ===",
                unsigned(_last_channel));
  for (uint16_t a : bb_canary)
    _logger->info("BB 0x{:04x} = 0x{:08X}", a, _device.rtw_read32(a));
  for (uint16_t a : mac_canary)
    _logger->info("MAC 0x{:03x} = 0x{:08X}", a, _device.rtw_read32(a));
  for (uint8_t a : rf_canary)
    _logger->info("RF[A] 0x{:02x} = 0x{:05X}", a,
                  _device.rtw_read32(static_cast<uint16_t>(0x3c00 + (a << 2))) &
                      0xfffffu);
  for (uint8_t a : rf_canary)
    _logger->info("RF[B] 0x{:02x} = 0x{:05X}", a,
                  _device.rtw_read32(static_cast<uint16_t>(0x4c00 + (a << 2))) &
                      0xfffffu);
  _logger->info("=== END DEVOURER_DUMP_CANARY ===");
}

void RadioManagementJaguar3::central_and_pri(uint8_t channel,
                                             uint8_t channel_offset,
                                             ChannelWidth_t bwmode,
                                             uint8_t &central, uint8_t &pri) {
  central = channel;
  pri = 0; /* 0 = 20 MHz / no sub-channel */
  if (bwmode == CHANNEL_WIDTH_40) {
    if (channel_offset == 2) { central = channel - 2; pri = 1; } /* HT40- */
    else                     { central = channel + 2; pri = 2; } /* HT40+ */
  } else if (bwmode == CHANNEL_WIDTH_80) {
    central = static_cast<uint8_t>(channel + 6); /* 80 MHz centre */
    /* pri-ch index for 80 MHz is a VHT_DATA_SC_* code (get_pri_ch_id), NOT a
     * plain 0-3 slot: primary = lowest 20 => VHT_DATA_SC_20_LOWEST_OF_80MHZ = 4.
     * This lands in 0x9b0[15:8]=0x44 and REG_DATA_SC 0x483=0xa4 (kernel-parity;
     * verified against a register dump of `iw 149 80MHz`). */
    pri = 4;
  }
}

/* RF bandwidth bits in RF18: BW20 = BIT13|BIT12, BW40 = BIT13, BW80 = BIT12
 * (config_phydm_switch_bandwidth; 5/10 MHz tune the RF at 20 MHz). The base's
 * channel/band bits AND the BW bits [13:12] are cleared before merging: the
 * vendor does rf_reg18 &= ~(BIT13|BIT12) before setting them, and the RMW mask
 * must too — otherwise a prior 20 MHz tune's BIT12 survives and a 40 MHz tune
 * ends up BIT13|BIT12 = 20 MHz RF while BB/descriptor say 40 MHz, which halts
 * the TX engine (frames fill TXPKTBUF but never drain). base 0 (8822e) composes
 * the value from scratch. */
uint32_t RadioManagementJaguar3::rf18_for(uint8_t central,
                                          ChannelWidth_t bwmode,
                                          uint32_t base) {
  const uint32_t rf_bw_bits = (bwmode == CHANNEL_WIDTH_80)   ? (1u << 12)
                              : (bwmode == CHANNEL_WIDTH_40) ? (1u << 13)
                                          : ((1u << 13) | (1u << 12));
  uint32_t rf18 = (base & 0xfffffu & ~0x733ffu) | (central & 0xffu) | rf_bw_bits;
  if (central > 14) { /* 5 GHz */
    rf18 |= (1u << 16) | (1u << 8);
    if (central > 144) rf18 |= (1u << 18);
    else if (central >= 80) rf18 |= (1u << 17);
  }
  return rf18;
}

/* SCO tracking f_c value (phydm_sco_trk_fc_setting_8822c), keyed on central. */
uint32_t RadioManagementJaguar3::sco_for(uint8_t central) {
  /* 2.4 GHz first so the 5 GHz test can carry an open lower bound. */
  if (central <= 10) return 0x9aa;
  if (central == 11 || central == 12) return 0x96a;
  if (central <= 14) return 0x969; /* 13/14 */
  /* 5 GHz. Below-band ch 15..35 rides the low bucket (extended-synth clamp);
   * the upper bound is already open. */
  if (central <= 51) return 0x494;
  if (central <= 55) return 0x493;
  if (central <= 111) return 0x453;
  if (central <= 119) return 0x452;
  if (central <= 172) return 0x412;
  return 0x411;
}

/* Per-band RX AGC-table selection. 8822C: phydm_cck/ofdm_agc_tab_sel_8822c —
 * 0x18ac/0x41ac [15:12]=CCK table, [8:4]=OFDM table; 0x828[7:3]=AGC lower bound
 * (L_BND_DEFAULT_8822C 0xd; the measured ofdm_rxagc_l_bnd is not tracked).
 * 8822E: phydm_set_agc_table_8822e — same registers, 8822e-specific indices
 * (OFDM 2G=0 / 5G_LOW=1 / MID=2 / HIGH=3, CCK 2G=8; per-band, no BW20/BW40
 * split). Without this the EU keeps the init-table AGC index when it tunes to
 * 2.4 GHz, so the front-end runs at the wrong gain and the receiver is stone
 * deaf on 2G (5 GHz survives on the init-table defaults). WiFi-only coex here,
 * so the no-BT table (not the *_BTC variants). */
void RadioManagementJaguar3::select_agc_tables(uint8_t central,
                                               ChannelWidth_t bwmode) {
  auto ofdm_agc_sel = [this](uint8_t table) {
    _device.phy_set_bb_reg(0x18ac, 0x1f0, table);
    _device.phy_set_bb_reg(0x41ac, 0x1f0, table);
    _device.phy_set_bb_reg(0x828, 0xf8, 0xd);
  };
  auto cck_agc_sel = [this](uint8_t table) {
    _device.phy_set_bb_reg(0x18ac, 0xf000, table);
    _device.phy_set_bb_reg(0x41ac, 0xf000, table);
  };
  const bool bw20 = (bwmode == CHANNEL_WIDTH_20 || bwmode == CHANNEL_WIDTH_10 ||
                     bwmode == CHANNEL_WIDTH_5);
  if (_variant != ChipVariant::C8822C) {
    if (central <= 14) {
      cck_agc_sel(8);   /* CCK_8822E */
      ofdm_agc_sel(0);  /* OFDM_2G_8822E */
    } else if (central < 80) {
      ofdm_agc_sel(1);  /* OFDM_5G_LOW_BAND_8822E */
    } else if (central <= 144) {
      ofdm_agc_sel(2);  /* OFDM_5G_MID_BAND_8822E */
    } else {
      ofdm_agc_sel(3);  /* OFDM_5G_HIGH_BAND_8822E */
    }
  } else if (central <= 14) {
    cck_agc_sel(bw20 ? 5 : 4);              /* CCK_BW20=5 / CCK_BW40=4 */
    ofdm_agc_sel(bw20 ? 6 : 0);            /* OFDM_2G_BW20=6 / OFDM_2G_BW40=0 */
  } else if (central < 80) {
    ofdm_agc_sel(1);                        /* OFDM_5G_LOW */
  } else if (central <= 144) {
    ofdm_agc_sel(2);                        /* OFDM_5G_MID */
  } else {
    ofdm_agc_sel(3);                        /* OFDM_5G_HIGH */
  }
}

void RadioManagementJaguar3::rf_window_write(uint16_t base, uint8_t rfaddr,
                                             uint32_t v) {
  _device.phy_set_bb_reg(static_cast<uint16_t>(base + (rfaddr << 2)),
                         0x000fffff, v);
}

/* BW-keyed RXBB baseband-filter write, inside the caller's 3-wire bracket.
 * 8822c: RF 0x3f gated sequence — RF 0xee[2]=1, RF 0x33[4:0]=0x12, RF 0x3f,
 * RF 0xee[2]=0 (config_phydm_switch_bandwidth_8822c); BW20 = BIT4|BIT3 (0x18),
 * BW40 = BIT4 (0x10), BW80 = BIT3 (0x08).
 * 8822e: RXBB/TXBB bandwidth lives in RF 0x1a[14:10]
 * (config_phydm_switch_bandwidth_8822e RMW: rf_reg1a &= ~0x7c00 then set the
 * BW bits) — 20M = BIT11|BIT10, 40M = BIT12|BIT11, 80M = BIT13|BIT10; the 40M
 * path also does the TX_CCK_IND workaround (RF 0x1a BIT0=1, BIT16=0). The
 * 8822e has NO RF 0x3f RXBB register (that is an 8822c-only write): the 20 MHz
 * path previously wrote RF 0x3f=0x18 and left RF 0x1a unset, so the 8822e RX
 * baseband filter stayed at the wrong bandwidth and the receiver was deaf on
 * 2.4 GHz (5 GHz limped on whatever the init RF table left in RF 0x1a). Set
 * RF 0x1a for every bandwidth, matching the vendor (kernel 2G RF 0x1a = 0xc00,
 * RF 0x3f = 0x2 default). */
void RadioManagementJaguar3::apply_rxbb(ChannelWidth_t bwmode) {
  const bool is40 = (bwmode == CHANNEL_WIDTH_40);
  const bool is80 = (bwmode == CHANNEL_WIDTH_80);
  if (_variant == ChipVariant::C8822C) {
    const uint32_t rxbb = is80 ? 0x08u : is40 ? 0x10u : 0x18u;
    for (uint16_t base : {uint16_t(0x3c00), uint16_t(0x4c00)}) {
      _device.phy_set_bb_reg(static_cast<uint16_t>(base + (0xee << 2)), 0x4, 0x1);
      _device.phy_set_bb_reg(static_cast<uint16_t>(base + (0x33 << 2)), 0x1f, 0x12);
      rf_window_write(base, 0x3f, rxbb);
      _device.phy_set_bb_reg(static_cast<uint16_t>(base + (0xee << 2)), 0x4, 0x0);
    }
  } else {
    const uint32_t bb = is80  ? ((1u << 13) | (1u << 10))
                        : is40 ? ((1u << 12) | (1u << 11))
                               : ((1u << 11) | (1u << 10)); /* 20M */
    for (uint16_t base : {uint16_t(0x3c00), uint16_t(0x4c00)}) {
      uint32_t r1a = _device.rtw_read32(
          static_cast<uint16_t>(base + (0x1a << 2))) & 0xfffffu;
      r1a &= ~0x7c00u;
      r1a |= bb;
      if (is40) { r1a |= (1u << 0); r1a &= ~(1u << 16); }
      rf_window_write(base, 0x1a, r1a);
    }
  }
}

/* TX DFIR (phydm_tx_dfir_setting_8822c): BB 0x808[22:20] + [6:4]. */
void RadioManagementJaguar3::apply_tx_dfir(uint8_t central) {
  if (central <= 14) {
    _device.phy_set_bb_reg(0x808, 0x700000, central == 11 ? 0x3 : 0x1);
    _device.phy_set_bb_reg(0x808, 0x70, central == 13 ? 0x3 : 0x1);
  } else {
    _device.phy_set_bb_reg(0x808, 0x700000, 0x1);
    _device.phy_set_bb_reg(0x808, 0x70, 0x3);
  }
}

/* phydm_bb_reset_8822c: toggle the BB reset (MAC reg 0x0 BIT16, 1->0->1) so
 * the receiver (re)starts after channel/BW config. */
void RadioManagementJaguar3::bb_reset_toggle() {
  uint32_t r0 = _device.rtw_read32(0x0);
  _device.rtw_write32(0x0, r0 | (1u << 16));
  _device.rtw_write32(0x0, r0 & ~(1u << 16));
  _device.rtw_write32(0x0, r0 | (1u << 16));
}

void RadioManagementJaguar3::invalidate_fast_caches() {
  _last_sco = 0xffffffff;
  _last_dfir = 0xffffffff;
  _last_agc_key = -1;
  _rxbb_asserted = false;
  _cw_primed = false;
}

bool RadioManagementJaguar3::fast_retune(uint8_t channel,
                                         uint8_t channel_offset,
                                         ChannelWidth_t bwmode, bool cache_rf) {
  if (_last_channel == 0)
    return false; /* never tuned — unknown band, cold BW/band state */
  const bool cur_2g = _last_channel <= 14;
  if (cur_2g != (channel <= 14))
    return false; /* band change needs RFE/AGC/CCK-RxIQ — full path only */
  if (channel == _last_channel)
    return true; /* no-op hop */

  devourer::HopProf prof(_logger->events(), _cfg.debug.hop_prof, "j3",
                         channel);
  uint8_t central, pri;
  central_and_pri(channel, channel_offset, bwmode, central, pri);

  /* Compose-cache prime: every masked write costs a read+write round-trip, so
   * read the full dwords of everything the hop touches ONCE per epoch, then
   * compose bit changes in memory and write whole dwords — zero per-hop reads
   * (Jaguar1's cached-LSSI trick generalised). cache_rf=false re-primes every
   * hop (the A/B knob now measures the whole read penalty). */
  const bool is_c = (_variant == ChipVariant::C8822C);
  if (!cache_rf || !_cw_primed) {
    _cw_1c90 = _device.rtw_read32(0x1c90);
    _cw_1830 = _device.rtw_read32(0x1830);
    _cw_4130 = _device.rtw_read32(0x4130);
    _cw_r0 = _device.rtw_read32(0x0);
    _cw_c30 = _device.rtw_read32(0xc30);
    _cw_808 = _device.rtw_read32(0x808);
    _cw_rfwin_a =
        _device.rtw_read32(static_cast<uint16_t>(0x3c00 + (0x18 << 2)));
    _cw_rfwin_b =
        _device.rtw_read32(static_cast<uint16_t>(0x4c00 + (0x18 << 2)));
    _cw_primed = true;
  }
  /* RF18: merge the new central into the cached low 20 bits (8822c — the
   * primed read carries the current BW/band bits) or compose from scratch
   * (8822e); the window dword's top 12 bits ride along from the prime. */
  const uint32_t rf18 = is_c ? rf18_for(central, bwmode, _cw_rfwin_a)
                             : rf18_for(central, bwmode, 0);
  const uint32_t win_a = (_cw_rfwin_a & 0xfff00000u) | rf18;
  const uint32_t win_b = (_cw_rfwin_b & 0xfff00000u) | rf18;
  prof.mark("prime");

  /* The per-hop core of config_phydm_switch_channel: 3-wire bracket, RF18 on
   * both paths, force-update-anapar (pushes the RF/analog shadow to the
   * front-end — required on both variants, see set_channel_bwmode). The
   * band-keyed RF 0xdf write stays untouched (set by the last full set at
   * this band). The BW-keyed RXBB is re-asserted ONCE per epoch — the
   * init-time halrf calibration rewrites it after the channel set
   * (hardware-observed on the 8812EU: IQK clears the 40 MHz TX_CCK_IND bit
   * in RF 0x1a) and a fast hop must end in the full path's state; per-variant
   * order matches the full path (8822c: RXBB then RF18; 8822e: RF18 then
   * RF 0x1a). */
  _device.rtw_write32(0x1c90, _cw_1c90 & ~(1u << 8)); /* rstb_3wire(false) */
  if (is_c && !_rxbb_asserted)
    apply_rxbb(bwmode);
  _device.rtw_write32(static_cast<uint16_t>(0x3c00 + (0x18 << 2)), win_a);
  _device.rtw_write32(static_cast<uint16_t>(0x4c00 + (0x18 << 2)), win_b);
  if (!is_c && !_rxbb_asserted)
    apply_rxbb(bwmode);
  _rxbb_asserted = true;
  _device.rtw_write32(0x1c90, _cw_1c90 | (1u << 8)); /* rstb_3wire(true) */
  _device.rtw_write32(0x1830, _cw_1830 | (1u << 29)); /* force anapar (A) */
  _device.rtw_write32(0x4130, _cw_4130 | (1u << 29)); /* force anapar (B) */
  _cw_1c90 |= (1u << 8);
  _cw_1830 |= (1u << 29);
  _cw_4130 |= (1u << 29);
  _cw_rfwin_a = win_a;
  _cw_rfwin_b = win_b;
  prof.mark("bracket");

  /* Channel-keyed constants, written only when their bucket moves (invalidated
   * by every full set, so the first fast hop writes them once). */
  const bool bw20 = (bwmode == CHANNEL_WIDTH_20 || bwmode == CHANNEL_WIDTH_10 ||
                     bwmode == CHANNEL_WIDTH_5);
  const int agc_key = (central <= 14   ? 0
                       : central < 80  ? 1
                       : central <= 144 ? 2
                                        : 3) |
                      (bw20 ? 0x10 : 0);
  if (agc_key != _last_agc_key) {
    select_agc_tables(central, bwmode);
    _last_agc_key = agc_key;
  }

  const uint32_t sco = sco_for(central);
  if (sco != _last_sco) {
    _cw_c30 = (_cw_c30 & ~0xfffu) | sco;
    _device.rtw_write32(0xc30, _cw_c30);
    _last_sco = sco;
  }

  /* TX DFIR: write the full path's END state. [22:20] is the per-channel rule
   * (2G ch11) everywhere; [6:4] is the per-channel rule (2G ch13 / 5G 0x3)
   * EXCEPT in 5/10 MHz mode on the 8822e, where the narrowband re-clock owns
   * it (set_bandwidth_dividers writes [6:4]=0x1 after apply_tx_dfir, so the
   * full path always ends at 0x1 there — the ch13 rule must not undo it). */
  const bool nb =
      (bwmode == CHANNEL_WIDTH_5 || bwmode == CHANNEL_WIDTH_10);
  const uint32_t dfir_msb =
      (central <= 14) ? (central == 11 ? 0x3u : 0x1u) : 0x1u;
  const uint32_t dfir_lsb =
      (nb && !is_c) ? 0x1u
      : (central <= 14) ? (central == 13 ? 0x3u : 0x1u)
                        : 0x3u;
  const uint32_t dfir = (dfir_msb << 8) | dfir_lsb;
  if (dfir != _last_dfir) {
    /* Both nibbles composed into one dword write. */
    _cw_808 = (_cw_808 & ~0x700070u) | (dfir_msb << 20) | (dfir_lsb << 4);
    _device.rtw_write32(0x808, _cw_808);
    _last_dfir = dfir;
  }

  prof.mark("consts");

  /* BB reset every hop (the kernel runs it after every switch_channel; the RX
   * engine must relatch on the new channel) — three composed writes from the
   * primed dword. */
  _device.rtw_write32(0x0, _cw_r0 | (1u << 16));
  _device.rtw_write32(0x0, _cw_r0 & ~(1u << 16));
  _device.rtw_write32(0x0, _cw_r0 | (1u << 16));
  _cw_r0 |= (1u << 16);
  prof.mark("bbrst");

  _last_channel = channel;
  DVR_DEBUG(_logger, "Jaguar3: fast retune -> ch {} (central {}, RF18=0x{:05x})",
                 channel, central, rf18);
  /* The J1 parity lesson: the fast path must emit the canary itself — it does
   * not pass through the full path, so without this the parity diff would
   * compare the fast run's stale full-set dump. */
  if (_cfg.debug.dump_canary)
    DumpCanary();
  return true;
}

void RadioManagementJaguar3::set_mac_bw_txsc(ChannelWidth_t bw, uint8_t pri) {
  /* halmac cfg_bw_88xx: REG_WMAC_TRXPTCL_CTL 0x668, clear BIT7|BIT8, set BIT7
   * for 40 MHz, BIT8 for 80 MHz. */
  uint32_t ctl = _device.rtw_read32(0x668) & ~((1u << 7) | (1u << 8));
  if (bw == CHANNEL_WIDTH_40)      ctl |= (1u << 7);
  else if (bw == CHANNEL_WIDTH_80) ctl |= (1u << 8);
  _device.rtw_write32(0x668, ctl);

  /* halmac cfg_pri_ch_idx_88xx: REG_DATA_SC 0x483 = txsc20 | (txsc40 << 4).
   * txsc20 = primary-sub-channel index; txsc40 = 9 if idx is 1 or 3 else 10.
   * pri=0 (primary = lowest 20) gives txsc40=10; 20 MHz clears it. */
  uint8_t sc = 0;
  if (bw == CHANNEL_WIDTH_40 || bw == CHANNEL_WIDTH_80) {
    uint8_t txsc40 = (pri == 1 || pri == 3) ? 9 : 10;
    sc = static_cast<uint8_t>((pri & 0xf) | (txsc40 << 4));
  }
  _device.rtw_write8(0x483, sc);
}

void RadioManagementJaguar3::set_tx_power_ref(uint8_t idx, bool zero_diffs,
                                              bool skip_path_b_ofdm_ref) {
  /* Writes the 0x1c90 TXAGC gate below — the fast path's composed 0x1c90
   * cache would go stale. */
  invalidate_fast_caches();
  /* Override the 8822C TX-power reference on both RF paths (port of
   * rtw8822c_set_write_tx_power_ref + zeroing the per-rate diffs). idx is a
   * 7-bit power index (0..0x7f); higher = more power. Each txagc write must be
   * preceded by clearing 0x1c90[15] (the txagc-write enable gate).
   *   OFDM ref: 0x18e8 (A) / 0x41e8 (B), field [16:10] (mask 0x1fc00)
   *   CCK  ref: 0x18a0 (A) / 0x41a0 (B), field [22:16] (mask 0x7f0000)
   * Then zero the per-rate diff table at 0x3a00.. so every rate emits at the
   * reference level (uniform max for a strong, simple test/link signal). */
  if (idx > 0x7f)
    idx = 0x7f;
  auto wr = [this](uint16_t off, uint32_t mask, uint32_t v) {
    _device.phy_set_bb_reg(0x1c90, 1u << 15, 0); /* txagc write enable */
    _device.phy_set_bb_reg(off, mask, v);
  };
  wr(0x18e8, 0x1fc00, idx);
  if (!skip_path_b_ofdm_ref)
    wr(0x41e8, 0x1fc00, idx); /* path B — 8822E TX+RX RX-desense hazard */
  wr(0x18a0, 0x7f0000, idx);
  wr(0x41a0, 0x7f0000, idx);
  if (zero_diffs)
    for (uint16_t off = 0x3a00; off <= 0x3a7c; off += 4)
      wr(off, 0xffffffff, 0x0);
  _logger->info("Jaguar3: TX power reference set to 0x{:02x} ({}) "
                "(per-rate diffs {})",
                idx,
                skip_path_b_ofdm_ref ? "path A + CCK B; 0x41e8 kept"
                                     : "both paths",
                zero_diffs ? "zeroed" : "kept");
}

/* Map a phy_reg_pg pseudo-address to the four MGN_* rates it encodes (byte i of
 * the packed value -> Rate[i]). Ported from _phy_get_rate_values_of_txpwr_by_rate
 * (hal_com_phycfg.c). Returns false for CCK/unknown addresses (handled via the
 * CCK reference, not the OFDM/HT/VHT 0x3a00 diff table). Path-B addresses
 * (0xE20..) carry identical values to path A, so only the 0xC2x/0xC3x/0xC4x set
 * is needed to fill the (path-shared) 0x3a00 table. */
#if defined(DEVOURER_HAVE_JAGUAR3_8822E)
static bool pg_addr_to_rates(uint32_t addr, std::array<uint8_t, 4> &rates) {
  switch (addr) {
  case 0xC24: rates = {MGN_6M, MGN_9M, MGN_12M, MGN_18M}; return true;
  case 0xC28: rates = {MGN_24M, MGN_36M, MGN_48M, MGN_54M}; return true;
  case 0xC2C: rates = {MGN_MCS0, MGN_MCS1, MGN_MCS2, MGN_MCS3}; return true;
  case 0xC30: rates = {MGN_MCS4, MGN_MCS5, MGN_MCS6, MGN_MCS7}; return true;
  case 0xC34: rates = {MGN_MCS8, MGN_MCS9, MGN_MCS10, MGN_MCS11}; return true;
  case 0xC38: rates = {MGN_MCS12, MGN_MCS13, MGN_MCS14, MGN_MCS15}; return true;
  case 0xC3C:
    rates = {MGN_VHT1SS_MCS0, MGN_VHT1SS_MCS1, MGN_VHT1SS_MCS2,
             MGN_VHT1SS_MCS3};
    return true;
  case 0xC40:
    rates = {MGN_VHT1SS_MCS4, MGN_VHT1SS_MCS5, MGN_VHT1SS_MCS6,
             MGN_VHT1SS_MCS7};
    return true;
  case 0xC44:
    rates = {MGN_VHT1SS_MCS8, MGN_VHT1SS_MCS9, MGN_VHT2SS_MCS0,
             MGN_VHT2SS_MCS1};
    return true;
  case 0xC48:
    rates = {MGN_VHT2SS_MCS2, MGN_VHT2SS_MCS3, MGN_VHT2SS_MCS4,
             MGN_VHT2SS_MCS5};
    return true;
  case 0xC4C:
    rates = {MGN_VHT2SS_MCS6, MGN_VHT2SS_MCS7, MGN_VHT2SS_MCS8,
             MGN_VHT2SS_MCS9};
    return true;
  default:
    return false; /* 0xC20 = CCK, or a path-B / unknown address */
  }
}
#endif /* DEVOURER_HAVE_JAGUAR3_8822E */

void RadioManagementJaguar3::apply_tx_power_refs_8822e(
    uint8_t ref_a, uint8_t ref_b, bool skip_path_b_ofdm_ref) {
  invalidate_fast_caches(); /* writes the 0x1c90 TXAGC gate below */
  /* Clamp to the 7-bit ref field — the masked BB write truncates mod 128. */
  if (ref_a > 0x7f)
    ref_a = 0x7f;
  if (ref_b > 0x7f)
    ref_b = 0x7f;
  auto wr = [this](uint16_t off, uint32_t mask, uint32_t v) {
    _device.phy_set_bb_reg(0x1c90, 1u << 15, 0); /* txagc write enable */
    _device.phy_set_bb_reg(off, mask, v);
  };
  wr(0x18e8, 0x1fc00, ref_a);   /* path A OFDM/HT/VHT ref */
  if (!skip_path_b_ofdm_ref)
    wr(0x41e8, 0x1fc00, ref_b); /* path B — RX-desense hazard, see header */
  wr(0x18a0, 0x7f0000, ref_a);  /* CCK ref */
  wr(0x41a0, 0x7f0000, ref_b);
}

void RadioManagementJaguar3::apply_power_by_rate_8822e(
    uint8_t channel, uint8_t ref_a, uint8_t ref_b, bool skip_path_b_ofdm_ref) {
  invalidate_fast_caches(); /* writes the 0x1c90 TXAGC gate below */
  /* Clamp to the 7-bit ref fields (masked BB writes truncate mod 128). */
  if (ref_a > 0x7f)
    ref_a = 0x7f;
  if (ref_b > 0x7f)
    ref_b = 0x7f;
#if defined(DEVOURER_HAVE_JAGUAR3_8822E)
  /* Port of the phy_reg_pg (power-by-rate) apply that devourer's table walk
   * skips. The 8822e TXAGC is ref + per-rate diff: the OFDM/HT/VHT reference
   * lives in 0x18e8 (A)/0x41e8 (B), and the signed per-rate diff table at
   * 0x3a00 + (hw_rate & 0xfc) (4 rates/word, byte = hw_rate & 3) — see
   * phydm_hal_api8822e config_phydm_write_txagc_{ref,diff}. Without it every
   * rate emits at the flat reference, so the robust low rates the kernel boosts
   * (e.g. MCS0 = MCS7 + 6 dB) come out ~2 dB low.
   *
   * phy_reg_pg holds the per-rate absolute indices (PHY_REG_PG_EXACT_VALUE). We
   * program diff[rate] = pg[rate] - pg[MCS7] so the reference (ref_base, tuned to
   * match the kernel's MCS7 level) anchors MCS7 at diff 0 and the by-rate spread
   * matches the kernel (MCS0 -> ref_base + 24 = the kernel's +6 dB). */
  const uint32_t band = channel <= 14 ? 0u : 1u;
  const uint32_t *pg = array_mp_8822e_phy_reg_pg;
  const uint32_t n = array_mp_8822e_phy_reg_pg_len;
  auto get_val = [](uint32_t v, int i) -> uint8_t {
    return static_cast<uint8_t>((v >> (i * 8)) & 0xff);
  };

  /* Pass 1: find the MCS7 anchor value (addr 0xC30 byte 3) for this band. */
  uint8_t anchor = 0;
  for (uint32_t i = 0; i + 6 <= n; i += 6) {
    if (pg[i] == band && pg[i + 1] == 0 && (pg[i + 3] & 0xffff) == 0xC30) {
      anchor = get_val(pg[i + 5], 3);
      break;
    }
  }
  if (anchor == 0) {
    _logger->info("Jaguar3: phy_reg_pg has no MCS7 anchor for band {} — "
                  "skipping power-by-rate", band);
    return;
  }

  /* Reference base on both paths (OFDM ref 0x18e8/0x41e8, field [16:10]). */
  auto wr = [this](uint16_t off, uint32_t mask, uint32_t v) {
    _device.phy_set_bb_reg(0x1c90, 1u << 15, 0); /* txagc write enable */
    _device.phy_set_bb_reg(off, mask, v);
  };
  wr(0x18e8, 0x1fc00, ref_a);     /* path A OFDM/HT/VHT ref */
  if (!skip_path_b_ofdm_ref)
    wr(0x41e8, 0x1fc00, ref_b);   /* path B — RX-desense hazard, see header */
  wr(0x18a0, 0x7f0000, ref_a);    /* CCK ref (2.4G) */
  wr(0x41a0, 0x7f0000, ref_b);

  /* Pass 2: write the per-rate diff table for this band (path-A entries; the
   * 0x3a00 table is path-shared and path-B pg values are identical). */
  int groups = 0;
  for (uint32_t i = 0; i + 6 <= n; i += 6) {
    if (pg[i] != band || pg[i + 1] != 0)
      continue;
    std::array<uint8_t, 4> rates{};
    if (!pg_addr_to_rates(pg[i + 3] & 0xffff, rates))
      continue;
    const uint32_t value = pg[i + 5];
    uint32_t diff_word = 0;
    for (int j = 0; j < 4; ++j) {
      int diff = static_cast<int>(get_val(value, j)) - static_cast<int>(anchor);
      diff_word |= static_cast<uint32_t>(diff & 0x7f) << (j * 8);
    }
    uint8_t hw0 = MRateToHwRate(rates[0]);
    uint16_t reg = static_cast<uint16_t>(0x3a00 + (hw0 & 0xfc));
    wr(reg, 0xffffffff, diff_word);
    ++groups;
  }
  _logger->info("Jaguar3: applied phy_reg_pg power-by-rate (band {}, {} rate "
                "groups, ref A=0x{:02x} B=0x{:02x}, MCS7 anchor 0x{:02x})",
                band, groups, ref_a, ref_b, anchor);
#else
  (void)channel;
  (void)ref_a;
  (void)ref_b;
#endif
}

void RadioManagementJaguar3::set_bandwidth_dividers(ChannelWidth_t bwmode) {
  /* Writes 0x808 (8822e shaping) + the BB-reset word — stale-proof the fast
   * path's composed caches. */
  invalidate_fast_caches();
  /* Narrowband baseband underclock — the Jaguar3-only payoff. Applies ONLY the
   * clock-divider / small-BW / RX-DFIR delta from config_phydm_switch_bandwidth
   * _8822c / _8822e, on top of an already-tuned channel (this re-clocks the
   * baseband without re-running the RF channel tune). 10 MHz halves the DAC/ADC
   * clock, 5 MHz quarters it; radiotap stays 20 MHz so only an SDR sees the
   * change. The DAC-divider codes differ per variant for the SAME clock — see
   * the encoding table in the header. */
  const bool is_e = (_variant == ChipVariant::C8822E);
  /* One DAC-code table for BOTH variants — the 8822c values. The 8822e
   * vendor phydm ships different codes (5M=0x2/10M=0x4/20M=0x6, commented as
   * the same clocks) but 0x2 is DEAD on real 8812EU silicon (TX keys up,
   * frames drain, nothing airs — SDR-bisected via DEVOURER_NB_DAC sweep:
   * only 0x4 and 0x6 emit the 5 MHz lobe). libc0607 hit the same wall and
   * patched the vendor table to these values ("Fix DAC clock setting for
   * 5MHz BW, from 8812cu driver"), which the OpenHD ecosystem has been
   * running at 10 MHz on 8812EU since. */
  uint8_t small_bw, dac, adc;
  uint16_t dfir;
  switch (bwmode) {
  case CHANNEL_WIDTH_10:
    small_bw = NB_SMALLBW_10M; dac = 0x6; adc = 0x5; dfir = 0x2ab; break;
  case CHANNEL_WIDTH_5:
    small_bw = NB_SMALLBW_5M;  dac = 0x4; adc = 0x4; dfir = 0x2ab; break;
  default: /* CHANNEL_WIDTH_20 — restore full-rate clocks */
    small_bw = 0x0;            dac = 0x7; adc = 0x6; dfir = 0x19b; break;
  }
  /* DEVOURER_NB_DAC=0xN — debug knob: force the DAC-divider code. For mirror
   * A/B experiments and for empirically mapping the (undocumented) divider
   * field encoding; not for normal use. */
  if (_cfg.tuning.nb_dac) {
    dac = *_cfg.tuning.nb_dac & 0x7;
    _logger->info("Jaguar3: tuning.nb_dac override — DAC code {:#x}", dac);
  }
  _device.phy_set_bb_reg(R_RX_DFIR_8822C, 0x3ff0, dfir);
  /* 8822e vendor parity: the small-BW write also zeroes the TX/RX pri-ch
   * fields [15:8] (mask 0xffc0); the SDR-validated 8822c path keeps its
   * original [7:6]-only write. */
  _device.phy_set_bb_reg(R_SMALL_BW_8822C, is_e ? 0xffc0 : 0xc0, small_bw);
  _device.phy_set_bb_reg(R_CLK_DIV_8822C, 0x00000700, dac);   /* 0x9b4[10:8] */
  _device.phy_set_bb_reg(R_CLK_DIV_8822C, 0x00700000, adc);   /* 0x9b4[22:20] */

  if (is_e) {
    /* halmac cfg_mac_clk_88xx — the MAC-side half of the narrowband re-clock:
     * MAC clock select at REG_AFE_CTRL1 0x24[21:20] (0=80M, 2=20M, 3=the
     * dedicated 20M_BW_5 mode) + the TSF/EDCA microsecond-tick clocks
     * (0x55c/0x638, in MHz). The vendor applies this for BW5/BW10 on every
     * Jaguar3; the SDR-validated 8822c NB path works without it, so it is
     * applied on the 8822e only, where 5 MHz TX never reaches the air without
     * the 20M_BW_5 MAC mode. */
    uint8_t clk_sel, ustime;
    if (bwmode == CHANNEL_WIDTH_5)       { clk_sel = 3; ustime = 20; }
    else if (bwmode == CHANNEL_WIDTH_10) { clk_sel = 2; ustime = 20; }
    else                                 { clk_sel = 0; ustime = 80; }
    uint32_t afe = _device.rtw_read32(0x24);
    afe = (afe & ~((1u << 20) | (1u << 21))) |
          (static_cast<uint32_t>(clk_sel) << 20);
    _device.rtw_write32(0x24, afe);
    _device.rtw_write8(0x55c, ustime);
    _device.rtw_write8(0x638, ustime);

    /* config_phydm_switch_bandwidth_8822e narrowband extras the 8822c recipe
     * doesn't have: CFR (crest-factor reduction) params + TX triangular-shape
     * spectrum shaping OFF for 5/10 MHz; the 20 MHz restore re-applies the
     * band-dependent shaping (phydm_tx_triangular_shap_cfg_8822e). */
    if (bwmode == CHANNEL_WIDTH_5 || bwmode == CHANNEL_WIDTH_10) {
      _device.phy_set_bb_reg(0xa74, 1u << 31, 0x0);
      _device.phy_set_bb_reg(0xa74, 0x3ff, 0x15);
      _device.phy_set_bb_reg(0xa74, 0xffc00, 0x13);
      _device.phy_set_bb_reg(0x808, 0x70, 0x1);
      _device.phy_set_bb_reg(0x80c, 0xf, 0x5);
      _device.phy_set_bb_reg(0x81c, 0xff, 0x0);
      _device.phy_set_bb_reg(0x81c, 0xf000000, 0x0);
      _device.phy_set_bb_reg(0x8a0, 0xf0000000, 0x0);
    } else {
      const bool is_2g = (_last_channel != 0 && _last_channel <= 14);
      _device.phy_set_bb_reg(0xa74, 1u << 31, 0x1);
      _device.phy_set_bb_reg(0x808, 0x70, 0x3);
      if (is_2g) {
        _device.phy_set_bb_reg(0xa74, 0x3ff, 0x15);
        _device.phy_set_bb_reg(0xa74, 0xffc00, 0x13);
        _device.phy_set_bb_reg(0x80c, 0xf, 0x5);
        _device.phy_set_bb_reg(0x81c, 0xff, 0xff);
        _device.phy_set_bb_reg(0x81c, 0xf000000, 0x0);
        _device.phy_set_bb_reg(0x8a0, 0xf0000000, 0xb);
      } else {
        _device.phy_set_bb_reg(0xa74, 0x3ff, 0x3f);
        _device.phy_set_bb_reg(0xa74, 0xffc00, 0x3f);
        _device.phy_set_bb_reg(0x80c, 0xf, 0x8);
        _device.phy_set_bb_reg(0x81c, 0xff, 0x55);
        _device.phy_set_bb_reg(0x81c, 0xf000000, 0x7);
        _device.phy_set_bb_reg(0x8a0, 0xf0000000, 0x0);
      }
    }
  }

  /* phydm_bb_reset: toggle MAC 0x0 BIT16 (1->0->1) so the receiver/DFE
   * relatches at the new sample rate — without it the re-clock doesn't take. */
  uint32_t r0 = _device.rtw_read32(0x0);
  _device.rtw_write32(0x0, r0 | (1u << 16));
  _device.rtw_write32(0x0, r0 & ~(1u << 16));
  _device.rtw_write32(0x0, r0 | (1u << 16));

  if (is_e) {
    /* phydm_igi_toggle_8822e: bump IGI down/up so the BB HW issues 3-wire and
     * the RF re-enters RX — the BB does not send 3-wire automatically on a
     * path/channel/BW config change. */
    uint32_t igi = _device.rtw_read32(0x1d70);
    _device.rtw_write32(0x1d70, igi - 0x202);
    _device.rtw_write32(0x1d70, igi);

    /* halrf_ex_dac_fifo_rst — "fix dac fifo error after TXCK setting": the
     * DAC FIFO can come out of a TX-clock change misaligned and emit spectral
     * images (the OpenHD 5 MHz "mirror"); soft-reset the AFE DACK banks. */
    dack_soft_rst_8822e();
  }

  const char *name = bwmode == CHANNEL_WIDTH_10  ? "10 MHz"
                     : bwmode == CHANNEL_WIDTH_5 ? "5 MHz"
                                                 : "20 MHz";
  _logger->info("Jaguar3: baseband re-clocked to {} (small_bw={} DAC={} ADC={} "
                "DFIR=0x{:03x})", name, small_bw, dac, adc, dfir);
}

/* halrf_write_check_afe_8822e: an AFE write must be confirmed (the bank reads
 * back non-zero); retry up to 100x past an IO race. Faithful duplicate of
 * Halrf8822e::write_check_afe (see the header note on the shared-base
 * follow-up). */
void RadioManagementJaguar3::write_check_afe_8822e(uint16_t add, uint32_t data) {
  const uint32_t wd = (add == 0x3800 || add == 0x3900) ? data : 0xee32001fu;
  const uint16_t wa = ((add >> 8) == 0x38) ? 0x3800 : 0x3900;
  for (uint32_t count = 0; count < 100; ++count) {
    _device.rtw_write32(0x2dd4, 0x0);
    _device.rtw_write32(add, data);
    _device.rtw_write32(add, data);
    _device.rtw_write32(0x2dd4, 0x0);
    if (_device.rtw_read32(wa) != 0x0)
      return;
    _device.rtw_write32(wa, wd);
    _device.rtw_write32(wa, wd);
  }
}

/* halrf_dack_soft_rst_8822e — soft-reset all four AFE DACK banks (S0/S1 x I/Q). */
void RadioManagementJaguar3::dack_soft_rst_8822e() {
  write_check_afe_8822e(0x3800, 0xee30001fu);
  write_check_afe_8822e(0x3800, 0xee32001fu);
  write_check_afe_8822e(0x382c, 0xee30001fu);
  write_check_afe_8822e(0x382c, 0xee32001fu);
  write_check_afe_8822e(0x3900, 0xee30001fu);
  write_check_afe_8822e(0x3900, 0xee32001fu);
  write_check_afe_8822e(0x392c, 0xee30001fu);
  write_check_afe_8822e(0x392c, 0xee32001fu);
}

} /* namespace jaguar3 */
