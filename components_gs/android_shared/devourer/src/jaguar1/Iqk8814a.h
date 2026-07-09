#ifndef IQK_8814A_H
#define IQK_8814A_H

#include "EepromManager.h"
#include "RfPath.h"
#include "RtlAdapter.h"
#include "logger.h"

#include <cstdint>
#include <memory>

class RadioManagementModule;
enum class BandType;

/* Port of upstream phydm's I/Q calibration for the 8814AU. Mirrors
 * `phy_iq_calibrate_8814a` -> `_phy_iq_calibrate_8814a` ->
 * `_IQK_Tx_8814A` in `aircrack-ng/rtl8812au/hal/phydm/halrf/rtl8814a/
 * halrf_iqk_8814a.c`.
 *
 * 8814 IQK differs structurally from 8812 IQK:
 *   - 4 RF paths (A/B/C/D) instead of 2.
 *   - HW-driven via the 0x1b00 NCO Control trigger register. Devourer
 *     writes a CMD ID into 0x1b00, polls 0x1b00 bit 0 for completion,
 *     and reads result coefficients from 0x1b38 / 0x1b3c — no
 *     iterative tone-sweep loop.
 *   - Separate LO leakage calibration (LOK) phase before TX/RX-IQK.
 *   - Path-C/D RFE pinmux at 0x18b4 / 0x1ab4 / 0x1abc included in
 *     backup set.
 *
 * Closes the documented post-channel-set RF[A]/[B] 0x00 bit-15
 * divergence at 5G (kernel achieves bit 15 = 0 by running this
 * routine after channel-set; devourer without it sits in SW-LNA
 * mode).
 *
 * IQK takes ~50-100 ms per invocation. Caller serialises against
 * channel-set, TX/RX activity, pwrtrk ticks.
 *
 * Out of scope:
 *   - FW-offload IQK — devourer has no H2C mailbox.
 *   - Per-channel IQC matrix caching — full recompute each trigger.
 *   - LC calibration / DPK — separate calibrations not in canary path.
 */
class Iqk8814a {
public:
  Iqk8814a(RtlAdapter device, std::shared_ptr<EepromManager> eepromManager,
           RadioManagementModule *radio, Logger_t logger);

  /* Run a full I/Q calibration for 8814A. is_recovery == false runs
   * the full LOK + TX-IQK + RX-IQK path on all four RF paths. true
   * is reserved for the cached-reload short-circuit (not yet wired). */
  void Calibrate(uint8_t channel, BandType band, bool is_recovery);

private:
  RtlAdapter _device;
  std::shared_ptr<EepromManager> _eepromManager;
  RadioManagementModule *_radio;
  Logger_t _logger;

  static constexpr int kMacRegNum = 2;
  static constexpr int kBbRegNum = 13;
  static constexpr int kRfRegNum = 2;

  void BackupMacBb(const uint32_t *macRegs, uint32_t *macOut,
                   const uint32_t *bbRegs, uint32_t *bbOut);
  void BackupRf(const uint32_t *regs, uint32_t out[][4]);
  void AFESetting(bool doIqk);
  void RestoreMacBb(const uint32_t *macRegs, const uint32_t *macBackup,
                    const uint32_t *bbRegs, const uint32_t *bbBackup);
  void RestoreRf(const uint32_t *regs, const uint32_t backup[][4]);
  void ResetNCTL();
  void ConfigureMAC();
  void IqkTx(BandType band);
  void LokOneShot();
  void IqkOneShot();
};

#endif /* IQK_8814A_H */
