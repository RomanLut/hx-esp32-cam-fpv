#ifndef DEVOURER_CFO_TRACKER_H
#define DEVOURER_CFO_TRACKER_H

#include <cmath>
#include <cstdint>

namespace devourer {

/* Closed-loop carrier-frequency-offset tracker (issue #217). Ported from the
 * vendor phydm_cfo_tracking control law: accumulate the per-frame path-A CFO
 * tail from the RX phy-status, and on a periodic tick step the crystal-cap
 * trim (IRtlDevice::SetXtalCap) by ±1 to drive the average CFO toward zero.
 *
 * A bang-bang integrator with hysteresis: it starts correcting once |CFO|
 * exceeds an enable threshold and stops inside a deadband, so it doesn't
 * dither when already aligned. One cap step per tick keeps it gentle enough
 * to track slow crystal warm-up drift without hunting. The receiver trims its
 * OWN crystal to null the offset it measures against the transmitter — the
 * natural single-ended loop in monitor mode (the TX is passive).
 *
 * CFO units: the phy-status tail is a signed HW code; the vendor
 * CFO_HW_RPT_2_KHZ macro is code * 2.5, but that assumes the 20 MHz FFT grid —
 * for the narrowband (5/10 MHz) re-clock the true kHz scale is smaller (bench
 * cross-check against the SDR LO shift suggests the 2.5 figure reads ~10x
 * high here), so treat the reported magnitude as a relative error signal, not
 * a calibrated kHz. The controller only needs the SIGN (which way to trim) and
 * a deadband; both hold under an uncalibrated linear scale. Thresholds are the
 * vendor's 11/10 (in the same reported units). */
class CfoTracker {
 public:
  /* Per-frame CFO tail from the OFDM phy-status (rx_pkt_attrib::cfo_tail). */
  void add(int8_t cfo_tail) {
    _sum += cfo_tail;
    ++_cnt;
  }

  /* Periodic control tick. `cur_cap` is the current crystal-cap code,
   * `cap_max` the trim ceiling. Returns the new cap to apply, or -1 for no
   * change (no frames since last tick, inside the deadband, or already at a
   * rail). `avg_khz_out` (optional) receives the measured average CFO. */
  int step(int cur_cap, int cap_max, double *avg_khz_out = nullptr) {
    if (_cnt == 0)
      return -1;
    const double avg_khz = (static_cast<double>(_sum) / _cnt) * 2.5;
    _sum = 0;
    _cnt = 0;
    if (avg_khz_out)
      *avg_khz_out = avg_khz;
    const double mag = std::fabs(avg_khz);

    /* Auto-detect the cap->CFO polarity: the sign that reduces a positive CFO
     * flips per silicon (some cap circuits raise the LO with cap, some lower
     * it). If the previous step made |CFO| worse, the guess was backwards —
     * flip it, once. */
    if (_stepped && mag > _prev_mag + 1.0)
      _invert = !_invert;
    _stepped = false;

    if (!_adjust) {
      if (mag > kEnableThKhz)
        _adjust = true;
      else {
        _prev_mag = mag;
        return -1;
      }
    } else if (mag <= kStopThKhz) {
      _adjust = false;
      _prev_mag = mag;
      return -1;
    }
    int dir = avg_khz > 0 ? +1 : -1;
    if (_invert)
      dir = -dir;
    int nc = cur_cap + dir;
    if (nc < 0)
      nc = 0;
    if (nc > cap_max)
      nc = cap_max;
    _prev_mag = mag;
    if (nc == cur_cap)
      return -1;
    _stepped = true;
    return nc;
  }

 private:
  static constexpr double kEnableThKhz = 11.0;
  static constexpr double kStopThKhz = 10.0;
  long _sum = 0;
  int _cnt = 0;
  bool _adjust = false;
  bool _invert = false;
  bool _stepped = false;
  double _prev_mag = 0.0;
};

} /* namespace devourer */

#endif /* DEVOURER_CFO_TRACKER_H */
