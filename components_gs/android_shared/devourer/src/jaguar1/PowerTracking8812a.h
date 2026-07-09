#ifndef POWER_TRACKING_8812A_H
#define POWER_TRACKING_8812A_H

#include "EepromManager.h"
#include "RfPath.h"
#include "RtlAdapter.h"
#include "SelectedChannel.h"
#include "logger.h"

#include <cstdint>
#include <memory>

enum class BandType;
class RadioManagementModule;

/* Port of upstream phydm's TX BB-swing thermal-meter compensation loop
 * for the 8812AU. Closes the last T1 canary divergence on BB 0xc1c /
 * 0xe1c bits 31:21 by reproducing the math kernel runs from its
 * watchdog timer:
 *
 *   1. Read RF[A][0x42][15:10] = chip thermal meter (6-bit reading).
 *   2. Maintain a rolling average over `kAvgThermalNum` samples.
 *   3. delta = |avg - eeprom_thermal_meter|, clamped to 29.
 *   4. Look up `absolute_ofdm_swing_idx` from `kDeltaSwingTable*` based
 *      on (band, channel range, sign-of-delta, RF path).
 *   5. final_idx = default_ofdm_index (24, 0 dB) + absolute_ofdm_swing_idx
 *      (clamped to [0, kPwrTrackingLimit]).
 *   6. Write `kTxScalingTableJaguar[final_idx]` to:
 *        - BB 0xc1c[31:21] for RF_PATH_A (rA_TxScale_Jaguar)
 *        - BB 0xe1c[31:21] for RF_PATH_B (rB_TxScale_Jaguar)
 *
 * Mirrors `odm_txpowertracking_callback_thermal_meter` +
 * `odm_tx_pwr_track_set_pwr8812a` + `odm_get_tracking_table` in
 * `aircrack-ng/rtl8812au/hal/phydm/halrf/halphyrf_ce.c` and
 * `.../rtl8812a/halrf_8812a_ce.c`. Helpers omitted because they're not
 * reachable from devourer's monitor-mode RX/TX path:
 *   - IQK retrigger on thermal delta — IQK is done at init only;
 *   - LCK retrigger — 8812A doesn't do LCK from powertracking;
 *   - by-rate `pwr_tracking_limit` table — devourer always sees
 *     tx_rate==0xFF, so the limit defaults to 26 (+1 dB) just like
 *     upstream when no rate is known;
 *   - tx-AGC remnant (`remnant_ofdm_swing_idx`) — final_idx is held
 *     below pwr_tracking_limit by clamping, so the remnant path never
 *     fires;
 *   - 8814A path-C/D — separate port;
 *   - CCK-rate / xtal-offset / DPK — not relevant on 8812AU.
 *
 * Wiring:
 *   - `Init()` runs once after BB+RF config (phydm:
 *     `odm_txpowertracking_init` from `phydm_rf_init`).
 *   - `TickThermalMeter()` runs after every channel-set; the
 *     `RtlJaguarDevice` watchdog thread also calls it every 2s for
 *     steady-state thermal tracking.
 */
class PowerTracking8812a {
public:
  PowerTracking8812a(RtlAdapter device,
                     std::shared_ptr<EepromManager> eepromManager,
                     RadioManagementModule *radio, Logger_t logger);

  /* Initialise from EFUSE thermal-meter baseline. Reads the current
   * value of BB 0xc1c[31:21] to seed `default_ofdm_index` (matches
   * upstream's `get_swing_index`). */
  void Init();

  /* Read live thermal meter, fold into the rolling average, recompute
   * the BB-swing index, and write 0xc1c[31:21] / 0xe1c[31:21]. The
   * `band` + `channel` args pick the right delta-swing-table bucket
   * (per `get_delta_swing_table_8812a`).
   *
   * Set `phy_set_rf_reg` true when called from outside the per-channel
   * lock path (i.e. from the watchdog thread); when called inside
   * channel-set the device serialisation is already held. */
  void TickThermalMeter(BandType band, uint8_t channel);

  /* Reset rolling thermal-average buffer and per-path baselines back
   * to `default_ofdm_index`. Upstream calls this on band switch +
   * tx_agc change (`odm_clear_txpowertracking_state`). */
  void ClearState();

private:
  RtlAdapter _device;
  std::shared_ptr<EepromManager> _eepromManager;
  RadioManagementModule *_radio; /* non-owning back-pointer for RF reads */
  Logger_t _logger;

  static constexpr int kAvgThermalNum = 4;
  static constexpr int kPwrTrackingLimit = 26; /* +1.0 dB */
  static constexpr int kTxScaleTableSize = 37;
  static constexpr int kDeltaSwingIdxSize = 30;

  bool _txPowerTrackControl = true;
  bool _initialised = false;

  uint8_t _defaultOfdmIndex = 24; /* 0 dB; reseeded by Init() */

  uint8_t _thermalValue = 0; /* last avg after compute */
  uint8_t _thermalValueAvgIndex = 0;
  uint8_t _thermalValueAvg[kAvgThermalNum]{};

  /* Per-RF-path running state. [0]=A, [1]=B. */
  int8_t _absoluteOfdmSwingIdx[2]{};
  int8_t _deltaPowerIndex[2]{};
  int8_t _deltaPowerIndexLast[2]{};

  uint8_t LookupSwingIndexFromBb();
  void GetTrackingTable(BandType band, uint8_t channel, uint8_t thermal_value,
                        uint8_t delta);
  void ApplySwingToBb();
};

#endif /* POWER_TRACKING_8812A_H */
