/* Runtime TX-power control types for the adaptive link.
 *
 * The TX-power lever has two knobs on IRtlDevice, both live (applied
 * immediately on a brought-up chip, recorded and applied at InitWrite
 * otherwise) and both sticky across channel switches:
 *
 *   - SetTxPowerOffsetQdb(qdb): adjust power RELATIVE to the efuse-calibrated
 *     per-rate table in quarter-dB units, preserving the calibrated per-rate
 *     shape until individual rates saturate at the hardware rails. This is
 *     the closed-loop controller's knob ("back off 2 dB, keep the shape").
 *   - SetTxPowerIndexOverride(idx): force a flat absolute TXAGC index across
 *     all rates (the debug/SDR-visibility knob; -1 reverts to the table).
 *
 * They compose: effective = clamp(baseline + offset_steps, 0, index_max),
 * where baseline is the flat index when set, else the per-rate table value.
 * The offset folds AFTER the per-rate regulatory min() (worldwide-min
 * txpwr_lmt tables where the family has them), clamped only at the hardware
 * max — headroom above the generated table is deliberate; compliance is the
 * operator's call, exactly as with the flat override.
 *
 * Units: one TXAGC index step is family-specific (0.5 dB on Jaguar1/Jaguar2's
 * 6-bit index, 0.25 dB on Jaguar3's 7-bit reference), so the API speaks
 * quarter-dB (qdB) and reports what was actually applied after quantization.
 */
#ifndef DEVOURER_TX_POWER_H
#define DEVOURER_TX_POWER_H

#include <cstdint>

namespace devourer {

/* Static per-family capabilities of the TX-power knobs. `step_qdb` is the
 * quarter-dB value of one hardware index step (2 = 0.5 dB Jaguar1/2,
 * 1 = 0.25 dB Jaguar3); `step_measured` stays false until the family's
 * dB-per-step slope has been confirmed on-air (SDR least-squares ramp) —
 * until then the step is vendor-documentation truth, not measured truth. */
struct TxPowerCaps {
  bool supported = false;
  uint8_t index_max = 0;       /* 63 (Jaguar1/2), 127 (Jaguar3) */
  uint8_t step_qdb = 0;        /* qdB per index step */
  bool step_measured = false;  /* on-air slope validated for this family */
  int16_t offset_min_qdb = 0;  /* most negative representable offset */
  int16_t offset_max_qdb = 0;  /* most positive representable offset */
};

/* Snapshot of the TX-power knobs plus representative effective indices.
 * `flat_index` = -1 means the efuse per-rate baseline is active. The three
 * representative indices are path-A values for CCK 1M, OFDM 6M and HT MCS7;
 * `hw_readback` says whether they were read back from the TXAGC registers
 * (8814A's packed TXAGC port is write-only, so it reports the software
 * shadow with hw_readback=false). `saturated_low/high` are set when the
 * last apply clamped at least one rate at a rail — the signal a closed-loop
 * controller uses to know the knob has run out of travel. */
struct TxPowerState {
  bool valid = false;
  int16_t flat_index = -1;
  int16_t offset_qdb = 0;   /* quantized offset currently folded, in qdB */
  int16_t offset_steps = 0; /* same, in hardware index steps */
  bool saturated_low = false;
  bool saturated_high = false;
  int16_t cck_index = -1;
  int16_t ofdm_index = -1;
  int16_t mcs7_index = -1;
  bool hw_readback = false;
};

/* Quantize a quarter-dB offset request to a family's step size: round to
 * nearest step, ties away from zero, then clamp to the caps' offset range.
 * Returns the applied qdB (what SetTxPowerOffsetQdb reports); the step count
 * lands in *steps_out when non-null. Returns 0 for unsupported caps. */
int quantize_offset_qdb(int qdb, const TxPowerCaps &caps, int *steps_out);

} // namespace devourer

#endif /* DEVOURER_TX_POWER_H */
