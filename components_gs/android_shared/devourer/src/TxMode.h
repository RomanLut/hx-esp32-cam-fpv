/* Programmatic TX-mode descriptor for the Jaguar TX path.
 *
 * A TxMode names the modulation class (legacy OFDM / HT / VHT), the rate
 * (MCS / NSS / legacy rate), bandwidth, and the GI / FEC / STBC modifiers —
 * everything that decides a frame's on-air PHY rate. It is the type accepted
 * by RtlJaguarDevice::SetTxMode (the runtime default applied when a frame's
 * radiotap carries no rate) and produced by RadiotapBuilder's env parser.
 *
 * Replaces the former per-knob DEVOURER_TX_* environment variables and the
 * DEVOURER_TX_HT_MCS library gate: send_packet now honours the radiotap rate
 * fields unconditionally, and a TxMode supplies the default when there is none.
 */
#ifndef DEVOURER_TX_MODE_H
#define DEVOURER_TX_MODE_H

#include <cstdint>

#include "SelectedChannel.h"  // ChannelWidth_t

namespace devourer {

struct TxMode {
  enum class Mode { Legacy, HT, VHT };
  Mode mode = Mode::Legacy;

  /* Legacy OFDM rate in 500 kbps units (the radiotap RATE convention, which is
   * identical to the MGN_* OFDM enum value): 12=6M, 18=9M, 24=12M, 36=18M,
   * 48=24M, 72=36M, 96=48M, 108=54M. Default 6M. */
  uint8_t legacy_rate_500kbps = 12;

  /* HT MCS index 0..31 (NSS implicit: MCS0-7=1ss, 8-15=2ss, ...). */
  uint8_t ht_mcs = 0;

  /* VHT per 802.11ac: mcs 0..9, nss 1..4. */
  uint8_t vht_mcs = 0;
  uint8_t vht_nss = 1;

  /* Bandwidth in MHz. Legacy is always 20; HT honours 20/40; VHT 20/40/80. */
  uint8_t bw_mhz = 20;

  bool sgi = false;
  bool ldpc = false;
  bool stbc = false;
};

/* The descriptor inputs send_packet writes, derived from a TxMode. `fixed_rate`
 * is an MGN_* value (see RadioManagementModule.h); `vht` drives the TX-desc
 * rate_id (8 for legacy/HT, 9 for VHT). */
struct TxParams {
  uint8_t fixed_rate;
  bool vht;
  uint8_t sgi;
  uint8_t ldpc;
  uint8_t stbc;
  ChannelWidth_t bwidth;
};

TxParams tx_mode_to_params(const TxMode& mode);

}  // namespace devourer

#endif  // DEVOURER_TX_MODE_H
