#include "PhyTableLoader.h"

namespace {

/* Opcode encoding (high nibble of v1):
 *   0x8 — IF       (c_cond bits 28-29 = 0b00)
 *   0x9 — ELSE_IF  (c_cond bits 28-29 = 0b01)
 *   0xA — ELSE     (c_cond bits 28-29 = 0b10)
 *   0xB — ENDIF    (c_cond bits 28-29 = 0b11)
 *   0x4 — negative condition (second half of IF/ELSE_IF; carries c3/c4) */
constexpr uint32_t BIT_POSITIVE = 1u << 31;
constexpr uint32_t BIT_NEGATIVE = 1u << 30;
constexpr uint32_t C_COND_MASK = (1u << 29) | (1u << 28);
constexpr uint32_t C_COND_SHIFT = 28;

constexpr uint8_t COND_IF = 0;
constexpr uint8_t COND_ELSE_IF = 1;
constexpr uint8_t COND_ELSE = 2;
constexpr uint8_t COND_ENDIF = 3;

constexpr uint8_t ODM_CUT_A = 0; /* matches upstream phydm */

}  // namespace

/* Translated verbatim from check_positive() in upstream
 * hal/phydm/rtl8814a/halhwimg8814a_bb.c. Returns true if the runtime
 * configuration in `ctx` matches the (cond1..cond4) selector. */
bool PhyTableLoader::CheckPositive(const JaguarPhyContext& ctx, uint32_t c1,
                                   uint32_t /*c2*/, uint32_t /*c3*/,
                                   uint32_t /*c4*/) {
  const uint8_t cut_for_para =
      (ctx.cut_version == ODM_CUT_A) ? 15 : ctx.cut_version;
  const uint8_t pkg_for_para = (ctx.package_type == 0) ? 15 : ctx.package_type;

  const uint32_t driver1 =
      (static_cast<uint32_t>(cut_for_para) << 24) |
      ((ctx.support_interface & 0xF0u) << 16) |
      (static_cast<uint32_t>(ctx.support_platform) << 16) |
      (static_cast<uint32_t>(pkg_for_para) << 12) |
      ((ctx.support_interface & 0x0Fu) << 8) |
      static_cast<uint32_t>(ctx.rfe_type);

  /* cut version [27:24] */
  if ((c1 & 0x0F000000u) != 0 && (c1 & 0x0F000000u) != (driver1 & 0x0F000000u))
    return false;
  /* pkg type [15:12] */
  if ((c1 & 0x0000F000u) != 0 && (c1 & 0x0000F000u) != (driver1 & 0x0000F000u))
    return false;
  /* interface [11:8] */
  if ((c1 & 0x00000F00u) != 0 && (c1 & 0x00000F00u) != (driver1 & 0x00000F00u))
    return false;

  /* Low byte (rfe_type) — exact match required when both sides are nonzero. */
  return (c1 & 0xFFu) == (driver1 & 0xFFu);
}

/* Walks the phydm table state machine. Mirrors odm_read_and_config_mp_8814a_*
 * in upstream. */
void PhyTableLoader::Load(const uint32_t* table, std::size_t len,
                          const JaguarPhyContext& ctx, const Writer& write) {
  bool is_matched = true;
  bool is_skipped = false;
  uint32_t pre_v1 = 0;
  uint32_t pre_v2 = 0;

  std::size_t i = 0;
  while (i + 1 < len) {
    const uint32_t v1 = table[i];
    const uint32_t v2 = table[i + 1];

    if (v1 & (BIT_POSITIVE | BIT_NEGATIVE)) {
      if (v1 & BIT_POSITIVE) {
        const uint8_t c_cond =
            static_cast<uint8_t>((v1 & C_COND_MASK) >> C_COND_SHIFT);
        if (c_cond == COND_ENDIF) {
          is_matched = true;
          is_skipped = false;
        } else if (c_cond == COND_ELSE) {
          is_matched = is_skipped ? false : true;
        } else {
          /* IF / ELSE_IF — save the positive half; the negative half on the
           * next iteration completes the condition group. */
          pre_v1 = v1;
          pre_v2 = v2;
        }
      } else if (v1 & BIT_NEGATIVE) {
        if (!is_skipped) {
          if (CheckPositive(ctx, pre_v1, pre_v2, v1, v2)) {
            is_matched = true;
            is_skipped = true;
          } else {
            is_matched = false;
            is_skipped = false;
          }
        } else {
          is_matched = false;
        }
      }
    } else {
      if (is_matched)
        write(v1, v2);
    }
    i += 2;
  }
}
