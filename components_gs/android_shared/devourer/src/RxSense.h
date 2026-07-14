#ifndef RX_SENSE_H
#define RX_SENSE_H

#include <cstdint>

/* RxEnergy — a frame-free RX energy / channel-busy snapshot. This is the read
 * side of the DEVOURER_CW_TONE emitter: a coarse "how much in-band energy /
 * channel activity is here" measurement that does NOT require receiving a frame.
 *
 * Filled by IRtlDevice::GetRxEnergy() from the chip's phydm facilities:
 *   - false-alarm (FA) + CCA (channel-busy) counters,
 *   - the DIG initial-gain index (a noise-floor proxy),
 *   - and, where triggered, the NHM in-band power histogram.
 *
 * All values are channel-wide scalars — no Realtek 88xx chip exports
 * per-subcarrier CSI to the host, so this is energy, not a spectrum. Build a
 * coarse spectrum by sweeping channels/bins and sampling this per bin.
 *
 * FA/CCA counts are the DELTA since the previous GetRxEnergy() call (each read
 * resets the hardware counters), so a strong in-band carrier shows up as a jump
 * in cca_ofdm / fa_ofdm and a rise in igi. Every field carries a valid_* flag
 * because the facilities differ by chip generation. */
struct RxEnergy {
  /* phydm false-alarm + CCA counters (delta since the previous read). */
  bool valid_fa = false;
  uint32_t fa_ofdm = 0;  /* OFDM false-alarm count */
  uint32_t fa_cck = 0;   /* CCK false-alarm count */
  uint32_t cca_ofdm = 0; /* OFDM CCA (channel-busy) count */
  uint32_t cca_cck = 0;  /* CCK CCA count */

  /* DIG initial-gain index (0x0c50[6:0] on the AC BB): the AGC backs the gain
   * off as the in-band floor rises, so a higher IGI means a busier/noisier
   * channel. Noise-floor proxy. */
  bool valid_igi = false;
  uint8_t igi = 0;

  /* NHM in-band power histogram: 12 IGI-referenced power buckets and the
   * measurement duration. A frame-free power distribution (fuller than the
   * scalar FA counts). Only populated when GetRxEnergy() triggered an NHM. */
  bool valid_nhm = false;
  uint8_t nhm[12] = {};
  uint16_t nhm_duration = 0;
};

#endif /* RX_SENSE_H */
