#ifndef PHY_TABLE_LOADER_H
#define PHY_TABLE_LOADER_H

#include <cstddef>
#include <cstdint>
#include <functional>

/* Loader for the Realtek phydm-style register tables shipped with RTL8814AU
 * (BB / AGC / MAC). The on-disk format encodes runtime-conditional blocks via
 * opcode words in the high nibble; see hal/phydm/rtl8814a/halhwimg8814a_bb.c
 * for the canonical encoding upstream. This loader replicates upstream's
 * `check_positive` + state-machine walk without pulling in the phydm
 * subsystem — `JaguarPhyContext` is the minimal subset of `dm_struct` that
 * `check_positive` reads. */

struct JaguarPhyContext {
  uint8_t  cut_version;        /* HAL_CUT_VERSION_E from EepromManager */
  uint8_t  support_interface;  /* ODM_ITRF_USB == 0x02 in upstream phydm */
  uint8_t  support_platform;   /* ODM_CE == 0x04 in upstream phydm */
  uint8_t  package_type;       /* 0 unless EFUSE-overridden */
  uint8_t  rfe_type;           /* EFUSE rfe_type */
  uint16_t type_glna;
  uint16_t type_gpa;
  uint16_t type_alna;
  uint16_t type_apa;
};

class PhyTableLoader {
public:
  /* Writer is invoked once per (addr, value) entry that passes the conditional
   * filter. For BB/AGC tables, write is a full 32-bit register write; for the
   * MAC table the value's low byte holds the byte-wide write — the caller
   * decides via the writer it supplies. */
  using Writer = std::function<void(uint32_t addr, uint32_t value)>;

  static void Load(const uint32_t* table, std::size_t len,
                   const JaguarPhyContext& ctx, const Writer& write);

private:
  static bool CheckPositive(const JaguarPhyContext& ctx, uint32_t c1, uint32_t c2,
                            uint32_t c3, uint32_t c4);
};

#endif /* PHY_TABLE_LOADER_H */
