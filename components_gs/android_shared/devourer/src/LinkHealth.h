/* Link-health classifier — the "why is my link bad" interpreter.
 *
 * Devourer exposes rich RX telemetry (per-frame RSSI/SNR/EVM, frame-free
 * FA/CCA/IGI), but a user staring at raw numbers can't tell a near-field
 * saturation problem from a weak link from external interference — and the
 * instinct ("signal's bad, add power / swap the antenna") is often exactly
 * backwards. This maps the sensor tuple to a plain-language verdict + the fix.
 *
 * The load-bearing insight, measured on-air (tests/saturation_knee_sweep.sh):
 * as TX power rises on a near-field link, RSSI climbs but EVM *improves then
 * reverses* at the front-end saturation knee — SNR misses it entirely (it sat
 * flat at 18 dB across the whole sweep while EVM went from -28 dB to -13 dB).
 * So EVM is the discriminator for "strong but dirty" (overload / multipath
 * self-jam), and RSSI splits that from "weak and dirty" (noise / interference).
 *
 * Thresholds are raw devourer units, calibrated from that sweep + the AWGN
 * interference sweep (tests/j3_dig_penalty_sweep.sh):
 *   RSSI  raw PWDB byte, dBm ~= raw - 110; rises with power, ceilings ~73.
 *   SNR   signed half-dB (dB = raw/2); higher better.
 *   EVM   signed half-dB (dB = raw/2); LOWER (more negative) better; 0 = none.
 *   FA    OFDM false alarms per window; quiet ~ tens, interfered ~ hundreds.
 */
#ifndef DEVOURER_LINK_HEALTH_H
#define DEVOURER_LINK_HEALTH_H

#include <cstdint>

namespace devourer {

enum class LinkVerdict {
  NoSignal,     /* no frames decoded this window */
  Saturated,    /* strong RSSI + dirty EVM: near-field overload / self-jam */
  Interference, /* not-strong + dirty + high false-alarm: external / raised floor */
  Weak,         /* low RSSI + low SNR: genuine range/sensitivity limit */
  Marginal,     /* decodes, but SNR/EVM below a comfortable margin */
  Healthy,      /* strong-enough RSSI, good SNR + EVM, in the linear range */
};

/* Sensor snapshot over one window. Frame metrics are the path-A medians the
 * demo already aggregates; the *_valid flags let a caller pass only what it
 * has (frame-free energy and IGI are optional). Raw devourer units. */
struct LinkHealthInput {
  uint32_t frames = 0; /* frames decoded in the window */
  /* rssi_raw is the STRENGTH signal — pass the window PEAK (rssi_max), not the
   * mean: near-field saturation trashes a fraction of frames to low apparent
   * power, dragging the mean down while the peak stays pegged near the PWDB
   * ceiling (measured: full-power near-field rssi_max ~81, backed-off ~77). */
  int rssi_raw = 0;    /* path-A window peak PWDB (dBm ~= raw - 110) */
  int snr_raw = 0;     /* path-A mean SNR, half-dB */
  int evm_raw = 0;     /* path-A mean EVM, half-dB (lower better) */
  bool evm_valid = false;

  bool energy_valid = false; /* fa_ofdm / cca_ofdm populated */
  uint32_t fa_ofdm = 0;
  uint32_t cca_ofdm = 0;

  bool igi_valid = false; /* current IGI + its family rails (rail = a hint) */
  int igi = 0;
  int igi_min = 0;
  int igi_max = 0;
};

struct LinkHealthVerdict {
  LinkVerdict verdict = LinkVerdict::NoSignal;
  const char *label = "NO_SIGNAL"; /* short tag for the marker line */
  const char *cause = "";          /* one-line mechanism */
  const char *fix = "";            /* one-line remedy */
  /* Converted, for display alongside the raw values. */
  int rssi_dbm = 0;
  double snr_db = 0.0;
  double evm_db = 0.0; /* 0 when evm_valid is false */
  bool igi_at_floor = false;
  bool igi_at_ceiling = false;
};

/* Threshold set (raw units), exposed so a test/tool can reference the exact
 * boundaries the measurement calibrated. */
struct LinkHealthThresholds {
  int rssi_strong = 66;  /* >= this = strong signal (near-field regime) */
  int rssi_weak = 38;    /* <= this = weak (~ -72 dBm) */
  /* EVM boundaries calibrated to the demo's window-aggregate (not the tight
   * sweep median): a near-field FULL-power link reads evm_mean ~ -40..-45,
   * the same geometry backed off to the EVM knee reads ~-51..-52. At strong
   * signal, EVM should be excellent (-28..-30 dB, raw -56..-60), so anything
   * worse than ~-23 dB (raw -47) at strong signal is degraded-for-its-strength
   * = the saturation / self-jam tell. */
  int evm_poor = -47;    /* worse (greater) than this = dirty constellation */
  int evm_good = -49;    /* better (less) than this = clean */
  int snr_lo = 16;       /* < this (8 dB) = poor SNR */
  int snr_good = 30;     /* >= this (15 dB) = comfortable */
  uint32_t fa_high = 300; /* OFDM FA/window above this = a noisy channel */
};

/* Classify. Pure function of the snapshot + thresholds (defaulted from the
 * on-air calibration). No I/O — unit-tested in tests/link_health_selftest.cpp. */
LinkHealthVerdict classify_link_health(const LinkHealthInput &in,
                                       const LinkHealthThresholds &th = {});

} // namespace devourer

#endif /* DEVOURER_LINK_HEALTH_H */
