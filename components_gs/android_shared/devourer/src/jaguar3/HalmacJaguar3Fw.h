#ifndef HALMAC_8822C_FW_H
#define HALMAC_8822C_FW_H

#include <cstddef>
#include <cstdint>

#include "AdapterHealth.h"
#include "logger.h"
#include "RtlAdapter.h"
#include "ChipVariant.h"

namespace jaguar3 {

/* HalmacJaguar3Fw — the Jaguar3 firmware-download (DLFW) state machine, ported
 * faithfully from Realtek HalMAC (hal/halmac/halmac_88xx/halmac_fw_88xx.c:
 * download_firmware_88xx + start_dlfw + dlfw_to_mem + iddma + check_fw_chksum +
 * dlfw_end_flow + wlan_cpu_en + pltfm_reset).
 *
 * Locked decision: DLFW is the one halmac piece ported verbatim (it is the
 * least-documented, highest-risk step — the analogue of the 8814 3081 boot).
 * Power-on / queue / EFUSE are hand-rolled elsewhere.
 *
 * The register/DDMA/checksum/boot-poll control flow plus the per-chunk transport
 * (send_fw_page() = halmac send_fwpkt_88xx -> dl_rsvd_page_88xx, a bulk-OUT of
 * the chunk with an 8822C TX descriptor into the HIQ reserved page) are ported.
 * Validated on hardware: the chip boots its firmware (REG_MCUFW_CTRL == 0xC078),
 * confirmed via usbmon diff vs the kernel rtl88x2eu driver. */
class HalmacJaguar3Fw {
public:
  HalmacJaguar3Fw(RtlAdapter device, Logger_t logger,
                ChipVariant variant = ChipVariant::C8822C);

  /* Download + boot the WLAN firmware image (fw_bin/size = the full
   * hal8822c_fw blob incl. 64-byte header). Returns true once the chip reports
   * FW ready (REG_MCUFW_CTRL == 0xC078). */
  bool download_firmware(const uint8_t *fw_bin, size_t size);

  /* Convenience: download the bundled RTL8822C NIC firmware image
   * (hal/hal8822c_fw.c). */
  bool download_default_firmware();

  /* Outcome of the last download_firmware attempt, recorded at the real
   * hardware boundaries (see src/AdapterHealth.h): checksum_ok = the IMEM/DMEM
   * checksum-ready bits (MCUFW_CTRL & 0x50), ready_ok = the FW-boot handshake
   * (MCUFW_CTRL == 0xC078). Distinguishes a transport/checksum failure from a
   * downloaded-but-never-booted MCU — the dying-adapter signature. */
  const devourer::FwBootStatus &boot_status() const { return _boot; }

private:
  /* --- ported halmac DLFW steps --- */
  bool start_dlfw(const uint8_t *fw_bin, size_t size);
  bool dlfw_to_mem(const uint8_t *fw_bin, uint32_t src, uint32_t dest,
                   uint32_t size);
  bool iddma_dlfw(uint32_t src, uint32_t dest, uint32_t len, bool first);
  bool iddma_en(uint32_t src, uint32_t dest, uint32_t ctrl);
  bool check_fw_chksum(uint32_t mem_addr);
  bool dlfw_end_flow();
  void wlan_cpu_en(bool enable);
  void pltfm_reset();
  bool chk_fw_size(const uint8_t *fw_bin, size_t size);

  /* Per-chunk transport (halmac send_fwpkt_88xx/dl_rsvd_page_88xx): build the
   * 8822C rsvd-page TX descriptor and bulk-OUT the chunk. */
  bool send_fw_page(uint16_t pg_addr, const uint8_t *chunk, uint32_t size);

  /* --- register helpers (HALMAC_REG_R/W*) --- */
  uint8_t r8(uint16_t reg);
  uint16_t r16(uint16_t reg);
  uint32_t r32(uint16_t reg);
  void w8(uint16_t reg, uint8_t v);
  void w16(uint16_t reg, uint16_t v);
  void w32(uint16_t reg, uint32_t v);
  void w32_set(uint16_t reg, uint32_t bits);

  RtlAdapter _device;
  Logger_t _logger;
  ChipVariant _variant; /* selects the firmware blob (8822c vs 8822e) */
  devourer::FwBootStatus _boot; /* last download_firmware outcome */
  /* halmac adapter->dlfw_pkt_size — the per-chunk DDMA size. */
  uint32_t _dlfw_pkt_size = 4096;
  /* halmac adapter->txff_alloc.rsvd_boundary — the reserved-page boundary the
   * rsvd-page bracket restores FIFOPAGE_CTRL_2 to. Computed by the queue/page
   * allocation during power-on. */
  uint16_t _rsvd_boundary = 0;
};

} /* namespace jaguar3 */

#endif /* HALMAC_8822C_FW_H */
