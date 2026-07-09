#ifndef JAGUAR3_PHY_TABLES_H
#define JAGUAR3_PHY_TABLES_H

#include <cstdint>
#include <memory>

#include "ChipVariant.h"

namespace jaguar3 {

/* Per-generation phydm table DATA (BB phy_reg, AGC, RF radio A/B, RFK cal-init).
 * The table WALKER (PhyTableLoaderJaguar3) and the apply sequence in HalJaguar3
 * are generation-shared — only this data differs between rtl8822c and rtl8822e.
 * HalJaguar3 holds one of these, selected by ChipVariant. */
class Jaguar3PhyTables {
public:
  struct Table {
    const uint32_t *data;
    uint32_t len;
  };

  virtual ~Jaguar3PhyTables() = default;

  virtual Table phy_reg() const = 0;   /* halhwimg *_phy_reg (BB baseline) */
  virtual Table agc_tab() const = 0;   /* halhwimg *_agc_tab */
  virtual Table radioa() const = 0;    /* halhwimg *_radioa (RF path A) */
  virtual Table radiob() const = 0;    /* halhwimg *_radiob (RF path B) */
  virtual Table cal_init() const = 0;  /* halrf rfk_init *_cal_init (0x1B00 block) */
};

/* Factory: returns the table set for the given generation. (8822e support is
 * added in a later phase; until then both variants return the 8822c tables.) */
std::unique_ptr<Jaguar3PhyTables> make_jaguar3_phy_tables(ChipVariant variant);

} /* namespace jaguar3 */

#endif /* JAGUAR3_PHY_TABLES_H */
