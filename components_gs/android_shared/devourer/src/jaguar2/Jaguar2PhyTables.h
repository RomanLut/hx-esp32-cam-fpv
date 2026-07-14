#ifndef JAGUAR2_PHY_TABLES_H
#define JAGUAR2_PHY_TABLES_H

#include <cstdint>
#include <memory>

#include "ChipVariant.h"

namespace jaguar2 {

/* Per-chip phydm table DATA (MAC, BB phy_reg, AGC, RF radio A/B) in the OLD
 * `check_positive` format — flat (addr,value) pairs with conditional blocks
 * gated on cut/pkg/interface/rfe. The table WALKER (src/PhyTableLoader) and the
 * apply sequence in HalJaguar2::apply_bb_rf_agc_tables are generation-shared —
 * only this data differs between rtl8822b and rtl8821c. HalJaguar2 holds one of
 * these, selected by ChipVariant.
 *
 * Subsumes the former PhyTableLoaderJaguar2 array-name glue. */
class Jaguar2PhyTables {
public:
  struct Table {
    const uint32_t *data;
    uint32_t len;
  };

  virtual ~Jaguar2PhyTables() = default;

  virtual Table mac_reg() const = 0; /* halhwimg *_mac_reg (reference; not applied) */
  virtual Table phy_reg() const = 0; /* halhwimg *_phy_reg (BB baseline) */
  virtual Table agc_tab() const = 0; /* halhwimg *_agc_tab */
  virtual Table radioa() const = 0;  /* halhwimg *_radioa (RF path A) */
  virtual Table radiob() const = 0;  /* halhwimg *_radiob (RF path B; 8821C = BTG) */
};

/* Factory: returns the table set for the given chip. Returns nullptr if the
 * requested variant's tables were compiled out. */
std::unique_ptr<Jaguar2PhyTables> make_jaguar2_phy_tables(ChipVariant variant);

} /* namespace jaguar2 */

#endif /* JAGUAR2_PHY_TABLES_H */
