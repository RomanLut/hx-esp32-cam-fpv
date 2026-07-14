/* Chip thermal-meter snapshot, generation-agnostic (moved out of the Jaguar1
 * HAL when GetThermalStatus was promoted to IRtlDevice — the meter register
 * family (RF 0x42) and the efuse-baseline semantics are common to all three
 * generations, only the field layout and baseline source differ per family).
 */
#ifndef DEVOURER_THERMAL_STATUS_H
#define DEVOURER_THERMAL_STATUS_H

#include <cstdint>

namespace devourer {

/* Read-only snapshot of the chip's thermal meter. `raw` is the live
 * RF[A][0x42] reading (0..63, Realtek "thermal units" — roughly
 * 1.5-2 C each, NOT absolute degrees). `baseline` is the EFUSE
 * factory-calibrated reading (0xFF = autoload failed / no baseline; on the
 * 8822C, which wires no efuse baseline, it is the first-read cold reference,
 * so `delta` there means "since first read"). `delta = raw - baseline`
 * (signed) is the heat signal — positive means the chip is running hotter
 * than calibration. `valid` is false when no baseline of either kind is
 * available, in which case only `raw` is meaningful. */
struct ThermalStatus {
  uint8_t raw = 0;
  uint8_t baseline = 0xFF;
  int delta = 0;
  bool valid = false;
};

/* Coarse, honest health label for a thermal reading. The meter is NOT a
 * calibrated °C sensor (Realtek publishes no °C transfer function for the AU
 * family; the value is an RF/PA-bias tracking index), so we deliberately bucket
 * the delta-from-baseline rather than fake a precise temperature — the same
 * stance the rtl88x2eu driver takes (cool/warm/hot/...). Thresholds are in
 * thermal units above the EFUSE baseline; "hot" aligns with the default
 * DEVOURER_THERMAL_WARN_DELTA of 15. Returns "unknown" when no EFUSE baseline
 * is available (delta is meaningless without it). */
inline const char *ThermalBucket(const ThermalStatus &s) {
  if (!s.valid) return "unknown";
  if (s.delta < 8) return "cool";
  if (s.delta < 15) return "warm";
  if (s.delta < 25) return "hot";
  return "critical";
}

} // namespace devourer

#endif /* DEVOURER_THERMAL_STATUS_H */
