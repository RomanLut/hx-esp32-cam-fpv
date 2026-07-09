/* Per-chip TX capability report — what modulation features a given adapter can
 * actually transmit. An adaptive link (or wfb-ng) that blindly sets STBC/LDPC
 * radiotap flags on a card that lacks them produces malformed frames that never
 * decode — a well-known OpenIPC-FPV footgun ("LDPC/STBC forced on a card that
 * lacks them → link dies", e.g. the 1T1R 8811AU/8821AU/8821CU where STBC needs
 * ≥2 TX chains). devourer already resolves chip identity at construction; this
 * surfaces it so callers can gate features, and send_packet drops an
 * unsupported STBC request rather than airing garbage.
 */
#ifndef DEVOURER_TX_CAPS_H
#define DEVOURER_TX_CAPS_H

#include <cstdint>

namespace devourer {

struct TxCaps {
  bool supported = false;  /* false on a generation that hasn't wired this */
  uint8_t n_ss = 0;        /* spatial streams the RF supports (1/2/3/4) */
  bool stbc_ok = false;    /* STBC needs ≥2 TX chains — false on 1T1R parts */
  bool ldpc_ok = false;    /* LDPC coding supported */
  bool sgi_ok = false;     /* short guard interval */
  uint8_t bw_max_mhz = 20; /* widest TX bandwidth (20/40/80) */
};

/* Build caps from the chain count. The load-bearing rule: STBC needs ≥2 TX
 * chains, so a 1T1R part (chains==1) reports stbc_ok=false — the invariant the
 * send_packet guard relies on. Pure; unit-tested in tests/tx_caps_selftest.cpp.
 * LDPC/SGI/bw default true/80 for the 802.11ac Jaguar family. */
inline TxCaps tx_caps_for_chains(uint8_t chains, bool ldpc = true,
                                 bool sgi = true, uint8_t bw_max_mhz = 80) {
  TxCaps c;
  c.supported = true;
  c.n_ss = chains;
  c.stbc_ok = chains >= 2;
  c.ldpc_ok = ldpc;
  c.sgi_ok = sgi;
  c.bw_max_mhz = bw_max_mhz;
  return c;
}

} // namespace devourer

#endif /* DEVOURER_TX_CAPS_H */
