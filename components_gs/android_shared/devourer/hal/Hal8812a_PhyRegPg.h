#ifndef HAL_8812A_PHY_REG_PG_H
#define HAL_8812A_PHY_REG_PG_H

#include <cstdint>

/* Verbatim copy of `array_mp_8812a_phy_reg_pg` from
 * aircrack-ng/rtl8812au@v5.6.4.2_35491.20191025
 * hal/phydm/rtl8812a/halhwimg8812a_bb.c:1012-1058.
 *
 * Per-rate TX-power table. Each row is 6 entries:
 *   {band, rfpath, tx_num, addr, bitmask, data}
 * - band: 0 = 2.4G, 1 = 5G
 * - rfpath: 0 = A, 1 = B
 * - tx_num: 0 = 1T, 1 = 2T
 * - addr: target BB register (e.g. 0xc24 = OFDM 18/6 power),
 *         each byte of `data` is the raw power index for one rate
 * - bitmask: 0xffffffff for the full 4-byte target
 * - data: 4 packed bytes — per-rate target value, parsed by
 *         `EepromManager::LoadTxPowerByRate` into
 *         `TxPwrByRateOffset[band][path][rate]`. */

static const uint32_t kHal8812aPhyRegPg[] = {
	0, 0, 0, 0x00000c20, 0xffffffff, 0x34363840,
	0, 0, 0, 0x00000c24, 0xffffffff, 0x42424444,
	0, 0, 0, 0x00000c28, 0xffffffff, 0x30323638,
	0, 0, 0, 0x00000c2c, 0xffffffff, 0x40424444,
	0, 0, 0, 0x00000c30, 0xffffffff, 0x28303236,
	0, 0, 1, 0x00000c34, 0xffffffff, 0x38404242,
	0, 0, 1, 0x00000c38, 0xffffffff, 0x26283034,
	0, 0, 0, 0x00000c3c, 0xffffffff, 0x40424444,
	0, 0, 0, 0x00000c40, 0xffffffff, 0x28303236,
	0, 0, 0, 0x00000c44, 0xffffffff, 0x42422426,
	0, 0, 1, 0x00000c48, 0xffffffff, 0x30343840,
	0, 0, 1, 0x00000c4c, 0xffffffff, 0x22242628,
	0, 1, 0, 0x00000e20, 0xffffffff, 0x34363840,
	0, 1, 0, 0x00000e24, 0xffffffff, 0x42424444,
	0, 1, 0, 0x00000e28, 0xffffffff, 0x30323638,
	0, 1, 0, 0x00000e2c, 0xffffffff, 0x40424444,
	0, 1, 0, 0x00000e30, 0xffffffff, 0x28303236,
	0, 1, 1, 0x00000e34, 0xffffffff, 0x38404242,
	0, 1, 1, 0x00000e38, 0xffffffff, 0x26283034,
	0, 1, 0, 0x00000e3c, 0xffffffff, 0x40424444,
	0, 1, 0, 0x00000e40, 0xffffffff, 0x28303236,
	0, 1, 0, 0x00000e44, 0xffffffff, 0x42422426,
	0, 1, 1, 0x00000e48, 0xffffffff, 0x30343840,
	0, 1, 1, 0x00000e4c, 0xffffffff, 0x22242628,
	1, 0, 0, 0x00000c24, 0xffffffff, 0x42424444,
	1, 0, 0, 0x00000c28, 0xffffffff, 0x30323640,
	1, 0, 0, 0x00000c2c, 0xffffffff, 0x40424444,
	1, 0, 0, 0x00000c30, 0xffffffff, 0x28303236,
	1, 0, 1, 0x00000c34, 0xffffffff, 0x38404242,
	1, 0, 1, 0x00000c38, 0xffffffff, 0x26283034,
	1, 0, 0, 0x00000c3c, 0xffffffff, 0x40424444,
	1, 0, 0, 0x00000c40, 0xffffffff, 0x28303236,
	1, 0, 0, 0x00000c44, 0xffffffff, 0x42422426,
	1, 0, 1, 0x00000c48, 0xffffffff, 0x30343840,
	1, 0, 1, 0x00000c4c, 0xffffffff, 0x22242628,
	1, 1, 0, 0x00000e24, 0xffffffff, 0x42424444,
	1, 1, 0, 0x00000e28, 0xffffffff, 0x30323640,
	1, 1, 0, 0x00000e2c, 0xffffffff, 0x40424444,
	1, 1, 0, 0x00000e30, 0xffffffff, 0x28303236,
	1, 1, 1, 0x00000e34, 0xffffffff, 0x38404242,
	1, 1, 1, 0x00000e38, 0xffffffff, 0x26283034,
	1, 1, 0, 0x00000e3c, 0xffffffff, 0x40424444,
	1, 1, 0, 0x00000e40, 0xffffffff, 0x28303236,
	1, 1, 0, 0x00000e44, 0xffffffff, 0x42422426,
	1, 1, 1, 0x00000e48, 0xffffffff, 0x30343840,
	1, 1, 1, 0x00000e4c, 0xffffffff, 0x22242628
};

#endif /* HAL_8812A_PHY_REG_PG_H */
