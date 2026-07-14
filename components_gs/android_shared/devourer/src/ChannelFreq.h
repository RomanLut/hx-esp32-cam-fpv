#ifndef CHANNEL_FREQ_H
#define CHANNEL_FREQ_H

#include <cstdint>

/* Wi-Fi channel <-> center-frequency mapping shared by the per-packet radiotap
 * CHANNEL hop path (library side, all generations) and the demos' hop/sweep
 * spec parsing. Generation-neutral, header-only. */

namespace devourer {

/* Map a radiotap CHANNEL frequency (MHz) to a Wi-Fi channel number. Returns 0
 * for a frequency outside the bands devourer drives (caller ignores it).
 *
 * The 5 GHz clause deliberately runs past the UNII channels: the RF
 * synthesizer on these chips covers roughly 5080..6165 MHz (vendor
 * rtl88x2bu "monitor_chan_override": chan 16..253, freq = 5000 + 5*chan),
 * so any 5 MHz-gridded frequency up to chan 253 maps to a tunable channel.
 * Channel stays uint8_t everywhere; 253 is the ceiling. Not every chip
 * locks every frequency — out-of-band use is validated per chip, and TX
 * power there is extrapolated from the nearest characterized channel. */
static inline int freq_to_chan(uint16_t freq_mhz) {
  if (freq_mhz == 2484)
    return 14;
  if (freq_mhz >= 2412 && freq_mhz <= 2472)
    return (freq_mhz - 2407) / 5;        /* 2.4 GHz ch1..13 */
  if (freq_mhz >= 5000 && freq_mhz <= 6265)
    return (freq_mhz - 5000) / 5;        /* 5 GHz ch, incl. extended synth range */
  return 0;
}

/* Wi-Fi channel number -> center frequency (MHz), for the radiotap CHANNEL
 * field that drives the library's per-packet retune (DEVOURER_HOP_RADIOTAP). */
static inline uint16_t chan_to_freq(int ch) {
  if (ch == 14) return 2484;
  if (ch <= 14) return static_cast<uint16_t>(2407 + 5 * ch);
  return static_cast<uint16_t>(5000 + 5 * ch);
}

} /* namespace devourer */

#endif /* CHANNEL_FREQ_H */
