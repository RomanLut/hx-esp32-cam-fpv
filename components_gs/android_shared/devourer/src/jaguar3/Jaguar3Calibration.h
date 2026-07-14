#ifndef JAGUAR3_CALIBRATION_H
#define JAGUAR3_CALIBRATION_H

#include <cstdint>
#include <memory>

#include "ChipVariant.h"
#include "SelectedChannel.h" /* ChannelWidth_t */
#include "RtlAdapter.h"
#include "logger.h"

namespace jaguar3 {

/* Per-generation halrf calibration + coex antenna control. The bring-up flow in
 * HalJaguar3 is shared, but these routines differ fundamentally between the
 * generations — e.g. rtl8822c DACK samples the BB DC report (0x2dbc) while
 * rtl8822e DACK uses the AFE S0/S1 banks (0x3800/0x3900). Each generation
 * supplies its own implementation; HalJaguar3 holds one, selected by variant.
 *
 * Coex hooks are meaningful on the WiFi+BT combo CU; the WiFi-only EU may
 * implement them as light no-ops. */
class Jaguar3Calibration {
public:
  virtual ~Jaguar3Calibration() = default;

  /* RF calibration — run after the channel is tuned (reads RF18 for band/ch). */
  virtual void phy_iq_calibrate(ChannelWidth_t bw, uint8_t channel) = 0;
  /* DAC DC/gain calibration — run during bring-up before IQK. */
  virtual void dac_calibrate() = 0;
  /* Thermal TX-power tracking + synth re-lock; called periodically during TX. */
  virtual void pwr_track() = 0;

  /* Supply pwr_track() with the efuse thermal baseline (per path) and the
   * operating channel, read once during bring-up. Default no-op (the 8822c reads
   * its own context; only the 8822e thermal tracker needs this). */
  virtual void set_pwr_track_ctx(uint8_t /*thermal_base_a*/,
                                 uint8_t /*thermal_base_b*/,
                                 uint8_t /*channel*/) {}

  /* One-shot thermal-meter read (path A) for IRtlDevice::GetThermalStatus:
   * `raw` = the live RF 0x42[6:1] reading, `baseline` = the calibration
   * reference — the efuse baseline on the 8822e (0xFF = unprogrammed), the
   * first-read cold reference on the 8822c (no efuse baseline is wired on the
   * CU, so its delta means "since first read"). Read-only apart from the
   * meter's own one-time enable trigger (RF 0x42[19]) — does NOT write the
   * pwr_track swing registers. Returns false when unimplemented / the meter
   * is not ready. Caller must serialize against pwr_track (same trigger RMW). */
  virtual bool read_thermal(uint8_t & /*raw*/, uint8_t & /*baseline*/) {
    return false;
  }

  /* Coex antenna control (combo chips). */
  virtual void force_wl_antenna() = 0;
  virtual void coex_wlan_only_init() = 0;
  virtual void coex_run_5g() = 0;
  virtual void coex_keepalive() = 0;
};

/* Factory: returns the calibration impl for the given generation. (8822e is
 * added in a later phase; until then both variants return the 8822c impl.) */
std::unique_ptr<Jaguar3Calibration>
make_jaguar3_calibration(ChipVariant variant, RtlAdapter device,
                         Logger_t logger,
                         const devourer::DeviceConfig &cfg = {});

} /* namespace jaguar3 */

#endif /* JAGUAR3_CALIBRATION_H */
