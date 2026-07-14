/* Radiotap-header builder + env parser shared by the TX binaries.
 *
 * build_stream_radiotap() turns a devourer::TxMode into a complete, well-formed
 * radiotap header (Legacy OFDM / HT-MCS / VHT) that send_packet parses to set
 * the on-air rate. The demos parse DEVOURER_TX_RATE with parse_tx_mode_str
 * into a TxMode for the demos' command-line interface.
 *
 * Two wire-format facts send_packet relies on:
 *   - A 13-byte (0x0d) radiotap keeps the legacy/HT path (rate_id=8). A length
 *     other than 13 flips send_packet's vht heuristic (rate_id=9) — VHT needs a
 *     22-byte radiotap.
 *   - The radiotap iterator parses all present fields; a 13-byte HT-MCS radiotap
 *     (it_present=MCS|TX_FLAGS) is honoured directly — no env gate required.
 */
#ifndef RADIOTAP_BUILDER_H
#define RADIOTAP_BUILDER_H

#include <cstdint>
#include <string>
#include <vector>

#include "TxMode.h"

namespace devourer {

/* Build a radiotap header according to mode.mode. Output is always a complete,
 * well-formed radiotap header — no 802.11 frame body. */
std::vector<uint8_t> build_stream_radiotap(const TxMode& mode);

/* Parse a TX-mode spec string into a TxMode. Single slash-separated string:
 *   <rate>[/<bw>][/SGI][/LDPC][/STBC]   (case-insensitive)
 *     <rate> : 6M|9M|12M|18M|24M|36M|48M|54M | MCS0..MCS31 | VHT1SS_MCS0..VHT4SS_MCS9
 *     <bw>   : 20|40|80|160 (default 20)
 * Empty or unrecognised falls back to 6M legacy. */
TxMode parse_tx_mode_str(const std::string& spec);

}  // namespace devourer

#endif  // RADIOTAP_BUILDER_H
