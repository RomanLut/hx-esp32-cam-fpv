#ifndef IQK_8812A_H
#define IQK_8812A_H

#include "EepromManager.h"
#include "RfPath.h"
#include "RtlAdapter.h"
#include "logger.h"

#include <cstdint>
#include <memory>

class RadioManagementModule;
enum class BandType;

/* Port of upstream phydm's I/Q calibration for the 8812AU. Mirrors
 * `phy_iq_calibrate_8812a` -> `_phy_iq_calibrate_8812a` ->
 * `_iqk_tx_8812a` in `aircrack-ng/rtl8812au/hal/phydm/halrf/rtl8812a/
 * halrf_8812a_ce.c`. Closes the bulk of remaining T1 canary
 * divergences against the kernel reference at ch6:
 *
 *   - BB 0xc90 (rOFDM0_XCTxIQImbalance) — kernel writes IQK output,
 *     devourer left it at the BB-init seed.
 *   - BB 0xc10 / 0xe10 — RX IQK fill (path A/B IQ-correction coefs).
 *   - BB 0xcc4 / 0xcc8 / 0xccc / 0xcd4 (+ 0xec*) — TX IQK setup +
 *     correction coefs.
 *   - RF[A][0x00] / RF[B][0x00] — RF AC/mode state altered by IQK
 *     setup; restored from RFA_backup/RFB_backup at the end.
 *
 * IQK runs a TX-tone calibration loop (up to 10 iterations per path,
 * averaging when two successive samples agree within ±4) followed by
 * an RX-tone loop with the same structure. Results are filled into
 * the IQC correction registers. If a path fails to converge, default
 * coefficients (0x200, 0x0) are written — same as upstream.
 *
 * Caller responsibility: serialise calibration against channel-set,
 * TX/RX activity, and pwrtrk ticks (all of these touch BB pages).
 * IQK takes ~50-100 ms per invocation and pauses BB activity for
 * its duration.
 *
 * Out of scope for this port:
 *   - FW-offload IQK (`phydm_iqk_wait`) — devourer doesn't have the
 *     H2C mailbox plumbing; the HW path is always taken.
 *   - Per-channel result caching (`iqk_matrix_reg_setting[]` +
 *     `phy_reload_iqk_setting_8812a`) — full recompute on every
 *     trigger keeps the port small. Costs ~50 ms per channel-set
 *     when actually triggered, which is acceptable for monitor mode.
 *   - VDF (VHT-160 wide) path — devourer's matrix doesn't reach 160 MHz.
 *   - LC calibration (`_phy_lc_calibrate_8812a`) — runs separately;
 *     not in the canary-divergence path.
 *   - DPK (`_phy_dp_calibrate_8812a`) — separate post-IQK calibration.
 */
class Iqk8812a {
public:
  Iqk8812a(RtlAdapter device, std::shared_ptr<EepromManager> eepromManager,
           RadioManagementModule *radio, Logger_t logger);

  /* Run a full I/Q calibration. Channel is used by upstream's `chnl_idx`
   * lookup for the cache; we ignore it but keep the parameter for
   * symmetry with `phy_iq_calibrate_8812a`. Band drives RFE pinmux +
   * RX IQK RF setup. is_recovery == false runs the full path; true
   * is reserved for future cached-reload short-circuit. */
  void Calibrate(uint8_t channel, BandType band, bool is_recovery);

private:
  RtlAdapter _device;
  std::shared_ptr<EepromManager> _eepromManager;
  RadioManagementModule *_radio;
  Logger_t _logger;

  static constexpr int kMacBbRegNum = 9;
  static constexpr int kAfeRegNum = 12;
  static constexpr int kRfRegNum = 3;
  static constexpr int kCalNum = 10; /* upstream `#define cal_num 10` */

  void BackupMacBb(const uint32_t *regs, uint32_t *out, int n);
  void BackupRf(const uint32_t *regs, uint32_t *outA, uint32_t *outB, int n);
  void BackupAfe(const uint32_t *regs, uint32_t *out, int n);
  void ConfigureMac();
  void RestoreMacBb(const uint32_t *regs, const uint32_t *backup, int n);
  void RestoreRf(RfPath path, const uint32_t *regs, const uint32_t *backup,
                 int n);
  void RestoreAfe(const uint32_t *regs, const uint32_t *backup, int n);
  void FillTxIqc(RfPath path, int X, int Y);
  void FillRxIqc(RfPath path, int X, int Y);

  /* The big TX+RX tone loop. chnl_idx is unused inside but kept to
   * match upstream signature. */
  void DoTxRxCalibration(uint8_t chnl_idx, BandType band);
};

#endif /* IQK_8812A_H */
