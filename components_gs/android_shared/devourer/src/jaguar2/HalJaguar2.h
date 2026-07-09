#ifndef HAL_JAGUAR2_H
#define HAL_JAGUAR2_H

#include <atomic>
#include <cstdint>
#include <memory>

#include "logger.h"
#include "RtlAdapter.h"
#include "RxSense.h"
#include "ChipVariant.h"
#include "Jaguar2PhyTables.h"

namespace jaguar2 {

/* HalJaguar2 — RTL8822B / RTL8821C (Jaguar2) chip bring-up. Jaguar2 sibling of
 * src/jaguar3/HalJaguar3, using the HalMAC power sequence + the shared
 * check_positive PHY-table walker, with per-variant PHY tables and calibration
 * selected via ChipVariant. */
class HalJaguar2 {
public:
  HalJaguar2(RtlAdapter device, Logger_t logger,
             ChipVariant variant = ChipVariant::C8822B,
             const devourer::DeviceConfig &cfg = {});

  /* Card-enable power sequence (card-disable -> card-emulation -> active),
   * transcribed from halmac card_en_flow_8822b (USB/ALL entries). Runs
   * power_off() first so the MAC is reset from any prior (kernel-left) state. */
  void power_on();
  /* Card-disable power sequence (active -> card-emulation -> card-disable). */
  void power_off();

  /* Read REG_SYS_CFG1 (0x00F0) and decode cut / vendor / 2T2R. */
  void read_chip_version();

  struct ChipVersion {
    uint8_t cut = 0;     /* 0=A,1=B,2=C,... */
    uint8_t vendor = 0;  /* 0=TSMC,1=SMIC,2=UMC */
    uint8_t rf_2t2r = 0; /* 1 = 2T2R */
    bool test_chip = false;
  };
  ChipVersion chip_version() const { return _ver; }

  /* Read the EFUSE logical map (standard 88xx OneByteRead + 2-byte-header
   * decode) and return the RFE type at EEPROM_RFE_OPTION_8822B (0xCA). The
   * phydm BB/AGC/RF tables are gated on rfe_type; a wrong value leaves the RX
   * front-end unconfigured. */
  uint8_t read_efuse_rfe();

  /* Walk the physical EFUSE header chain and decode it into a logical map
   * (map[i] = 0xFF where unprogrammed). Shared by read_efuse_rfe and
   * apply_tx_power. `dump`=true logs the raw physical bytes + a few key fields. */
  void read_efuse_logical_map(uint8_t *map, uint16_t map_size, bool dump);

  /* One logical EFUSE byte from the cached map (lazy-reads the map on first
   * use). 0xFF where unprogrammed / out of range. Used for the crystal-cap
   * default (logical 0xB9). */
  uint8_t efuse_logical_byte(uint16_t off);

  /* Program the per-rate TXAGC (0x1d00 path A / 0x1d80 path B) from the EFUSE
   * power-by-rate calibration for `channel` at bandwidth `bw` (0=20/1=40/2=80;
   * 5/6 = 5/10 MHz narrowband, folded to the 20 MHz column — the RF runs in
   * 20 MHz mode and the regulatory tables have no narrowband rows) —
   * the efuse-calibrated level the kernel uses. Without it the TXAGC sits at the
   * hot BB-table default which overdrives high-order QAM (MCS5/7) into PA
   * compression. Ports phy_get_pg_txpwr_idx (base + per-BW/Nss diff) +
   * config_phydm_write_txagc_8822b. Both bands. On the 8822B this covers the
   * VHT sections too (vendor parity: phy_get_pg_txpwr_idx computes VHT by
   * stream count only, so VHT1SS/VHT2SS ride the HT 1SS/2SS bases with the
   * VHT regulatory clamp — hw_rate 0x2c-0x3f).
   *
   * `offset_steps` is the runtime TX-power offset (TXAGC index steps, 0.5 dB
   * each) behind IRtlDevice::SetTxPowerOffsetQdb: folded AFTER the min() with
   * the regulatory table, clamped only at the 6-bit rails (the saturation
   * flags below record rail hits — reset per apply). */
  void apply_tx_power(uint8_t channel, uint8_t bw = 0, uint8_t rfe_type = 0,
                      int offset_steps = 0);

  /* Rail-hit flags from the last apply_tx_power/flat-compose (see
   * apply_tx_power). Atomic so a state snapshot may read them cross-thread. */
  bool txpwr_saturated_low() const { return _txpwr_sat_low; }
  bool txpwr_saturated_high() const { return _txpwr_sat_high; }
  void set_txpwr_saturation(bool lo, bool hi) {
    _txpwr_sat_low = lo;
    _txpwr_sat_high = hi;
  }

  /* Software shadow of the last path-A TXAGC write for the representative
   * rates (CCK 1M / OFDM 6M / HT MCS7). The Jaguar2 TXAGC block
   * (0x1d00/0x1d80) is WRITE-ONLY — reads return 0 (hardware-verified on
   * 8822BU + 8821CU) — so GetTxPowerState reports these with
   * hw_readback=false, like the 8814A's packed TXAGC port. -1 = never
   * written (TXAGC still at the BB-table default). */
  void txagc_shadow(int &cck, int &ofdm, int &mcs7) const {
    cck = _txagc_shadow_cck;
    ofdm = _txagc_shadow_ofdm;
    mcs7 = _txagc_shadow_mcs7;
  }
  void set_txagc_shadow_flat(int idx) {
    _txagc_shadow_cck = idx;
    _txagc_shadow_ofdm = idx;
    _txagc_shadow_mcs7 = idx;
  }

  /* Chip thermal meter: raw = RF[A] 0x42[15:10] (a plain direct-window read —
   * the vendor 8822B/8821C halrf reads it with no trigger), baseline = the
   * EFUSE thermal calibration at logical 0xBA (EEPROM_THERMAL_METER_8822B /
   * _8821C — same offset both variants; 0xFF = unprogrammed). Reads the
   * cached logical map (lazy-read on first use, like apply_tx_power). */
  void read_thermal(uint8_t &raw, uint8_t &baseline);

  /* Apply the 8822B BB (phy_reg), AGC (agc_tab) and RF (radioa/radiob) phydm
   * tables via the shared check_positive walker, bracketed by the OFDM/CCK
   * block disable/enable (config_phydm_parameter_init_8822b PRE/POST). Mirrors
   * rtl8822b_phy.c: PRE -> init_bb_reg -> init_rf_reg -> POST. rfe_type selects
   * the conditional table blocks. */
  void apply_bb_rf_agc_tables(uint8_t rfe_type);

  /* Set RF channel + bandwidth (config_phydm_switch_channel_8822b +
   * config_phydm_switch_bandwidth_8822b): RF18 tune, band AGC/fc/CCK-filter,
   * RFE antenna pins, RX-path + IGI toggle. bw: 0=20/1=40/2=80 MHz, 5/6 =
   * 5/10 MHz narrowband (raw ChannelWidth_t values) — a baseband ADC/DAC
   * re-clock packed into the 0x8ac dword (small-BW [7:6] = 1/2) plus
   * 0x8c4[30]=0 / 0x8c8[31]=1; the RF synth stays in its 20 MHz mode, so the
   * RF18 BW bits equal the 20 MHz encoding. primary_ch_idx = sub-channel index
   * for 40/80 (the vendor primary_ch_idx; from SelectedChannel.ChannelOffset).
   * rfe_type selects the RFE-pin table and the BW80 extra writes; rf_2t2r
   * drives path-B writes. */
  void set_channel_bw(uint8_t channel, uint8_t bw, uint8_t rfe_type,
                      uint8_t primary_ch_idx = 0);

  /* halrf kfree (efuse power/PA-bias trim), 8822B port. kfree_init reads the
   * PPG physical-efuse bytes (2G/5G per-band-group TX gain trims
   * 0x3DB..0x3EE, 2G PA bias 0x3D5/0x3D6) and applies the PA-bias RF LUT
   * correction (RF 0x51/0x52 -> 0x3f via the 0xef[10] window, entries 0/1/3
   * — write-only LUT state a readable-register diff can never see).
   * kfree_apply programs the per-channel-group TX gain trim (RF 0xde/0x65/
   * 0x55), phydm_config_kfree parity; run by every full channel set. */
  void kfree_init();
  void kfree_apply(uint8_t channel);

  /* phydm_spur_calibration_8822b port: clear the NBI/CSI notch state on every
   * channel set, and on the fixed spur-channel set (2G 5-8/13 @20M, 3-11
   * @40M; 5G 153/161/54/118/151/159/58/122/155) PSD-scan the chip's own
   * clock spur and program the NBI notch + CSI mask above threshold. 8822B
   * only. `cch` = central channel (notch center frequency). */
  void spur_calibration(uint8_t channel, uint8_t cch, uint8_t bw);

  /* Lean intra-band, same-bandwidth hop retune — the Jaguar2 FastRetune core
   * (see docs/frequency-hopping.md), variant-dispatched like set_channel_bw.
   * Only the per-hop essentials run: one cached full-register RF18 write per
   * path (a compose cache primed on the first fast hop collapses the full
   * path's read-modify-write rounds — the steady hop is write-only), plus the
   * channel-keyed constants (AGC table index, CFO-tracking fc, 8822B RF 0xBE
   * VCO band / ch144 RF 0xDF flag / 2G spur regs, 8821C 2G CCK filter) as
   * composed writes only when their bucket moves. The vendor switch_channel
   * tail (RF 0xb8 / RX-path / IGI toggles) stays in the full path only — a
   * hop does not need it (hardware-measured on both variants, both
   * directions: identical hopping-RX catch rate and hopping-TX delivery with
   * and without, no decay over repeated kickless retunes). Everything
   * bandwidth-keyed (0x8ac/0x8c4/0x8c8 block — including the 5/10 MHz
   * narrowband re-clock state, whose RF18 BW bits equal the 20 MHz encoding,
   * so NB survives fast hops with no divider re-cache — RX DFIR, CCA
   * thresholds) and band-keyed (RFE pins, 8821C switch-band/RF-set block)
   * stays untouched — set by the last full set at this BW/band. Returns false (chip untouched)
   * on a band change or when the radio was never tuned; the caller falls back
   * to the full set_channel_bw. */
  /* Lean same-channel bandwidth toggle between 20 MHz and 5/10 MHz narrowband
   * (8822B only for now). Replays only the BW-dependent re-clock delta
   * (0x8ac/0x8c4/0x8c8, the 8822B RF18 re-latch edge, the CCK trio, BB reset)
   * from the cached channel state — skipping the RF channel tune, RF18 read,
   * CCA/DFIR tail, TX power, and calibrations. Returns false when the toggle
   * involves 40/80 MHz, the 8821C variant, or a cold/uncached channel. */
  bool fast_set_bandwidth(uint8_t bw);
  bool fast_retune(uint8_t channel, uint8_t bw, uint8_t primary_ch_idx,
                   bool cache_rf);

  /* Channel/BW register-canary dump for tests/hop_parity_check.sh — the same
   * grep format as the Jaguar1/Jaguar3 DumpCanary. Emitted by both the full
   * and fast channel paths when DEVOURER_DUMP_CANARY is set. Live registers
   * (IGI 0xc50/0xe50, FA counters) deliberately excluded. */
  void DumpCanary();

  /* Enable the MAC RX engine (CR MACRXEN + promiscuous RCR for monitor). */
  void enable_rx();

  /* config_phydm_trx_mode_8822b: RF mode table (0xc08/0xe08 = 0x3231), TX/RX
   * antenna-path HW-block enable, RX-path config, and the RF 0xef/0x33/0x3e/
   * 0x3f mode-table sync. Puts the RF paths into TRX mode — without it the RF
   * never enters RX and the chip delivers no frames. Called before the channel
   * set. 2T2R -> tx=rx=AB (0x3). */
  void config_trx_mode();

  /* halrf LC calibration (_phy_lc_calibrate_8822b): locks the RF LO tank at the
   * current channel. Without it the RF synthesizer does not lock and the
   * front-end receives nothing. Run after the channel is set. On C8821C this
   * dispatches to do_lck_8821c() (a different RF sequence). */
  void do_lck();

  /* Grant the antenna to WLAN (WiFi-only coex). 8822B is a WiFi+BT combo: on
   * power-up the PTA/antenna switch can be owned by BT, leaving the WL RX
   * front-end deaf (correct BB/RF config but no energy reaches the LNA). Ported
   * verbatim from ex_hal8822b_wifi_only_hw_config: switch to WL-side controller,
   * gnt_wl=1/gnt_bt=0, antenna mux to WL. Must run before enable_rx. */
  void coex_wlan_only(bool is_5g);

  /* phydm_rfe_8822b_init: RFE chip-top-mux init (0x64/0x4c/0x40 top mux, 0x1990
   * s0/s1 select, 0x974 in/out). The kernel runs this from odm_dm_init
   * (phydm_rfe_init) after BB/RF config; devourer skipped it, leaving 0x1990 at 0
   * (kernel = 0xc30) so the RFE antenna-mux source select was never applied. */
  void rfe_init();

  /* rtl8822b_phy_bf_init: MU-MIMO / TXBF init. Only the grouping-bitmap write
   * (0x1c94 = 0xafffafff, overriding the BB-table default 0x5fff5fff) affects a
   * non-beamformed frame, but ported whole for parity. Runs after rfe_init. */
  void bf_init();
  /* rtl8821c_phy_bf_init: 8821C MU-MIMO/TXBF MAC setup (0x14C0/0x167C/0x1680/
   * 0x042F/0x045F/0x6DF + 0x1c94). Replaces the 8822B bf_init (which only wrote
   * 0x1c94 and missed the MU/TXBF MAC registers). */
  void bf_init_8821c();

  /* Force a flat per-rate TXAGC power index (0..63) on both paths — a debug /
   * SDR-visibility knob (DEVOURER_TX_PWR). 8822B TXAGC is 4 rates packed per
   * dword at 0x1d00 (path A) / 0x1d80 (path B). */
  void set_tx_power_flat(uint8_t idx);

  /* One DIG (dynamic initial gain) iteration: read the per-window false-alarm
   * count (OFDM 0xf48 + CCK 0xa5c), reset the counters, and nudge IGI
   * (0xc50/0xe50) up when FA is high or down when FA is low, bounded
   * [0x1c, 0x3e]. Ported from phydm phydm_new_igi_by_fa (fa_th {250,500,750},
   * step {2,1,2}). Called on a ~100 ms cadence by the orchestrator's DIG thread
   * so weak signals are caught (low IGI) without drowning in false alarms. */
  void dig_step();
  uint8_t dbg_igi() { return static_cast<uint8_t>(_device.rtw_read8(0x0c50) & 0x7f); }
  uint32_t dbg_last_fa() const { return _last_fa; }
  /* Frame-free RX energy snapshot from the last dig_step window (FA/CCA/IGI).
   * dig_step already reads+resets these counters on its ~100 ms cadence, so the
   * energy poller piggybacks it rather than racing the reset. Invalid until
   * dig_step has run at least once (i.e. requires the DIG thread — default on). */
  RxEnergy last_energy() const { return _energy; }

  /* Debug: direct RF register read (direct-BB window). For RX bring-up probes. */
  uint32_t dbg_rf_read(uint8_t path, uint32_t addr) { return rf_read(path, addr); }
  /* Debug: RF register write (3-wire LSSI). For canary-patch bisect. */
  void dbg_rf_write(uint8_t path, uint32_t addr, uint32_t val) {
    rf_write(path, addr, val);
  }
  /* Masked RF register write (read-modify-write; full-register write when
   * mask == RFREGOFFSETMASK). Used by the CW single-tone path. */
  void dbg_rf_set(uint8_t path, uint32_t addr, uint32_t mask, uint32_t val) {
    rf_set(path, addr, mask, val);
  }

private:
  /* config_phydm_parameter_init_8822b: OFDM/CCK block enable via 0x808[29:28]
   * (post=0x3) / disable (pre=0x0). */
  void phydm_pre_post_setting(bool post);
  /* BB/AGC table writer: phydm delay opcodes (0xfe..0xf9) else a full-dword BB
   * register write, +1us settle (odm_config_bb_phy_8812a pattern). */
  void bb_write(uint32_t addr, uint32_t value);
  /* RF table writer: Jaguar 3-wire LSSI write (rA/rB_LSSIWrite_Jaguar 0xC90 /
   * 0xE90) — data = (addr<<20)|(val&0xfffff), masked to 28 bits. path: 0=A,1=B. */
  void rf_write(uint8_t path, uint32_t addr, uint32_t value);
  /* RF 3-wire LSSI read (rHSSIRead_Jaguar 0x8B0 -> rA/rB SI/PI readback). */
  uint32_t rf_read(uint8_t path, uint32_t addr);
  /* Masked RF write: full-mask -> direct LSSI write; else read-modify-write. */
  void rf_set(uint8_t path, uint32_t addr, uint32_t mask, uint32_t value);
  /* phydm_rfe_ifem 2.4G/5G antenna-switch pins (rfe_type 0 path). */
  void rfe_ifem(uint8_t channel);
  /* phydm_igi_toggle_8822b: toggle 0xc50/0xe50 IGI to enter RX mode. */
  void igi_toggle();

  /* BB reset (MAC 0x0 BIT16 = FEN_BBRSTB toggle, the _iqk_bb_reset_8822b
   * mechanism) — relatches the BB clock tree; required after the 5/10 MHz
   * ADC/DAC re-clock (as on Jaguar3). */
  void bb_reset();

  /* Central channel of the wide channel (shared full/fast paths): 20 MHz ->
   * primary; 40 MHz -> ±2 by primary_ch_idx; 80 MHz -> +6/+2/-2/-6. */
  static uint8_t central_ch(uint8_t channel, uint8_t bw, uint8_t primary_ch_idx);
  /* RF 0xBE[17:15] per-5G-channel phase-noise / VCO-band value (8822B); 0 for
   * 2.4 GHz, 0xff = no write (shared full/fast paths). */
  static uint8_t rf_be_for_8822b(uint8_t cch);

  /* fast_retune write-on-change caches. Invalidated by every full
   * set_channel_bw (either variant); they only ever mirror fast-path writes —
   * the Jaguar1 RadioManagementModule pattern. _last_tuned_ch also gates the
   * fast path (0 = never tuned / unknown band). */
  uint8_t _last_tuned_ch = 0;
  uint32_t _rf18_cache = 0;
  /* Compose cache (the Jaguar1 cached-LSSI trick generalised): the dwords the
   * fast path touches are primed by one read each on the first fast hop of an
   * epoch; thereafter bit changes are composed in memory and written as whole
   * dwords — zero per-hop reads (every masked write is otherwise a read+write
   * USB round-trip). _cw_rfbe is the 8822B RF 0xBE 20-bit word. */
  bool _cw_primed = false;
  uint32_t _cw_agc = 0, _cw_fc = 0, _cw_rfbe = 0;
  int _last_agc_bucket = -1;
  uint32_t _last_fc = 0xffffffff;
  int _last_rf_be = -1;
  int _last_df18 = -1;   /* 8822B ch144 RF 0xdf[18] state */
  int _last_cck_key = -1; /* 2G spur (8822B) / CCK-filter (8821C) key: ch14? */
  bool _warned_uncharacterized = false; /* one-shot extended-channel warning */

  /* fast_set_bandwidth cache: the values a full set_channel_bw computes for the
   * current channel, so a same-channel 20<->5/10 toggle can replay the
   * narrowband re-clock delta without the RF channel tune, RF18 read, TX power,
   * or calibrations. _fbw_8ac_20m is the 20 MHz-state 0x8ac (captured on a
   * bw==0 set) — the base the narrowband/20 compose masks are applied to.
   * Invalidated (validity flags) by every full set_channel_bw. */
  uint32_t _fbw_rf18 = 0;   /* final RF18 the full path wrote (20 MHz-mode) */
  uint32_t _fbw_8ac_20m = 0;
  uint8_t _fbw_cch = 0;
  bool _fbw_g2 = false, _fbw_r2t2r = false;
  bool _fbw_cache_valid = false; /* rf18/cch/g2/r2t2r captured this channel */
  bool _fbw_8ac_valid = false;   /* _fbw_8ac_20m holds a real 20 MHz snapshot */
  uint8_t _fbw_cur_bw = 0xff;    /* bw of the last full/fast set (0/5/6/...) */

  /* kfree (efuse power-trim) state — see kfree_init/kfree_apply. bb_gain rows:
   * 0=2G, 1..5 = 5G L1/L2/M1/M2/H per the PHYDM_5G* channel groups. */
  uint8_t _kfree_bb_gain[6][2] = {};
  bool _kfree_2g = false, _kfree_5g = false;

  /* --- 8821C-specific (C8821C variant) channel/RF-set/LCK, transcribed from
   * reference/8821cu phydm_hal_api8821c.c + halrf_8821c.c. The 8822B channel-set
   * reuse diverges materially for the 1T1R 8821C (BTG/WLG RF-set mux, 0xc1c AGC
   * index, RF-firmware LCK), so these replace the 8822B path when _variant is
   * C8821C. `rfe_raw` is the raw efuse RFE byte (rfe_type_expand). --- */
  void set_channel_bw_8821c(uint8_t channel, uint8_t bw, uint8_t rfe_raw,
                            uint8_t primary_ch_idx);
  /* config_phydm_switch_rf_set_8821c: RX front-end mux (0xcb8/0xa84/0xa80).
   * rf_set: 0=BTG (2.4G path B), 1=WLG (2.4G path A), 2=WLA (5G). */
  void switch_rf_set_8821c(uint8_t rf_set);
  /* ex_hal8821c_wifi_only_hw_config + switch_antenna: 8821C-specific WiFi-only
   * coex (GNT->WL + 0xcb4[29:28] SPDT antenna switch), distinct from the 8822B
   * coex_wlan_only register set. */
  void coex_wlan_only_8821c(bool is_5g);
  /* _phy_lc_calibrate_8821c: RF-firmware LCK (RF 0xcc/0xc4), no poll. */
  void do_lck_8821c();

  RtlAdapter _device;
  devourer::DeviceConfig _cfg; /* dump_canary / efuse_dump / hop_prof /
                                  phy_status_8821c / igi / keep_corrupted */
  Logger_t _logger;
  ChipVariant _variant;
  /* Per-chip phydm BB/AGC/RF table data, selected by _variant. */
  std::unique_ptr<Jaguar2PhyTables> _tables;
  ChipVersion _ver{};
  bool _aac_checked = false;
  uint32_t _last_fa = 0; /* last DIG-window false-alarm count (telemetry) */
  RxEnergy _energy{};    /* last DIG-window FA/CCA/IGI snapshot (telemetry) */
  /* Decoded logical EFUSE map, read once (a full physical walk is ~hundreds of
   * USB control transfers ≈ 0.5s). read_efuse_rfe populates it; apply_tx_power
   * reuses it — avoids a second walk that would slow every cold init. */
  uint8_t _efuse_map[0x200];
  /* Rail-hit flags from the last TXAGC apply (see txpwr_saturated_low/high). */
  std::atomic<bool> _txpwr_sat_low{false};
  std::atomic<bool> _txpwr_sat_high{false};
  /* Path-A TXAGC software shadow (see txagc_shadow — the block is write-only). */
  std::atomic<int> _txagc_shadow_cck{-1};
  std::atomic<int> _txagc_shadow_ofdm{-1};
  std::atomic<int> _txagc_shadow_mcs7{-1};
  bool _efuse_valid = false;

  /* 8821C CCK TX-filter defaults (0xa24/0xa28/0xaac) snapshotted from the BB
   * table at parameter-init POST; config_phydm_switch_channel_8821c restores
   * them for 2.4G non-ch14. Saved once when the 8821C tables are applied. */
  uint32_t _cck_a24_8821c = 0, _cck_a28_8821c = 0, _cck_aac_8821c = 0;
  bool _cck_saved_8821c = false;
};

} /* namespace jaguar2 */

#endif /* HAL_JAGUAR2_H */
