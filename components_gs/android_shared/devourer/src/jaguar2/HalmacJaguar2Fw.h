#ifndef HALMAC_8822B_FW_H
#define HALMAC_8822B_FW_H

#include <cstddef>
#include <cstdint>

#include "AdapterHealth.h"
#include "logger.h"
#include "RtlAdapter.h"
#include "ChipVariant.h"

namespace jaguar2 {

/* HalmacJaguar2Fw — the RTL8822B firmware-download (DLFW) state machine. The
 * HalMAC 88xx DLFW code (halmac_fw_88xx.c) is byte-identical between the
 * rtl88x2bu and rtl88x2cu trees and the DLFW register addresses match, so this
 * is a faithful sibling of src/jaguar3/HalmacJaguar3Fw with the ChipVariant
 * dispatch removed (8822B is a single chip). The per-chunk transport is the
 * halmac send_fwpkt/dl_rsvd_page path: build an 88xx TX descriptor for the chunk
 * (FrameParserJaguar2) and bulk-OUT it into the HIQ reserved page.
 *
 * Boot success = REG_MCUFW_CTRL == 0xC078. Requires the pre-DLFW MAC config
 * (DDMA enable, SYS_FUNC_EN) and the queue/page allocation (_rsvd_boundary) to
 * have run first — supplied by HalmacJaguar2MacInit. */
class HalmacJaguar2Fw {
public:
  HalmacJaguar2Fw(RtlAdapter device, Logger_t logger,
                  ChipVariant variant = ChipVariant::C8822B);

  bool download_firmware(const uint8_t *fw_bin, size_t size);
  bool download_default_firmware(); /* bundled per-variant NIC image */

  /* Outcome of the last download_firmware attempt, recorded at the real
   * hardware boundaries (see src/AdapterHealth.h): checksum_ok = the IMEM/DMEM
   * checksum-ready bits (MCUFW_CTRL & 0x50), ready_ok = the FW-boot handshake
   * (MCUFW_CTRL == 0xC078). Distinguishes a transport/checksum failure from a
   * downloaded-but-never-booted MCU — the dying-adapter signature. */
  const devourer::FwBootStatus &boot_status() const { return _boot; }

  /* The reserved-page boundary FIFOPAGE_CTRL_2 is restored to after each
   * rsvd-page chunk (from the queue/page allocation). Set by MacInit before
   * download_firmware(). */
  void set_rsvd_boundary(uint16_t b) { _rsvd_boundary = b; }
  uint16_t rsvd_boundary() const { return _rsvd_boundary; }

  /* Download an FW reserved-page template blob to page `pg_addr` (halmac
   * dl_rsvd_page_88xx == send_fw_page). The FW requires its reserved-page set
   * before it enables the MAC TX scheduler; without it RX works but TX frames
   * never leave the MAC. */
  bool download_rsvd_page(uint16_t pg_addr, const uint8_t *buf, uint32_t size) {
    return send_fw_page(pg_addr, buf, size);
  }

private:
  bool start_dlfw(const uint8_t *fw_bin, size_t size);
  bool dlfw_to_mem(const uint8_t *fw_bin, uint32_t src, uint32_t dest,
                   uint32_t size);
  bool iddma_dlfw(uint32_t src, uint32_t dest, uint32_t len, bool first);
  bool iddma_en(uint32_t src, uint32_t dest, uint32_t ctrl);
  bool check_fw_chksum(uint32_t mem_addr);
  bool dlfw_end_flow();
  void wlan_cpu_en(bool enable);
  void pltfm_reset();
  bool ltecoex_read(uint16_t offset, uint32_t &val);
  bool ltecoex_write(uint16_t offset, uint32_t val);
  bool chk_fw_size(const uint8_t *fw_bin, size_t size);
  bool send_fw_page(uint16_t pg_addr, const uint8_t *chunk, uint32_t size);

  uint8_t r8(uint16_t reg);
  uint16_t r16(uint16_t reg);
  uint32_t r32(uint16_t reg);
  void w8(uint16_t reg, uint8_t v);
  void w16(uint16_t reg, uint16_t v);
  void w32(uint16_t reg, uint32_t v);
  void w32_set(uint16_t reg, uint32_t bits);

  RtlAdapter _device;
  Logger_t _logger;
  ChipVariant _variant;
  devourer::FwBootStatus _boot; /* last download_firmware outcome */
  /* Per-chunk DLFW size. The vendor 8822B driver downloads in 4096-byte chunks
   * (4144-byte bulk-OUT incl. the 48-byte TX desc); this fits the HIQ page
   * space allocated by init_trx_cfg (64 pages) with margin. DLFW_PKT_MAX_SIZE is
   * 8192 upstream, but 8192+48 exceeds the 8192-byte HIQ, so 4096 is used. */
  uint32_t _dlfw_pkt_size = 4096;
  uint16_t _rsvd_boundary = 0;
  /* Packet-offset (bytes) applied by the last send_fw_page() for an
   * exact-bulk-multiple chunk; the iddma source skips it to reach the payload. */
  uint32_t _last_pkt_offset = 0;
};

} /* namespace jaguar2 */

#endif /* HALMAC_8822B_FW_H */
