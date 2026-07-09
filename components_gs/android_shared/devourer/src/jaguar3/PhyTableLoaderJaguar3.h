#ifndef PHY_TABLE_LOADER_8822C_H
#define PHY_TABLE_LOADER_8822C_H

#include <cstdint>
#include <functional>

namespace jaguar3 {

/* Runtime walker for the Jaguar3 "halbb" phydm register tables
 * (array_mp_8822c_*). This is NOT the Jaguar1 format — src/PhyTableLoader walks
 * the older check_positive (0x8/0x9/0xA/0xB with 0x80000000/0x40000000 halves)
 * encoding. Jaguar3 uses the newer "halbb" encoding:
 *
 *   - A headline section: leading (v1,v2) pairs whose v1>>28 == 0xf, each
 *     encoding a {cut, rfe} case. halbb_sel_headline() picks the best-matching
 *     case (5-level priority: exact, rfe-match/cut-dontcare, rfe-match/cut-max,
 *     rfe-dontcare/cut-max, none) and yields cfg_target.
 *   - A body section of (v1,v2) pairs dispatched on v1>>28:
 *       0x8 PARA_IF / 0x9 PARA_ELSE_IF  -> set cfg_para = v1 & 0x0fffffff
 *       0xa PARA_ELSE                   -> stop matching this block
 *       0xb PARA_END (endif)            -> reset to default (matching on)
 *       0x4 PARA_CHK                    -> matched = (cfg_para == cfg_target)
 *       else (a real register addr)     -> if matched, write(v1, v2)
 *
 * Faithful port of odm_read_and_config_mp_8822c_* in the vendor
 * hal/phydm/rtl8822c/halhwimg8822c_bb.c. Validated on hardware.
 *
 * The writer receives (addr, data) for MASKDWORD writes. Pseudo-addresses
 * 0xf9..0xfe encode delays in the BB table; the caller's writer must handle
 * them (see odm_config_bb_phy_8822c) — the walker passes them through verbatim. */
struct Jaguar3PhyContext {
  uint8_t cut_version; /* dm->cut_version (ODM_CUT_*) */
  uint8_t rfe_type;    /* dm->rfe_type (EFUSE RFE frontend type) */
};

using Jaguar3PhyWriter = std::function<void(uint32_t addr, uint32_t data)>;

class PhyTableLoaderJaguar3 {
public:
  /* Walk one array_mp_8822c_* table of `len` u32 words, applying the matched
   * register writes through `write`. Mirrors a single
   * odm_read_and_config_mp_8822c_*() call. */
  static void Load(const uint32_t *table, uint32_t len,
                   const Jaguar3PhyContext &ctx, const Jaguar3PhyWriter &write);

private:
  /* Returns false if no case matches (caller should skip the table). On success
   * sets headline_size (number of leading headline words) and headline_idx
   * (pair index of the chosen case). */
  static bool sel_headline(const uint32_t *array, uint32_t array_len,
                           const Jaguar3PhyContext &ctx, uint8_t &headline_size,
                           uint8_t &headline_idx);
};

} /* namespace jaguar3 */

#endif /* PHY_TABLE_LOADER_8822C_H */
