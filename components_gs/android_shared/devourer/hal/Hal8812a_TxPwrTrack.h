#ifndef HAL_8812A_TX_PWR_TRACK_H
#define HAL_8812A_TX_PWR_TRACK_H

#include <cstdint>

/* Verbatim copy of `g_delta_swing_table_idx_mp_*_txpowertrack_usb_8812a` from
 * `aircrack-ng/rtl8812au/hal/phydm/rtl8812a/halhwimg8812a_rf.c:1310..1339`.
 *
 * These are phydm's per-band, per-RF-path, per-temperature-direction
 * lookup tables for the TX BB-swing thermal compensation loop. Indexed
 * by `|thermal_value - eeprom_thermal|` (clamped to 29). The value
 * returned is `absolute_ofdm_swing_idx[p]` — added to
 * `default_ofdm_index` (= 24 for 8812A, i.e. table-index 0x200/0dB)
 * to produce the final `tx_scaling_table_jaguar[]` index that
 * `odm_tx_pwr_track_set_pwr8812a` writes to BB 0xc1c[31:21]
 * (path A) or 0xe1c[31:21] (path B).
 *
 * Dimensions: `5g*_p/n` are [3][30] — three sub-band buckets (ch36..64,
 * ch100..144, ch149..177) × 30 temperature deltas. The 2G arrays
 * collapse to [30] (single band).
 *
 * "_p" suffix = thermal_value > eeprom_thermal (chip warmer than PG),
 * "_n" suffix = chip cooler (negate the delta before applying).
 *
 * Source tag: aircrack-ng/rtl8812au@5.6.4.2_35491.20191025
 */

constexpr int kDeltaSwingIdxSize = 30;
constexpr int kFiveGBandNum = 3;

extern const uint8_t kDeltaSwingTable2gaP[kDeltaSwingIdxSize];
extern const uint8_t kDeltaSwingTable2gaN[kDeltaSwingIdxSize];
extern const uint8_t kDeltaSwingTable2gbP[kDeltaSwingIdxSize];
extern const uint8_t kDeltaSwingTable2gbN[kDeltaSwingIdxSize];
extern const uint8_t kDeltaSwingTable2gCckAP[kDeltaSwingIdxSize];
extern const uint8_t kDeltaSwingTable2gCckAN[kDeltaSwingIdxSize];
extern const uint8_t kDeltaSwingTable2gCckBP[kDeltaSwingIdxSize];
extern const uint8_t kDeltaSwingTable2gCckBN[kDeltaSwingIdxSize];
extern const uint8_t kDeltaSwingTable5gaP[kFiveGBandNum][kDeltaSwingIdxSize];
extern const uint8_t kDeltaSwingTable5gaN[kFiveGBandNum][kDeltaSwingIdxSize];
extern const uint8_t kDeltaSwingTable5gbP[kFiveGBandNum][kDeltaSwingIdxSize];
extern const uint8_t kDeltaSwingTable5gbN[kFiveGBandNum][kDeltaSwingIdxSize];

/* phydm's TX BB-swing table for the Jaguar family, indexed by ofdm-swing
 * index. Verbatim from `tx_scaling_table_jaguar` in
 * `halrf_powertracking_ce.c:538`. Index 24 = 0x200 (0 dB), index 26 =
 * 0x23E (+1.0 dB), table size 37 entries spanning -12 dB to +6 dB in
 * 0.5 dB steps. */
constexpr int kTxScaleTableSize = 37;
extern const uint32_t kTxScalingTableJaguar[kTxScaleTableSize];

#endif /* HAL_8812A_TX_PWR_TRACK_H */
