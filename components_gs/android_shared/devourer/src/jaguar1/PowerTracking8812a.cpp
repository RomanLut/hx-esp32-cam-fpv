#include "PowerTracking8812a.h"

#include "Hal8812a_TxPwrTrack.h"
#include "Hal8812PhyReg.h"
#include "RadioManagementModule.h"
#include "RfPath.h"

PowerTracking8812a::PowerTracking8812a(
    RtlAdapter device, std::shared_ptr<EepromManager> eepromManager,
    RadioManagementModule *radio, Logger_t logger)
    : _device(device), _eepromManager(eepromManager), _radio(radio),
      _logger(logger) {}

uint8_t PowerTracking8812a::LookupSwingIndexFromBb() {
  /* Mirror upstream `get_swing_index` for the Jaguar (8812A) branch
   * (`halrf_powertracking_ce.c:632`). Read BB 0xc1c[31:21] and find
   * the matching index in `tx_scaling_table_jaguar`. If no match, the
   * upstream caller falls back to default_ofdm_index = 24. */
  uint32_t bb_swing = _radio->phy_query_bb_reg_public(rA_TxScale_Jaguar, 0xFFE00000u);
  for (int i = 0; i < kTxScaleTableSize; i++) {
    if (bb_swing == kTxScalingTableJaguar[i]) {
      return static_cast<uint8_t>(i);
    }
  }
  return static_cast<uint8_t>(kTxScaleTableSize); /* sentinel — caller picks 24 */
}

void PowerTracking8812a::Init() {
  /* Mirror `odm_txpowertracking_thermal_meter_init` (the 8812A path at
   * line 730 of halrf_powertracking_ce.c).
   *
   * `default_bb_swing_index_flag` is per-driver-lifetime — devourer
   * has only one HalModule init per device-open, so we always run
   * the init body. */
  uint8_t swing_idx = LookupSwingIndexFromBb();
  if (swing_idx >= kTxScaleTableSize) {
    _defaultOfdmIndex = 24; /* 0 dB */
  } else {
    _defaultOfdmIndex = swing_idx;
  }

  /* eeprom_thermal_meter is populated by `Hal_ReadThermalMeter_8812A`
   * during EFUSE shadow-map parse. 0xff means autoload failed — no
   * thermal compensation possible. */
  if (_eepromManager->GetEepromThermalMeter() == 0xff) {
    _txPowerTrackControl = false;
    _logger->info("PowerTracking8812a: EFUSE thermal_meter=0xFF — "
                  "tracking disabled");
  } else {
    _txPowerTrackControl = true;
  }

  _thermalValue = _eepromManager->GetEepromThermalMeter();
  _thermalValueAvgIndex = 0;
  for (auto &v : _thermalValueAvg) {
    v = 0;
  }
  for (int p = 0; p < 2; p++) {
    _absoluteOfdmSwingIdx[p] = 0;
    _deltaPowerIndex[p] = 0;
    _deltaPowerIndexLast[p] = 0;
  }
  _initialised = true;

  _logger->info(
      "PowerTracking8812a init: default_ofdm_index={} (swing_idx_from_bb={}) "
      "eeprom_thermal=0x{:x} txpwrtrack_ctrl={}",
      unsigned(_defaultOfdmIndex), unsigned(swing_idx),
      unsigned(_eepromManager->GetEepromThermalMeter()),
      unsigned(_txPowerTrackControl));
}

void PowerTracking8812a::ClearState() {
  _thermalValueAvgIndex = 0;
  for (auto &v : _thermalValueAvg) {
    v = 0;
  }
  for (int p = 0; p < 2; p++) {
    _absoluteOfdmSwingIdx[p] = 0;
    _deltaPowerIndex[p] = 0;
    _deltaPowerIndexLast[p] = 0;
  }
  _thermalValue = _eepromManager->GetEepromThermalMeter();

  /* Re-seed default_ofdm_index from the current 0xc1c[31:21] value.
   * `phy_SetBBSwingByBand_8812A` calls us right after writing the
   * band-specific BB-swing base (0x200 at 2.4G, 0x16A at 5G for
   * dongles with EFUSE swing-down). Without this refresh, the
   * Init()-time snapshot (taken before the band switch fires) is
   * stale and pwrtrk computes the wrong final swing index relative
   * to the actual base — surfaced as ~6 step over-compute on 5G
   * TX-AGC. */
  uint8_t swing_idx = LookupSwingIndexFromBb();
  _defaultOfdmIndex =
      (swing_idx < kTxScaleTableSize) ? swing_idx : uint8_t{24};
}

void PowerTracking8812a::GetTrackingTable(BandType band, uint8_t channel,
                                          uint8_t thermal_value,
                                          uint8_t delta) {
  /* Mirror `odm_get_tracking_table` (halphyrf_ce.c:155) + the
   * channel/rate dispatch from `get_delta_swing_table_8812a`
   * (halrf_8812a_ce.c:328). devourer always passes `tx_rate==0xFF`
   * (monitor mode, no current rate) so we always take the
   * non-CCK OFDM lookup. */

  const uint8_t *tab_up_a = kDeltaSwingTable2gaP;
  const uint8_t *tab_down_a = kDeltaSwingTable2gaN;
  const uint8_t *tab_up_b = kDeltaSwingTable2gbP;
  const uint8_t *tab_down_b = kDeltaSwingTable2gbN;

  if (band == BandType::BAND_ON_2_4G) {
    tab_up_a = kDeltaSwingTable2gaP;
    tab_down_a = kDeltaSwingTable2gaN;
    tab_up_b = kDeltaSwingTable2gbP;
    tab_down_b = kDeltaSwingTable2gbN;
  } else {
    /* 5G — pick sub-band bucket per upstream:
     *   ch 36..64  -> [0],  ch 100..144 -> [1],  ch 149..177 -> [2]. */
    int bucket = 0;
    if (channel >= 36 && channel <= 64)
      bucket = 0;
    else if (channel >= 100 && channel <= 144)
      bucket = 1;
    else if (channel >= 149 && channel <= 177)
      bucket = 2;
    else
      bucket = 0; /* safety — invalid channel for 5G */
    tab_up_a = kDeltaSwingTable5gaP[bucket];
    tab_down_a = kDeltaSwingTable5gaN[bucket];
    tab_up_b = kDeltaSwingTable5gbP[bucket];
    tab_down_b = kDeltaSwingTable5gbN[bucket];
  }

  uint8_t eeprom_thermal = _eepromManager->GetEepromThermalMeter();
  bool warmer = (thermal_value > eeprom_thermal);

  for (int p = 0; p < 2; p++) {
    _deltaPowerIndexLast[p] = _deltaPowerIndex[p];
    const uint8_t *tab = (p == 0)
                             ? (warmer ? tab_up_a : tab_down_a)
                             : (warmer ? tab_up_b : tab_down_b);
    int8_t v = static_cast<int8_t>(tab[delta]);
    if (warmer) {
      _deltaPowerIndex[p] = v;
      _absoluteOfdmSwingIdx[p] = v;
    } else {
      _deltaPowerIndex[p] = static_cast<int8_t>(-v);
      _absoluteOfdmSwingIdx[p] = static_cast<int8_t>(-v);
    }
  }
}

void PowerTracking8812a::ApplySwingToBb() {
  /* Mirror `odm_tx_pwr_track_set_pwr8812a` MIX_MODE path
   * (halrf_8812a_ce.c:218). Devourer never sees a known tx_rate so
   * `pwr_tracking_limit` keeps its initial value of 26 (+1.0 dB) —
   * same as upstream's `tx_rate==0xFF` default. */
  for (int p = 0; p < 2; p++) {
    int final_ofdm_swing_index =
        static_cast<int>(_defaultOfdmIndex) + _absoluteOfdmSwingIdx[p];

    int idx_to_write;
    if (final_ofdm_swing_index > kPwrTrackingLimit) {
      idx_to_write = kPwrTrackingLimit;
    } else if (final_ofdm_swing_index <= 0) {
      idx_to_write = 0;
    } else {
      idx_to_write = final_ofdm_swing_index;
    }

    uint32_t bb_addr =
        (p == 0) ? rA_TxScale_Jaguar : rB_TxScale_Jaguar;
    _device.phy_set_bb_reg(bb_addr, 0xFFE00000u,
                           kTxScalingTableJaguar[idx_to_write]);
  }
}

void PowerTracking8812a::TickThermalMeter(BandType band, uint8_t channel) {
  if (!_initialised) {
    DVR_DEBUG(_logger, 
        "PowerTracking8812a::TickThermalMeter called before Init — skipped");
    return;
  }
  if (!_txPowerTrackControl) {
    return;
  }

  /* Read live thermal meter via RF[A][0x42][15:10]. Mirrors the
   * `odm_get_rf_reg(dm, RF_PATH_A, c.thermal_reg_addr, 0xfc00)` call
   * in `odm_txpowertracking_callback_thermal_meter` — 8812A's
   * `c.thermal_reg_addr = RF_T_METER_8812A = 0x42`. RMM's
   * `phy_query_rf_reg` already shifts the masked bits down, so the
   * returned value is the 6-bit thermal reading. */
  uint32_t rf_thermal =
      _radio->phy_query_rf_reg(RfPath::RF_PATH_A, 0x42, 0xfc00u);
  uint8_t thermal_value = static_cast<uint8_t>(rf_thermal & 0x3F);

  /* Average over kAvgThermalNum samples. */
  _thermalValueAvg[_thermalValueAvgIndex] = thermal_value;
  _thermalValueAvgIndex = static_cast<uint8_t>(
      (_thermalValueAvgIndex + 1) % kAvgThermalNum);
  uint32_t sum = 0;
  uint8_t cnt = 0;
  for (int i = 0; i < kAvgThermalNum; i++) {
    if (_thermalValueAvg[i] != 0) {
      sum += _thermalValueAvg[i];
      cnt++;
    }
  }
  uint8_t avg = thermal_value;
  if (cnt > 0) {
    avg = static_cast<uint8_t>(sum / cnt);
  }

  uint8_t eeprom_thermal = _eepromManager->GetEepromThermalMeter();
  uint8_t delta_abs = 0;
  if (avg > _thermalValue) {
    delta_abs = static_cast<uint8_t>(avg - _thermalValue);
  } else {
    delta_abs = static_cast<uint8_t>(_thermalValue - avg);
  }

  DVR_DEBUG(_logger, 
      "pwrtrk tick: ch={} band={} thermal_raw=0x{:x} avg=0x{:x} eeprom=0x{:x} "
      "delta_abs={} last_thermal=0x{:x}",
      unsigned(channel), unsigned(band), unsigned(thermal_value), unsigned(avg),
      unsigned(eeprom_thermal), unsigned(delta_abs), unsigned(_thermalValue));

  if (delta_abs > 0) {
    uint8_t delta = 0;
    if (avg > eeprom_thermal) {
      delta = static_cast<uint8_t>(avg - eeprom_thermal);
    } else {
      delta = static_cast<uint8_t>(eeprom_thermal - avg);
    }
    if (delta >= kDeltaSwingIdxSize) {
      delta = kDeltaSwingIdxSize - 1;
    }
    GetTrackingTable(band, channel, avg, delta);

    DVR_DEBUG(_logger, 
        "pwrtrk delta={} abs_ofdm_swing_idx=[A={}, B={}] delta_pwr=[{}, {}] "
        "default_ofdm={} -> final=[A={}, B={}]",
        unsigned(delta), int(_absoluteOfdmSwingIdx[0]),
        int(_absoluteOfdmSwingIdx[1]), int(_deltaPowerIndex[0]),
        int(_deltaPowerIndex[1]), unsigned(_defaultOfdmIndex),
        int(_defaultOfdmIndex) + int(_absoluteOfdmSwingIdx[0]),
        int(_defaultOfdmIndex) + int(_absoluteOfdmSwingIdx[1]));

    if (_deltaPowerIndex[0] != _deltaPowerIndexLast[0] ||
        _deltaPowerIndex[1] != _deltaPowerIndexLast[1]) {
      ApplySwingToBb();
    }
  }

  _thermalValue = avg;
}
