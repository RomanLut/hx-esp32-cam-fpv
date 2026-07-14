#ifndef RADIOMANAGEMENTMODULE_H
#define RADIOMANAGEMENTMODULE_H

#include <atomic>
#include <cstdint>
#include <vector>

#include "EepromManager.h"
#include "Iqk8812a.h"
#if defined(DEVOURER_HAVE_8814)
#include "Iqk8814a.h"
#endif
#include "PowerTracking8812a.h"
#include "RfPath.h"
#include "RtlAdapter.h"
#include "SelectedChannel.h"
#include "logger.h"

#define HAL_PRIME_CHNL_OFFSET_DONT_CARE 0
#define HAL_PRIME_CHNL_OFFSET_LOWER 1
#define HAL_PRIME_CHNL_OFFSET_UPPER 2

enum class HwPort { HW_PORT0, HW_PORT1 };

enum class BandType {
  BAND_ON_2_4G = 0,
  BAND_ON_5G = 1,
  BAND_ON_BOTH = 2,
  BAND_MAX = 3,
};

enum RATE_SECTION {
  CCK = 0,
  OFDM = 1,
  HT_MCS0_MCS7 = 2,
  HT_MCS8_MCS15 = 3,
  HT_MCS16_MCS23 = 4,
  HT_MCS24_MCS31 = 5,
  HT_1SS = HT_MCS0_MCS7,
  HT_2SS = HT_MCS8_MCS15,
  HT_3SS = HT_MCS16_MCS23,
  HT_4SS = HT_MCS24_MCS31,
  VHT_1SSMCS0_1SSMCS9 = 6,
  VHT_2SSMCS0_2SSMCS9 = 7,
  VHT_3SSMCS0_3SSMCS9 = 8,
  VHT_4SSMCS0_4SSMCS9 = 9,
  VHT_1SS = VHT_1SSMCS0_1SSMCS9,
  VHT_2SS = VHT_2SSMCS0_2SSMCS9,
  VHT_3SS = VHT_3SSMCS0_3SSMCS9,
  VHT_4SS = VHT_4SSMCS0_4SSMCS9,
  RATE_SECTION_NUM,
};

#include "RateDefinitions.h"

/* ThermalStatus/ThermalBucket moved to the generation-agnostic
 * src/ThermalStatus.h when GetThermalStatus was promoted to IRtlDevice;
 * the alias keeps the many existing Jaguar1 + demo references compiling
 * unchanged. */
#include "../ThermalStatus.h"
using ThermalStatus = devourer::ThermalStatus;
using devourer::ThermalBucket;

class RadioManagementModule {
  RtlAdapter _device;
  devourer::DeviceConfig::Tuning _tuning; /* skip_txpwr / force_iqk / disable_iqk */
  bool _keep_corrupted = false;           /* rx.keep_corrupted */
  bool _dump_canary = false;              /* debug.dump_canary */
  std::shared_ptr<EepromManager> _eepromManager;
  Logger_t _logger;
  HwPort _hwPort = HwPort::HW_PORT0;
  bool _needIQK = false;
  bool _warned_uncharacterized = false; /* one-shot extended-channel warning */
  ChannelWidth_t _currentChannelBw;
  uint8_t _currentChannel;
  BandType current_band_type;
  bool _swChannel = false;
  bool _channelBwInitialized = false;
  bool _setChannelBw = false;
  /* Cached RF_CHNLBW (0x18) value per RF path for fast_retune(cache_rf=true):
   * lets the hop write the full register without a read-modify-write (and thus
   * without the 20 ms C-cut RF-read sleep). Invalidated by set_channel_bwmode,
   * since the full path can change 0x18's bandwidth bits. */
  uint32_t _rf_chnlbw_cache[4] = {0, 0, 0, 0};
  bool _rf_chnlbw_cached = false;
  /* fast_retune trim caches: fc_area (0x860) and the spur-fix config are
   * constant across an intra-band same-BW hop set, so skip re-writing them
   * when unchanged. Also invalidated by set_channel_bwmode. */
  uint32_t _last_fc_area = 0xffffffff;
  int _last_spur_class = -1;
  /* Last secondary-channel value written by the lean 40/80 fast path. It
   * depends only on the prime-channel offset (not the channel number), so for a
   * same-BW same-offset hop set it's constant — write the SubChnl registers
   * once, then skip. Invalidated by set_channel_bwmode. */
  int _last_subchnl = -1;
  /* Set by fast_retune around a reused set_channel_bwmode call so the shared
   * channel-set skips the per-rate TX-power loop + thermal pwrtrk tick (the
   * heavy stages a hop doesn't need), while still doing the channel + BW
   * registers identically to the full path. */
  bool _fast_skip_heavy = false;
  /* fast_set_bandwidth compose-cache: the 20 MHz-state value of BB 0x8ac
   * (rRFMOD_Jaguar), the ONLY register that differs across a same-channel
   * 20<->5/10 MHz toggle (0x8c4[30], L1PeakTH, 0x668, RF18, DATA_SC are all
   * invariant — the RF stays in 20 MHz mode). Caching the full dword lets the
   * toggle be a single write-only dword, and preserves whatever ADC-clock
   * [9:8] value phy_FixSpur_8812A chose for this channel on the 20 MHz restore.
   * Primed lazily from a 20 MHz state; invalidated by set_channel_bwmode. */
  uint32_t _bw_cached_8ac = 0;
  bool _bw_cache_valid = false;
  uint8_t _cur40MhzPrimeSc;
  uint8_t _cur80MhzPrimeSc;
  uint8_t _currentCenterFrequencyIndex;
  uint8_t power = 16;
  /* Experiment knob: when >= 0, every per-rate TXAGC index is forced to this
   * value instead of the EFUSE-derived per-rate table (or the `power`
   * fallback). -1 = disabled (normal behaviour). Set via SetTxPowerOverride
   * and re-applied on the next channel-set; used by the thermal-vs-gain
   * ramp in txdemo. Atomic so GetTxPowerState's cached snapshot is
   * readable from any thread (setters remain control-plane-thread calls). */
  std::atomic<int> txpwr_override_{-1};
  /* Runtime TX-power offset in TXAGC index steps (0.5 dB each), folded onto
   * the per-rate baseline (EFUSE table or flat override) AFTER the per-rate
   * regulatory min and clamped only at the 6-bit rails — the relative knob
   * behind IRtlDevice::SetTxPowerOffsetQdb. The saturation flags record
   * whether the last apply hit a rail on any rate (reset per
   * PHY_SetTxPowerLevel8812 pass) — the "knob out of travel" signal for a
   * closed-loop controller. */
  std::atomic<int> txpwr_offset_steps_{0};
  std::atomic<bool> txpwr_sat_low_{false};
  std::atomic<bool> txpwr_sat_high_{false};
  PowerTracking8812a _pwrTrk;
  Iqk8812a _iqk;
#if defined(DEVOURER_HAVE_8814)
  Iqk8814a _iqk8814;
#endif

public:
  RadioManagementModule(RtlAdapter device,
                        std::shared_ptr<EepromManager> eepromManager,
                        Logger_t logger,
                        const devourer::DeviceConfig &cfg = {});
  /* Initialise phydm thermal-meter pwrtrk state. Call once after the
   * BB + RF table application is complete (mirrors phydm's
   * `phydm_rf_init -> odm_txpowertracking_init`). Reads EFUSE +
   * current 0xc1c[31:21]. Safe to call multiple times. */
  void InitPwrTrack();
  /* Run one phydm thermal-meter pwrtrk tick. Mirrors the watchdog
   * callback `odm_txpowertracking_callback_thermal_meter` and writes
   * the resulting BB-swing index to 0xc1c[31:21] / 0xe1c[31:21]. */
  void TickPwrTrack();
  /* Read the chip thermal meter (RF[A][0x42][15:10]) and pair it with the
   * EFUSE baseline. Read-only — does NOT touch the TX-power-tracking
   * BB-swing registers (that correction lives in TickPwrTrack). Works on
   * every Jaguar member: path-A RF reads succeed on 8812/8811/8814/8821
   * (on the 8814 only paths C/D are write-only). */
  ThermalStatus ReadThermalStatus();
  /* Run a full I/Q calibration. Mirrors upstream
   * `phy_iq_calibrate_8812a` triggered from the channel-set callback
   * when `_needIQK` is asserted. Takes ~50-100 ms per invocation. */
  void RunIQK();
  /* Arm the IQK trigger so the next channel-set runs a full I/Q
   * calibration. HalModule calls this once at init so the initial
   * channel-set converges 0xc1c IQK output + 0xc90 IQ-imbalance
   * registers to kernel-equivalent state. */
  void ArmIQKOnNextChannelSet() { _needIQK = true; }
  void hw_var_rcr_config(uint32_t rcr);
  void SetMonitorMode();
  void set_channel_bwmode(uint8_t channel, uint8_t channel_offset,
                          ChannelWidth_t bwmode);
  /* Lean frequency-hop retune. Runs ONLY the RF channel switch (phy_SwChnl),
   * skipping the per-rate TX-power loop, the bandwidth post-set, and the
   * thermal pwrtrk tick that the full set_channel_bwmode does — those don't
   * change across an intra-band, same-bandwidth hop. Returns false WITHOUT
   * touching the chip if the request crosses the 2.4/5 GHz boundary or the
   * current bandwidth isn't 20 MHz (the caller must then fall back to the full
   * set_channel_bwmode, which handles band switch + IQK). With cache_rf=true
   * the RF_CHNLBW (0x18) writes are done as full-register writes from a cached
   * value instead of masked read-modify-writes, avoiding the per-read 20 ms
   * C-cut sleep — the dominant cost of phy_SwChnl on C-cut silicon. */
  bool fast_retune(uint8_t channel, bool cache_rf);
  /* Lean same-channel bandwidth toggle between 20 MHz and 5/10 MHz narrowband:
   * one BB 0x8ac dword write, skipping the RF read-modify-write (the ~40 ms
   * cut-C sleep), the invariant MAC/RF/DATA_SC writes, TX-power re-fold, thermal
   * pwrtrk, and IQK. Returns false (touching nothing) when the toggle involves
   * 40/80 MHz or a non-8812 die — the caller falls back to set_channel_bwmode. */
  bool fast_set_bandwidth(ChannelWidth_t new_bw);
  void phy_set_rf_reg(RfPath eRFPath, uint16_t RegAddr, uint32_t BitMask,
                      uint32_t Data);
  uint32_t phy_query_rf_reg(RfPath eRFPath, uint32_t RegAddr,
                            uint32_t BitMask);
  uint32_t phy_query_bb_reg_public(uint16_t regAddr, uint32_t bitMask);
  void init_hw_mlme_ext(SelectedChannel pmlmeext);
  void rtw_hal_set_chnl_bw(uint8_t channel, ChannelWidth_t Bandwidth,
                           uint8_t Offset40, uint8_t Offset80);
  void PHY_SwitchWirelessBand8812(BandType Band);
  void PHY_SwitchWirelessBand8814A(BandType Band);
  /* One-time RFE GPIO pin-select for the 8814 (mirror of the kernel's
   * PHY_SetRFEReg8814A bInit=TRUE branch). Must run once at init, after the
   * first band-set. Without it the external PA/T-R switch never engages on
   * TX (submits OK, 0 on-air) even though RX works. */
  void InitRFEGpio8814A();
  void SetTxPower(uint8_t p);
  /* Force the per-rate TXAGC index (0..63) on the next and subsequent
   * channel-sets, overriding the EFUSE per-rate table. -1 restores normal
   * (EFUSE-driven) behaviour. Takes effect when set_channel_bwmode runs, or
   * immediately via ApplyTxPower(). */
  void SetTxPowerOverride(int idx) { txpwr_override_ = idx; }
  /* Runtime TX-power offset in index steps (see txpwr_offset_steps_). Takes
   * effect on the next channel-set, or immediately via ApplyTxPower(). */
  void SetTxPowerOffsetSteps(int steps) { txpwr_offset_steps_ = steps; }
  int GetTxPowerOffsetSteps() const { return txpwr_offset_steps_; }
  int GetTxPowerOverride() const { return txpwr_override_; }
  bool TxPowerSaturatedLow() const { return txpwr_sat_low_; }
  bool TxPowerSaturatedHigh() const { return txpwr_sat_high_; }
  /* Effective per-rate TXAGC index for the current channel/BW: the flat
   * override (when armed) or the EFUSE per-rate base (or the legacy `power`
   * fallback), plus the runtime offset, clamped to the 6-bit field with the
   * saturation flags recording rail hits. The single computation behind both
   * the per-rate apply loop and the 0xc54 power-training word; public so the
   * device layer can build the software-shadow TxPowerState on the 8814A,
   * whose packed TXAGC port (0x1998) is write-only. */
  uint8_t ComputeTxPowerIndex(uint8_t path, uint8_t rate, uint8_t ntx_idx);
  /* Re-run the per-rate TXAGC writes for the current channel WITHOUT a channel
   * switch. set_channel_bwmode early-returns when the channel/bw is unchanged,
   * so this is the only way to push a freshly-set SetTxPowerOverride() /
   * SetTxPowerOffsetSteps() value to the TXAGC registers mid-session (used by
   * the thermal-vs-gain ramp and SetTxPowerOffsetQdb). */
  void ApplyTxPower() { PHY_SetTxPowerLevel8812(_currentChannel); }

private:
  void rtw_hal_set_msr(uint8_t net_type);
  void hw_var_set_monitor();
  void PHY_SetSwChnlBWMode8812(uint8_t channel, ChannelWidth_t Bandwidth,
                               uint8_t Offset40, uint8_t Offset80);
  void PHY_HandleSwChnlAndSetBW8812(bool bSwitchChannel, bool bSetBandWidth,
                                    uint8_t ChannelNum,
                                    ChannelWidth_t ChnlWidth,
                                    uint8_t ChnlOffsetOf40MHz,
                                    uint8_t ChnlOffsetOf80MHz,
                                    uint8_t CenterFrequencyIndex1);
  void phy_SwChnlAndSetBwMode8812();
  uint32_t phy_RFSerialRead(RfPath eRFPath, uint32_t Offset);
  void phy_RFSerialWrite(RfPath eRFPath, uint32_t Offset, uint32_t Data);
  void phy_SetRFEReg8812(BandType Band);
  void phy_SetRFEReg8821(BandType Band);
  void phy_SetRFEReg8814A(BandType Band);
  void phy_SetBwRegAdc_8814A(BandType Band, ChannelWidth_t bw);
  void phy_SetBwRegAgc_8814A(BandType Band, ChannelWidth_t bw);
  void phy_SetBBSwingByBand_8812A(BandType Band);
  void phy_SetBBSwingByBand_8814A(BandType Band);
  uint32_t phy_get_tx_bb_swing_8812a(BandType Band, RfPath RFPath);
  void Set_HW_VAR_ENABLE_RX_BAR(bool val);
  void phy_SwChnl8812();
  void phy_SwChnl8812_fast(uint8_t channelToSW);
  void DumpCanary();
  void phy_SwChnl8814A();
  bool phy_SwBand8812(uint8_t channelToSW);
  void phy_FixSpur_8812A(ChannelWidth_t Bandwidth, uint8_t Channel);
  void phy_PostSetBwMode8812();
  void phy_PostSetBwMode8814A();
  void phy_SetRegBW_8812(ChannelWidth_t CurrentBW);
  void PHY_RF6052SetBandwidth8812(ChannelWidth_t Bandwidth);
  uint8_t phy_GetSecondaryChnl_8812();
  void PHY_SetTxPowerLevel8812(uint8_t Channel);
  void phy_set_tx_power_level_by_path(uint8_t channel, RfPath path);
  void phy_set_tx_power_index_by_rate_section(RfPath rfPath, uint8_t channel,
                                              RATE_SECTION rateSection);
  void PHY_TxPowerTrainingByPath_8812(RfPath rfPath);
  void PHY_SetTxPowerIndexByRateArray(RfPath rfPath,
                                      const std::vector<MGN_RATE> &rates);
  void PHY_SetTxPowerIndex_8812A(uint32_t powerIndex, RfPath rfPath,
                                 MGN_RATE rate);
  uint32_t phy_query_bb_reg(uint16_t regAddr, uint32_t bitMask);
  uint32_t PHY_QueryBBReg8812(uint16_t regAddr, uint32_t bitMask);
};

#endif /* RADIOMANAGEMENTMODULE_H */
