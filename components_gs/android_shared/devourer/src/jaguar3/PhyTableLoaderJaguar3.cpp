#include "PhyTableLoaderJaguar3.h"

namespace jaguar3 {

namespace {
/* halbb opcode nibbles (v1 >> 28) and don't-care sentinels, from
 * hal/phydm/rtl8822c/halhwimg8822c_bb.c. */
constexpr uint32_t PARA_IF = 0x8;
constexpr uint32_t PARA_ELSE_IF = 0x9;
constexpr uint32_t PARA_ELSE = 0xa;
constexpr uint32_t PARA_END = 0xb;
constexpr uint32_t PARA_CHK = 0x4;
constexpr uint32_t CUT_DONT_CARE = 0xf;
constexpr uint32_t RFE_DONT_CARE = 0xff;
} /* namespace */

bool PhyTableLoaderJaguar3::sel_headline(const uint32_t *array, uint32_t array_len,
                                       const Jaguar3PhyContext &ctx,
                                       uint8_t &headline_size,
                                       uint8_t &headline_idx) {
  const uint32_t cut_drv = ctx.cut_version;
  const uint32_t rfe_drv = ctx.rfe_type;
  uint32_t i = 0;

  headline_idx = 0;
  headline_size = 0;

  /* Headline rows have v1>>28 == 0xf; the first non-0xf row ends the section. */
  while ((i + 1) < array_len) {
    if ((array[i] >> 28) != 0xf) {
      headline_size = static_cast<uint8_t>(i);
      break;
    }
    i += 2;
  }
  if (i == 0)
    return true; /* no headline → all default-case writes apply */

  /* [1] {RFE:Match, CUT:Match} */
  uint32_t compare_target = ((cut_drv & 0x0f) << 24) | (rfe_drv & 0xff);
  for (i = 0; i < headline_size; i += 2) {
    if ((array[i] & 0x0f0000ff) == compare_target) {
      headline_idx = static_cast<uint8_t>(i >> 1);
      return true;
    }
  }

  /* [2] {RFE:Match, CUT:Dont_Care} */
  compare_target = (CUT_DONT_CARE << 24) | (rfe_drv & 0xff);
  for (i = 0; i < headline_size; i += 2) {
    if ((array[i] & 0x0f0000ff) == compare_target) {
      headline_idx = static_cast<uint8_t>(i >> 1);
      return true;
    }
  }

  /* [3] {RFE:Match, CUT:Max_in_table} */
  bool case_match = false;
  uint32_t cut_max = 0;
  for (i = 0; i < headline_size; i += 2) {
    const uint32_t rfe_para = array[i] & 0xff;
    const uint32_t cut_para = (array[i] & 0x0f000000) >> 24;
    if (rfe_para == rfe_drv && cut_para >= cut_max) {
      cut_max = cut_para;
      headline_idx = static_cast<uint8_t>(i >> 1);
      case_match = true;
    }
  }
  if (case_match)
    return true;

  /* [4] {RFE:Dont_Care, CUT:Max_in_table} */
  cut_max = 0;
  for (i = 0; i < headline_size; i += 2) {
    const uint32_t rfe_para = array[i] & 0xff;
    const uint32_t cut_para = (array[i] & 0x0f000000) >> 24;
    if (rfe_para == RFE_DONT_CARE && cut_para >= cut_max) {
      cut_max = cut_para;
      headline_idx = static_cast<uint8_t>(i >> 1);
      case_match = true;
    }
  }
  if (case_match)
    return true;

  /* [5] no case matched */
  return false;
}

void PhyTableLoaderJaguar3::Load(const uint32_t *table, uint32_t array_len,
                               const Jaguar3PhyContext &ctx,
                               const Jaguar3PhyWriter &write) {
  uint8_t h_size = 0;
  uint8_t h_idx = 0;
  if (!sel_headline(table, array_len, ctx, h_size, h_idx))
    return; /* no matching case — skip this table */

  uint32_t cfg_target = 0;
  if (h_size != 0)
    cfg_target = table[h_idx << 1] & 0x0fffffff;

  uint32_t i = h_size;
  uint32_t cfg_para = 0;
  /* halbb_flag_2_default: matching on, target not yet found. */
  bool is_matched = true;
  bool find_target = false;

  while ((i + 1) < array_len) {
    const uint32_t v1 = table[i];
    const uint32_t v2 = table[i + 1];
    i += 2;

    switch (v1 >> 28) {
    case PARA_IF:
    case PARA_ELSE_IF:
      cfg_para = v1 & 0x0fffffff;
      break;
    case PARA_ELSE:
      is_matched = false;
      if (!find_target)
        return; /* nothing matched in this if/else chain — vendor aborts */
      break;
    case PARA_END:
      is_matched = true;
      find_target = false;
      break;
    case PARA_CHK:
      if (find_target) {
        is_matched = false; /* already satisfied an earlier branch */
      } else if (cfg_para == cfg_target) {
        is_matched = true;
        find_target = true;
      } else {
        is_matched = false;
        find_target = false;
      }
      break;
    default:
      if (is_matched)
        write(v1, v2);
      break;
    }
  }
}

} /* namespace jaguar3 */
