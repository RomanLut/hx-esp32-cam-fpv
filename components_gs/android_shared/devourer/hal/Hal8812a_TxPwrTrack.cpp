/* Verbatim copy of `g_delta_swing_table_idx_mp_*_txpowertrack_usb_8812a` +
 * `tx_scaling_table_jaguar` from `aircrack-ng/rtl8812au` —
 * `hal/phydm/rtl8812a/halhwimg8812a_rf.c:1310..1339` and
 * `hal/phydm/halrf/halrf_powertracking_ce.c:538`. Do not hand-edit; if
 * upstream changes these tables, regenerate from the canonical source.
 */
#include "Hal8812a_TxPwrTrack.h"

const uint8_t kDeltaSwingTable2gaP[kDeltaSwingIdxSize] = {
    0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
const uint8_t kDeltaSwingTable2gaN[kDeltaSwingIdxSize] = {
    0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6, 7, 7,
    7, 8, 8, 9, 10, 10, 10, 10, 10, 10};
const uint8_t kDeltaSwingTable2gbP[kDeltaSwingIdxSize] = {
    0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
const uint8_t kDeltaSwingTable2gbN[kDeltaSwingIdxSize] = {
    0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 11, 11, 11, 11};
const uint8_t kDeltaSwingTable2gCckAP[kDeltaSwingIdxSize] = {
    0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
const uint8_t kDeltaSwingTable2gCckAN[kDeltaSwingIdxSize] = {
    0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6, 7, 7,
    7, 8, 8, 9, 10, 10, 10, 10, 10, 10};
const uint8_t kDeltaSwingTable2gCckBP[kDeltaSwingIdxSize] = {
    0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
const uint8_t kDeltaSwingTable2gCckBN[kDeltaSwingIdxSize] = {
    0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 11, 11, 11, 11};

const uint8_t kDeltaSwingTable5gaP[kFiveGBandNum][kDeltaSwingIdxSize] = {
    {0, 1, 1, 2, 2, 3, 4, 5, 6, 7, 7, 8, 8, 9, 10, 11, 11, 11, 11, 11,
     11, 11, 11, 11, 11, 11, 11, 11, 11, 11},
    {0, 1, 1, 2, 3, 3, 4, 5, 6, 7, 7, 8, 8, 9, 10, 11, 11, 11, 11, 11,
     11, 11, 11, 11, 11, 11, 11, 11, 11, 11},
    {0, 1, 1, 2, 3, 3, 4, 5, 6, 7, 7, 8, 8, 9, 10, 11, 11, 12, 12, 11,
     11, 11, 11, 11, 11, 11, 11, 11, 11, 11},
};
const uint8_t kDeltaSwingTable5gaN[kFiveGBandNum][kDeltaSwingIdxSize] = {
    {0, 1, 1, 2, 2, 3, 4, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
     12, 12, 13, 13, 14, 15, 15, 15, 15, 15},
    {0, 1, 1, 2, 2, 3, 4, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
     12, 12, 13, 13, 14, 15, 15, 15, 15, 15},
    {0, 1, 1, 2, 2, 3, 4, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
     12, 12, 13, 13, 14, 15, 15, 15, 15, 15},
};
const uint8_t kDeltaSwingTable5gbP[kFiveGBandNum][kDeltaSwingIdxSize] = {
    {0, 1, 1, 2, 2, 3, 3, 4, 5, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
     11, 11, 11, 11, 11, 11, 11, 11, 11, 11},
    {0, 1, 1, 2, 3, 3, 4, 5, 5, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
     11, 11, 11, 11, 11, 11, 11, 11, 11, 11},
    {0, 1, 1, 2, 3, 3, 4, 5, 6, 7, 7, 8, 8, 9, 9, 10, 11, 11, 11, 11,
     11, 11, 11, 11, 11, 11, 11, 11, 11, 11},
};
const uint8_t kDeltaSwingTable5gbN[kFiveGBandNum][kDeltaSwingIdxSize] = {
    {0, 1, 1, 2, 2, 3, 4, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
     12, 12, 13, 13, 14, 14, 14, 14, 14, 14},
    {0, 1, 1, 2, 2, 3, 4, 4, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
     12, 12, 13, 13, 14, 14, 14, 14, 14, 14},
    {0, 1, 1, 2, 2, 3, 4, 5, 6, 6, 7, 8, 8, 9, 10, 10, 11, 11, 12, 12,
     13, 13, 14, 14, 15, 16, 16, 16, 16, 16},
};

const uint32_t kTxScalingTableJaguar[kTxScaleTableSize] = {
    0x081, /*  0, -12.0 dB */
    0x088, /*  1, -11.5 dB */
    0x090, /*  2, -11.0 dB */
    0x099, /*  3, -10.5 dB */
    0x0A2, /*  4, -10.0 dB */
    0x0AC, /*  5,  -9.5 dB */
    0x0B6, /*  6,  -9.0 dB */
    0x0C0, /*  7,  -8.5 dB */
    0x0CC, /*  8,  -8.0 dB */
    0x0D8, /*  9,  -7.5 dB */
    0x0E5, /* 10,  -7.0 dB */
    0x0F2, /* 11,  -6.5 dB */
    0x101, /* 12,  -6.0 dB */
    0x110, /* 13,  -5.5 dB */
    0x120, /* 14,  -5.0 dB */
    0x131, /* 15,  -4.5 dB */
    0x143, /* 16,  -4.0 dB */
    0x156, /* 17,  -3.5 dB */
    0x16A, /* 18,  -3.0 dB */
    0x180, /* 19,  -2.5 dB */
    0x197, /* 20,  -2.0 dB */
    0x1AF, /* 21,  -1.5 dB */
    0x1C8, /* 22,  -1.0 dB */
    0x1E3, /* 23,  -0.5 dB */
    0x200, /* 24,  +0.0 dB */
    0x21E, /* 25,  +0.5 dB */
    0x23E, /* 26,  +1.0 dB */
    0x261, /* 27,  +1.5 dB */
    0x285, /* 28,  +2.0 dB */
    0x2AB, /* 29,  +2.5 dB */
    0x2D3, /* 30,  +3.0 dB */
    0x2FE, /* 31,  +3.5 dB */
    0x32B, /* 32,  +4.0 dB */
    0x35C, /* 33,  +4.5 dB */
    0x38E, /* 34,  +5.0 dB */
    0x3C4, /* 35,  +5.5 dB */
    0x3FE, /* 36,  +6.0 dB */
};
