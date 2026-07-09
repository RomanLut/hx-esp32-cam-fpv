#ifndef JAGUAR2_CALIBRATION_H
#define JAGUAR2_CALIBRATION_H

#include <cstdint>
#include <memory>

#include "ChipVariant.h"
#include "RtlAdapter.h"
#include "logger.h"

namespace jaguar2 {

/* Per-chip halrf IQ calibration. The bring-up flow in HalJaguar2/RtlJaguar2Device
 * is shared, but the SW IQK algorithm differs between the chips (rtl8822b's
 * 0x1b00 nctl per-path LOK/TXK/RXK vs rtl8821c's halrf_iqk_8821c path, 1T1R with
 * a 2.4G-BTG path-B). Each chip supplies its own implementation; the calibration
 * is constructed per-variant after read_chip_version (cut / rf-type known).
 *
 * Kept minimal: LC calibration (do_lck), RFE/BF init and coex antenna control
 * remain register-level methods on HalJaguar2 (branched on ChipVariant there),
 * because in Jaguar-2 they are already HAL methods rather than halrf routines. */
class Jaguar2Calibration {
public:
  virtual ~Jaguar2Calibration() = default;

  /* phy_iq_calibrate entry (SW path). band2g: true = 2.4 GHz. Runs the full
   * per-path LOK/TXK/RXK, backing up and restoring MAC/BB/RF around it. */
  virtual void iqk_trigger(bool band2g) = 0;

  /* Thermal TX-power tracking (8822B/8821C, default no-op so a
   * future variant compiles without a stub). `set_pwr_track_ctx` wires the
   * efuse thermal baseline + channel (band-table select) once bring-up is
   * complete and primes the per-path rolling average + the reverse-mapped
   * default BB-swing index (from 0xc1c[31:21]). `pwr_track` runs one
   * compensation tick — reads the RF 0x42[15:10] meter per path, averages,
   * and writes the vendor MIX_MODE swing (0xc94/0xe94 TXAGC coarse split +
   * 0xc1c/0xe1c BB scale). `current_ofdm_index` is the live path-A OFDM TXAGC
   * software shadow used for the MIX_MODE headroom (63 − idx); -1 = TXAGC
   * never written (fall back to all-swing-into-BB). Serialization against the
   * channel set / TX-power setters is the caller's responsibility. */
  virtual void set_pwr_track_ctx(uint8_t baseline, uint8_t channel) {
    (void)baseline;
    (void)channel;
  }
  virtual void pwr_track(int current_ofdm_index) { (void)current_ofdm_index; }
};

/* Factory: returns the calibration impl for the given chip. cut / is_2t2r come
 * from HalJaguar2::chip_version(), so this is called during bring-up (after
 * read_chip_version), not at device construction. */
std::unique_ptr<Jaguar2Calibration>
make_jaguar2_calibration(ChipVariant variant, RtlAdapter device,
                         Logger_t logger, uint8_t cut, bool is_2t2r);

} /* namespace jaguar2 */

#endif /* JAGUAR2_CALIBRATION_H */
