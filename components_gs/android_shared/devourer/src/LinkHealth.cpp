#include "LinkHealth.h"

namespace devourer {

LinkHealthVerdict classify_link_health(const LinkHealthInput &in,
                                       const LinkHealthThresholds &th) {
  LinkHealthVerdict v;
  v.rssi_dbm = in.rssi_raw - 110;
  v.snr_db = in.snr_raw / 2.0;
  v.evm_db = in.evm_valid ? in.evm_raw / 2.0 : 0.0;
  if (in.igi_valid) {
    v.igi_at_floor = in.igi <= in.igi_min;
    v.igi_at_ceiling = in.igi >= in.igi_max;
  }

  if (in.frames == 0) {
    v.verdict = LinkVerdict::NoSignal;
    v.label = "NO_SIGNAL";
    v.cause = "no frames decoded this window";
    v.fix = "check the TX is up, the channel/bandwidth matches, and the "
            "canonical SA is being sent";
    return v;
  }

  /* Signal-strength band and constellation cleanliness. EVM is the primary
   * discriminator (SNR misses saturation); RSSI splits strong-dirty from
   * weak-dirty. When EVM is absent (CCK-only stream), fall back to SNR. */
  const bool strong = in.rssi_raw >= th.rssi_strong;
  const bool weak = in.rssi_raw <= th.rssi_weak;
  const bool snr_poor = in.snr_raw < th.snr_lo;
  const bool snr_good = in.snr_raw >= th.snr_good;
  const bool evm_poor = in.evm_valid && in.evm_raw > th.evm_poor;
  const bool evm_good = !in.evm_valid || in.evm_raw < th.evm_good;
  const bool dirty = evm_poor || (!in.evm_valid && snr_poor);
  const bool noisy = in.energy_valid && in.fa_ofdm > th.fa_high;

  /* Strong signal but a dirty constellation = the near-field failure the
   * classifier exists for: front-end saturation and/or the strong signal
   * self-jamming via wall reflections. IGI pinned at its floor (the AGC has
   * already backed gain all the way off and still can't cope) corroborates. */
  if (strong && dirty) {
    v.verdict = LinkVerdict::Saturated;
    v.label = "SATURATED";
    v.cause = "strong RSSI but poor EVM — receiver front-end overload and/or "
              "the strong signal self-jamming via reflections (near-field). "
              "SNR alone can look fine here";
    v.fix = "REDUCE TX power (SetTxPowerOffsetQdb, e.g. -40..-80 qdB), add an "
            "attenuator, or increase distance — do NOT add power/antenna";
    return v;
  }

  /* Not strong, but dirty AND the false-alarm rate is up = something other
   * than your own signal is raising the floor: external / co-channel. */
  if (!strong && dirty && noisy) {
    v.verdict = LinkVerdict::Interference;
    v.label = "INTERFERENCE";
    v.cause = "elevated false-alarm rate with a degraded constellation while "
              "the wanted signal is not strong — external / co-channel energy "
              "raising the noise floor";
    v.fix = "change channel (FastRetune / hop away from the interferer); a "
            "narrowband notch (DEVOURER_RX_NBI) only helps an in-band spur";
    return v;
  }

  /* Weak signal, poor SNR, AGC wide open = a genuine range/sensitivity limit. */
  if (weak && (snr_poor || !snr_good)) {
    v.verdict = LinkVerdict::Weak;
    v.label = "WEAK";
    v.cause = "low RSSI and low SNR — genuine range / sensitivity limit "
              "(this is where more power or a better antenna actually helps)";
    v.fix = "raise TX power, improve antenna gain/alignment, or reduce "
            "distance; consider a narrowband re-clock for link budget";
    return v;
  }

  /* Decodes cleanly with comfortable margin. */
  if (snr_good && evm_good && !strong) {
    v.verdict = LinkVerdict::Healthy;
    v.label = "HEALTHY";
    v.cause = "good SNR and EVM with RSSI in the linear range";
    v.fix = "none — link is in its comfortable operating region";
    return v;
  }
  /* A strong-but-clean link is healthy too, but flag that it is near the top
   * of the range (one step from the saturation regime on a near-field bench). */
  if (snr_good && evm_good && strong) {
    v.verdict = LinkVerdict::Healthy;
    v.label = "HEALTHY";
    v.cause = "good SNR and EVM, but RSSI is high — near the top of the linear "
              "range; a little more power or less distance risks saturation";
    v.fix = "fine as-is; leave headroom before adding TX power on a short link";
    return v;
  }

  /* Everything else: decoding but without a comfortable margin. */
  v.verdict = LinkVerdict::Marginal;
  v.label = "MARGINAL";
  v.cause = "decoding, but SNR/EVM is below a comfortable margin and no single "
            "cause dominates";
  v.fix = "watch the trend; if RSSI is high, try backing off power first "
          "(cheap to test), otherwise a small power/antenna improvement";
  return v;
}

} // namespace devourer
