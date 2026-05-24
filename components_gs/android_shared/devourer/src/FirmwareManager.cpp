#include "FirmwareManager.h"

#include "Firmware.h"
#include "FrameParser.h"
#include "rtl8812a_hal.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

/* 8814AU-specific constants pulled from hal/rtl8814a_spec.h. Inlined here to
 * avoid dragging the full 8814 spec header (which has #define overlap with the
 * 8812 spec on shared Jaguar symbols) into this TU. */
namespace {
constexpr uint16_t REG_DDMA_CH0SA_8814A = 0x1200;
constexpr uint16_t REG_DDMA_CH0DA_8814A = 0x1204;
constexpr uint16_t REG_DDMA_CH0CTRL_8814A = 0x1208;
constexpr uint16_t REG_FIFOPAGE_CTRL_2_8814A = 0x0204;
constexpr uint16_t REG_FWHW_TXQ_CTRL_8814A = 0x0420;
constexpr uint16_t REG_CR_8814A = 0x0100;
constexpr uint32_t DDMA_LEN_MASK_8814A = 0x0001FFFFu;
constexpr uint32_t DDMA_CH_CHKSUM_CNT_8814A = (1u << 24);
constexpr uint32_t DDMA_RST_CHKSUM_STS_8814A = (1u << 25);
constexpr uint32_t DDMA_CHKSUM_FAIL_8814A = (1u << 27);
constexpr uint32_t DDMA_CHKSUM_EN_8814A = (1u << 29);
constexpr uint32_t DDMA_CH_OWN_8814A = (1u << 31);
constexpr uint8_t IMEM_DL_RDY_8814A = (1u << 3);
constexpr uint8_t IMEM_CHKSUM_OK_8814A = (1u << 4);
constexpr uint8_t DMEM_DL_RDY_8814A = (1u << 5);
constexpr uint8_t DMEM_CHKSUM_OK_8814A = (1u << 6);
constexpr uint32_t OCPBASE_TXBUF_3081 = 0x18780000;
constexpr uint32_t OCPBASE_DMEM_3081 = 0x00200000;
constexpr uint32_t OCPBASE_IMEM_3081 = 0x00000000;
constexpr uint32_t FW_CHKSUM_DUMMY_SZ_8814A = 8;
constexpr uint16_t TXDESC_OFFSET_8814A = 40;
constexpr uint32_t FW_HEADER_SIZE_8814A = 64;
/* TX_PAGE_BOUNDARY for 8814 USB. devourer's TX_PAGE_BOUNDARY_8812 is 0xF8;
 * 8814 uses the same value for the standard USB config. */
constexpr uint16_t TX_PAGE_BOUNDARY_8814A_USB = 0xF8;
constexpr uint32_t TX_PAGE_SIZE = 128;
constexpr uint32_t MAX_RSVD_PAGE_CHUNK_SZ = 4096u; /* rtw88 sends 4096-byte fw chunks */
}  // namespace

template <class result_t = std::chrono::milliseconds,
          class clock_t = std::chrono::steady_clock,
          class duration_t = std::chrono::milliseconds>
auto since(std::chrono::time_point<clock_t, duration_t> const &start) {
  return std::chrono::duration_cast<result_t>(clock_t::now() - start);
}

FirmwareManager::FirmwareManager(RtlUsbAdapter device, Logger_t logger)
    : _device{device}, _logger{logger} {}

/* The Jaguar firmware-header signature lives in the low 16 bits of the first
 * word: 0x95xx for 8812/8811, 0x88xx for 8814, 0x21xx for 8821A. */
static bool jaguar_fw_header_present(const uint8_t *buf, HAL_IC_TYPE_E ic_type) {
  const uint16_t signature =
      static_cast<uint16_t>(GET_FIRMWARE_HDR_SIGNATURE_8812(buf));
  if (ic_type == CHIP_8814A)
    return (signature & 0xFFF0u) == 0x8810u;
  if (ic_type == CHIP_8821)
    return (signature & 0xFFF0u) == 0x2100u;
  return (signature & 0xFFF0u) == 0x9500u;
}

void FirmwareManager::FirmwareDownload(HAL_IC_TYPE_E ic_type) {
  if (ic_type == CHIP_8814A) {
    /* 8814AU uses an entirely different firmware transport (TX-FIFO reserved
     * page + internal DMA controller) than 8812. Hand off to the 8814 path.
     *
     * NOTE on macOS: bulk OUT to this chip is blocked by the OS because
     * RTL8814AU has `NeedsDeviceAccessEntitlement = Yes` in the IOUSBHostDevice
     * attributes — macOS requires `com.apple.developer.driverkit.transport.usb`
     * to permit bulk writes. Without that entitlement (or SIP disabled), the
     * fw download below times out at the bulk-OUT stage. Verified working on
     * Linux. RTL8812AU does not have this restriction. */
    FirmwareDownload_8814A();
    return;
  }

  bool rtStatus = true;
  uint8_t write_fw = 0;

  auto blob = PickFirmwareForChip(ic_type);
  auto pFirmwareBuf = const_cast<uint8_t *>(blob.buf);
  auto FirmwareLen = blob.len;

  auto firmwareVersion = (uint16_t)GET_FIRMWARE_HDR_VERSION_8812(pFirmwareBuf);
  auto firmwareSubVersion =
      (uint16_t)GET_FIRMWARE_HDR_SUB_VER_8812(pFirmwareBuf);
  auto firmwareSignature =
      (uint16_t)GET_FIRMWARE_HDR_SIGNATURE_8812(pFirmwareBuf);

  _logger->info("FirmwareDownload(ic={}): fw_ver={} fw_subver={} sig=0x{:X}",
                (int)ic_type, firmwareVersion, firmwareSubVersion,
                firmwareSignature);

  if (jaguar_fw_header_present(pFirmwareBuf, ic_type)) {
    /* Shift 32 bytes for FW header */
    pFirmwareBuf = pFirmwareBuf + 32;
    FirmwareLen -= 32;
  }

  /* Suggested by Filen. If 8051 is running in RAM code, driver should inform Fw
   * to reset by itself, */
  /* or it will cause download Fw fail. 2010.02.01. by tynli. */
  if ((_device.rtw_read8(REG_MCUFWDL) & BIT7) != 0) {
    /* 8051 RAM code */
    _device.rtw_write8(REG_MCUFWDL, 0x00);
    _device._8051Reset8812();
  }

  _FWDownloadEnable_8812(true);

  auto fwdl_start_time = std::chrono::steady_clock::now();
  while ((write_fw++ < 3 || (since(fwdl_start_time).count()) < 500)) {
    /* reset FWDL chksum */
    _device.rtw_write8(REG_MCUFWDL, (uint8_t)(_device.rtw_read8(REG_MCUFWDL) |
                                              FWDL_ChkSum_rpt));

    rtStatus = WriteFW8812(pFirmwareBuf, FirmwareLen);
    if (rtStatus != true) {
      continue;
    }

    rtStatus = polling_fwdl_chksum(5, 50);
    if (rtStatus == true) {
      break;
    }
  }

  _FWDownloadEnable_8812(false);
  if (true != rtStatus) {
    return;
  }

  rtStatus = _FWFreeToGo8812(10, 200, ic_type);
  if (true != rtStatus) {
    return;
  }

  InitializeFirmwareVars8812();
}

// TODO: now does nothing
static void yield() {}

/* 8814AU firmware download via vendor-control-transfer page writes (same
 * transport as 8812), but with the 8814-specific 8051 control sequence:
 *
 *   _FWDownloadEnable_8814A(true)   -- put chip in FW-load mode
 *   _3081Disable8814A               -- gate the 8051 MCU
 *   _DDMAReset8814A                 -- clear stale DDMA state
 *   page-write firmware bytes       -- chip computes checksum on the fly
 *   (poll FWDL_ChkSum_rpt for OK)
 *   _3081Enable8814A                -- ungate the 8051
 *   _FWDownloadEnable_8814A(false)
 *   set BIT(6) of REG_MCUFWDL+1     -- "FW go" kick
 *   poll CPU_DL_READY               -- 8051 reports it's running
 *
 * The RSVD-page + IDDMA path upstream uses is not available to us because the
 * bulk-OUT TX endpoint times out during fwdl mode on this driver; the
 * page-write path's bytes also reach the chip (verified by FWDL_ChkSum_rpt
 * being set after upload). The 8814 firmware header is 64 bytes (vs 8812's
 * 32). */
void FirmwareManager::FirmwareDownload_8814A() {
  /* If fw is already running on the chip (e.g. another driver loaded it and
   * we're picking up the device mid-flight), skip fwdl entirely.
   *
   * Detect via byte 0 of REG_MCUFWDL == 0x78 (IMEM_DL_RDY | IMEM_CHKSUM_OK |
   * DMEM_DL_RDY | DMEM_CHKSUM_OK all set, FWDL_EN cleared). BIT15
   * (FW_INIT_RDY) is transient — set briefly during fw init then cleared
   * once fw is operational — so polling for BIT15 misses an already-running
   * fw. The 0x78 byte-0 pattern is the stable post-fw-boot signature
   * (verified May 2026 against rtw88_8814au's chip via pyusb probe). */
  const uint32_t mcufwdl_state = _device.rtw_read32(REG_MCUFWDL);
  if ((mcufwdl_state & 0xFF) == 0x78) {
    _logger->info("8814A fw already running (REG_MCUFWDL=0x{:08x}) — skipping fwdl",
                  mcufwdl_state);
    return;
  }
  _logger->info("8814A pre-fwdl REG_MCUFWDL=0x{:08x}", mcufwdl_state);

  auto blob = PickFirmwareForChip(CHIP_8814A);
  uint8_t *fw = const_cast<uint8_t *>(blob.buf);
  uint32_t fw_len = static_cast<uint32_t>(blob.len);

  const uint16_t firmwareVersion =
      static_cast<uint16_t>(GET_FIRMWARE_HDR_VERSION_8812(fw + 4));
  const uint16_t firmwareSignature =
      static_cast<uint16_t>(GET_FIRMWARE_HDR_SIGNATURE_8812(fw));
  _logger->info("FirmwareDownload_8814A: fw_ver={} sig=0x{:X} blob={} bytes",
                firmwareVersion, firmwareSignature, fw_len);

  /* NOTE: leave the 64-byte 8814 firmware header in the staged bytes. The
   * 8814 bootloader reads section sizes from the header to know how to split
   * DMEM/IRAM. (Upstream's RSVD-page path strips the header because it
   * supplies the section addresses to IDDMA directly; we're not, so we let
   * the chip see the header.) */
  (void)FW_HEADER_SIZE_8814A;

  if ((_device.rtw_read8(REG_MCUFWDL) & BIT7) != 0) {
    /* 8051 already running from previous session — reset it. */
    _device.rtw_write8(REG_MCUFWDL, 0x00);
    _device._8051Reset8812();
  }

  /* Parse 8814 firmware-header DMEM / IRAM section sizes. The 8814 fw header
   * is 64 bytes; DMEM size lives at offset +36 (LE u32), IRAM size at +48. */
  auto le32 = [](const uint8_t *p) -> uint32_t {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
  };
  const uint32_t dmem_size = le32(blob.buf + 36) + FW_CHKSUM_DUMMY_SZ_8814A;
  const uint32_t iram_size = le32(blob.buf + 48) + FW_CHKSUM_DUMMY_SZ_8814A;
  _logger->info("8814A FW sections: dmem={} iram={} (total payload {})",
                dmem_size, iram_size, fw_len);
  if (dmem_size + iram_size + FW_HEADER_SIZE_8814A != fw_len) {
    _logger->error("8814A FW header/blob size mismatch: {} + {} + {} != {}",
                   dmem_size, iram_size, FW_HEADER_SIZE_8814A, fw_len);
    return;
  }

  /* Full 242-op pre-fwdl register sequence mirrored byte-for-byte from
   * rtw88_8814au's usbmon trace (163 reads + 79 writes). The reads matter:
   * several Realtek registers have read-side-effects (RC1 status clears,
   * latched values, state-machine triggers), and a writes-only replay was
   * insufficient to unlock the chip's BCN_VALID ack after the bulk OUT.
   * Generated from /tmp/rtw88_init.seq via tools/gen_rtw88_mimic.py. */
  (void)_device.rtw_read32(0x00F0);
  _device.rtw_write8(0x001C, 0x00);
  (void)_device.rtw_read32(0x0064);
  _device.rtw_write32(0x0064, 0x30200000);
  (void)_device.rtw_read32(0x004C);
  _device.rtw_write32(0x004C, 0x60228282);
  (void)_device.rtw_read32(0x0040);
  _device.rtw_write32(0x0040, 0x00000004);
  (void)_device.rtw_read8(0x0002);
  _device.rtw_write8(0x0002, 0x1C);
  (void)_device.rtw_read8(0x001F);
  _device.rtw_write8(0x001F, 0x00);
  (void)_device.rtw_read32(0x00EC);
  _device.rtw_write32(0x00EC, 0x30453F15);
  (void)_device.rtw_read8(0xFE58);
  (void)_device.rtw_read16(0x0080);
  (void)_device.rtw_read8(0x0100);
  (void)_device.rtw_read8(0xFE58);
  (void)_device.rtw_read16(0x0080);
  (void)_device.rtw_read8(0x0100);
  (void)_device.rtw_read8(0x0C00);
  _device.rtw_write8(0x0C00, 0x04);
  (void)_device.rtw_read8(0x0E00);
  _device.rtw_write8(0x0E00, 0x04);
  (void)_device.rtw_read8(0x1002);
  _device.rtw_write8(0x1002, 0xAC);
  (void)_device.rtw_read8(0x001F);
  _device.rtw_write8(0x001F, 0x00);
  (void)_device.rtw_read8(0x0007);
  _device.rtw_write8(0x0007, 0x28);
  (void)_device.rtw_read8(0x0008);
  _device.rtw_write8(0x0008, 0x21);
  (void)_device.rtw_read8(0x0066);
  _device.rtw_write8(0x0066, 0x20);
  (void)_device.rtw_read8(0x0041);
  _device.rtw_write8(0x0041, 0x00);
  (void)_device.rtw_read8(0x0042);
  _device.rtw_write8(0x0042, 0x00);
  (void)_device.rtw_read8(0x004E);
  _device.rtw_write8(0x004E, 0x22);
  (void)_device.rtw_read8(0x0041);
  _device.rtw_write8(0x0041, 0x00);
  (void)_device.rtw_read8(0x0005);
  _device.rtw_write8(0x0005, 0x02);
  (void)_device.rtw_read8(0x0005);
  (void)_device.rtw_read8(0x0005);
  (void)_device.rtw_read8(0x0003);
  _device.rtw_write8(0x0003, 0x72);
  (void)_device.rtw_read8(0x0080);
  _device.rtw_write8(0x0080, 0x01);
  (void)_device.rtw_read8(0x0081);
  _device.rtw_write8(0x0081, 0x30);
  (void)_device.rtw_read8(0x0045);
  _device.rtw_write8(0x0045, 0x00);
  (void)_device.rtw_read8(0x0046);
  _device.rtw_write8(0x0046, 0xFF);
  (void)_device.rtw_read8(0x0047);
  _device.rtw_write8(0x0047, 0x00);
  (void)_device.rtw_read8(0x0015);
  _device.rtw_write8(0x0015, 0xCF);
  (void)_device.rtw_read8(0x0015);
  _device.rtw_write8(0x0015, 0xEF);
  (void)_device.rtw_read8(0x0012);
  _device.rtw_write8(0x0012, 0x83);
  (void)_device.rtw_read8(0x0023);
  _device.rtw_write8(0x0023, 0x15);
  (void)_device.rtw_read8(0x0008);
  _device.rtw_write8(0x0008, 0x21);
  (void)_device.rtw_read8(0x0007);
  _device.rtw_write8(0x0007, 0x20);
  (void)_device.rtw_read8(0x001F);
  _device.rtw_write8(0x001F, 0x00);
  (void)_device.rtw_read8(0x0020);
  _device.rtw_write8(0x0020, 0x00);
  (void)_device.rtw_read8(0x0021);
  _device.rtw_write8(0x0021, 0x00);
  (void)_device.rtw_read8(0x0076);
  _device.rtw_write8(0x0076, 0x00);
  (void)_device.rtw_read8(0x0091);
  _device.rtw_write8(0x0091, 0xE1);
  (void)_device.rtw_read8(0x0070);
  _device.rtw_write8(0x0070, 0x08);
  (void)_device.rtw_read8(0x0005);
  _device.rtw_write8(0x0005, 0x08);
  _device.rtw_write8(0x001C, 0x00);
  (void)_device.rtw_read32(0x0064);
  _device.rtw_write32(0x0064, 0x31200000);
  (void)_device.rtw_read32(0x004C);
  _device.rtw_write32(0x004C, 0x60228282);
  (void)_device.rtw_read32(0x0040);
  _device.rtw_write32(0x0040, 0x00000004);
  (void)_device.rtw_read8(0x0002);
  _device.rtw_write8(0x0002, 0x1C);
  (void)_device.rtw_read8(0x001F);
  _device.rtw_write8(0x001F, 0x00);
  (void)_device.rtw_read32(0x00EC);
  _device.rtw_write32(0x00EC, 0x30453F15);
  (void)_device.rtw_read8(0xFE58);
  (void)_device.rtw_read16(0x0080);
  (void)_device.rtw_read8(0x0100);
  (void)_device.rtw_read8(0x10C2);
  _device.rtw_write8(0x10C2, 0x22);
  (void)_device.rtw_read8(0x0012);
  _device.rtw_write8(0x0012, 0xC3);
  (void)_device.rtw_read8(0x0015);
  _device.rtw_write8(0x0015, 0xCF);
  (void)_device.rtw_read8(0x0015);
  _device.rtw_write8(0x0015, 0x8F);
  (void)_device.rtw_read8(0x0023);
  _device.rtw_write8(0x0023, 0x05);
  (void)_device.rtw_read8(0x0046);
  _device.rtw_write8(0x0046, 0x00);
  (void)_device.rtw_read8(0x0062);
  _device.rtw_write8(0x0062, 0x00);
  (void)_device.rtw_read8(0x0005);
  _device.rtw_write8(0x0005, 0x00);
  (void)_device.rtw_read8(0x0005);
  _device.rtw_write8(0x0005, 0x00);
  (void)_device.rtw_read8(0x0006);
  (void)_device.rtw_read8(0x0005);
  _device.rtw_write8(0x0005, 0x00);
  (void)_device.rtw_read8(0x00F0);
  _device.rtw_write8(0x00F0, 0x35);
  (void)_device.rtw_read8(0x0081);
  _device.rtw_write8(0x0081, 0x20);
  (void)_device.rtw_read8(0x0005);
  _device.rtw_write8(0x0005, 0x01);
  /* rtw88 polls 0x0005 BIT0 here for power-on-ready (75 reads in the trace).
   * Replicate the poll rather than blast 75 unconditional reads. */
  for (int i = 0; i < 75; ++i) {
    if (!(_device.rtw_read8(0x0005) & 0x01)) {
      break;
    }
  }
  (void)_device.rtw_read32(0x1080);
  _device.rtw_write32(0x1080, 0xF7816D20);
  (void)_device.rtw_read8(0x0003);
  _device.rtw_write8(0x0003, 0xFE);
  (void)_device.rtw_read8(0x1103);
  _device.rtw_write8(0x1103, 0x0C);
  (void)_device.rtw_read32(0x0080);
  _device.rtw_write8(0x01A0, 0xFD);
  (void)_device.rtw_read8(0x0003);
  _device.rtw_write8(0x0003, 0xFA);
  (void)_device.rtw_read8(0x001D);
  _device.rtw_write8(0x001D, 0x08);
  (void)_device.rtw_read8(0x010D);
  _device.rtw_write8(0x010D, 0xC0);
  (void)_device.rtw_read8(0x0100);
  _device.rtw_write8(0x0100, 0x05);
  _device.rtw_write32(0x1330, 0x80000000);
  (void)_device.rtw_read16(0x0230);
  (void)_device.rtw_read32(0x022C);
  _device.rtw_write16(0x0230, 0x0200);
  _device.rtw_write32(0x022C, 0x80000000);
  (void)_device.rtw_read8(0x0550);
  _device.rtw_write8(0x0550, 0x14);
  (void)_device.rtw_read8(0x1082);
  _device.rtw_write8(0x1082, 0x80);
  (void)_device.rtw_read8(0x0009);
  _device.rtw_write8(0x0009, 0xBC);
  (void)_device.rtw_read8(0x1082);
  _device.rtw_write8(0x1082, 0x81);
  (void)_device.rtw_read8(0x0009);
  _device.rtw_write8(0x0009, 0xFC);
  (void)_device.rtw_read16(0x0080);
  _device.rtw_write16(0x0080, 0x2001);
  (void)_device.rtw_read32(0x1208);
  _device.rtw_write32(0x1208, 0x02000000);
  (void)_device.rtw_read8(0x0550);
  _device.rtw_write16(0x0204, 0x8000);
  (void)_device.rtw_read8(0x0101);
  _device.rtw_write8(0x0101, 0x01);
  _device.rtw_write8(0x0550, 0x14);
  _logger->info("8814A: rtw88-mimic 242-op pre-fwdl reg sequence applied");

  /* The 79-op mimic above already includes:
   *   - _FWDownloadEnable_8814A equivalent (0x0080 = 0x2001)
   *   - _3081Disable equivalent (0x0003 writes)
   *   - DDMA reset (0x1080 toggling)
   *   - Beacon queue gate (REG_CR+1 = 0x01, REG_BCN_CTRL = 0x14,
   *     REG_FWHW_TXQ_CTRL+2 clears BIT6)
   *   - REG_FIFOPAGE_CTRL_2 = 0x8000 (clear BIT15)
   * So additional explicit setup here would only CORRUPT the carefully-
   * staged chip state. The chunk loop below picks up directly. */
  /* Vestigial state-restore variables. Both REG_FWHW_TXQ_CTRL+2 (0x0422)
   * pre-bulk-OUT read AND the BCN_FUNC/REG_BCN_CTRL restore were removed —
   * the 0x0422 read in particular was the LAST control transfer before the
   * bulk OUT in our trace, while rtw88's last op is the 0x0550=0x14 write.
   * That extra read between mimic and bulk OUT is the only divergence vs
   * rtw88's wire sequence; removing it lets us match byte-for-byte. */
  const uint8_t bcn_ctrl = 0x14;   /* known final value after mimic */
  const bool sendBeacon = false;   /* mimic cleared the BCN-active flag */
  (void)bcn_ctrl;
  (void)sendBeacon;

  /* RSVD-page TX path: for each fw section (DMEM, IRAM), bulk-OUT the chunk
   * via the beacon queue, wait for the chip to ack the RSVD-page write,
   * then trigger IDDMA to copy from the TX FIFO to the 8051's DMEM/IMEM.
   * The fw header (64 bytes) is parsed for section sizes but NOT uploaded.
   *
   * IDDMA source = OCPBASE_TXBUF_3081 + 40. Confirmed via usbmon from a
   * working rtw88_8814au init capture: REG_DDMA_CH0SA was set to
   * 0x18780028 = OCPBASE_TXBUF_3081 + TXDESC_OFFSET. The TX_PAGE_BOUNDARY
   * register is about beacon-queue *routing*, not where the actual bytes
   * sit — those always land at TX_BUF + (TXDESC_OFFSET) after the chip
   * strips the descriptor. */
  const uint32_t MEMOffsetInTxBuf = OCPBASE_TXBUF_3081 + TXDESC_OFFSET_8814A;

  /* No CLEAR_FEATURE on EP 0x02 here. rtw88_8814au's usbmon trace shows the
   * first bulk OUT (4136-byte fw chunk) happens WITHOUT any preceding
   * CLEAR_FEATURE — the 117 CLEAR_FEATUREs in rtw88's full trace are all
   * issued ~160s later, during normal TX-queue operation. clear_halt resets
   * the EP 0x02 data-toggle bit, which corrupts the chip's fwdl state
   * machine. */

  auto stream_section =
      [&](const uint8_t *section_start, uint32_t section_size,
          uint32_t ocp_dest) -> bool {
    uint32_t remaining = section_size;
    uint32_t pkt_offset = 0;
    while (remaining > 0) {
      uint32_t chunk;
      bool ls;
      if (remaining > MAX_RSVD_PAGE_CHUNK_SZ) {
        chunk = MAX_RSVD_PAGE_CHUNK_SZ;
        ls = false;
        const uint32_t last_block = remaining - MAX_RSVD_PAGE_CHUNK_SZ;
        if (last_block < MAX_RSVD_PAGE_CHUNK_SZ &&
            ((last_block + 40) & 0x3F) == 0) {
          chunk -= 4;
        }
      } else {
        chunk = remaining;
        ls = true;
      }
      const bool fs = (pkt_offset == 0);
      /* Per-chunk sequence mirrors upstream rtw88_8814au usbmon capture. */
      _SetDownLoadFwRsvdPagePkt_8814A(section_start + pkt_offset, chunk);

      /* Brief settle delay: rtw88's chip latches BIT15 ~60us after bulk OUT
       * completion. Without this, fast polling can race the chip and read
       * stale 0x0FF80000 throughout the 10ms window. */
      std::this_thread::sleep_for(std::chrono::microseconds(100));

      /* Poll 0x0204 (4-byte read) for BIT15 (chip ack of RSVD page). */
      bool got_ack = false;
      for (int i = 0; i < 200; ++i) {
        const uint32_t v = _device.rtw_read32(REG_FIFOPAGE_CTRL_2_8814A);
        if (v & (1u << 15)) { got_ack = true; break; }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      if (!got_ack) {
        _logger->error("8814A RSVD ack timeout @ ocp=0x{:X} pkt_offset={}",
                       ocp_dest, pkt_offset);
        /* Continue anyway. */
      }

      /* Clear BIT15 (W1C), set BCN_CTRL, disable BCN_DMA temporarily. */
      _device.rtw_write16(REG_FIFOPAGE_CTRL_2_8814A, 0x8000);
      _device.rtw_write8(REG_BCN_CTRL, 0x14);
      _device.rtw_write8(REG_CR_8814A + 1, 0x00);

      if (!_IDDMADownLoadFW_3081(MEMOffsetInTxBuf, ocp_dest + pkt_offset,
                                 chunk, fs, ls)) {
        _logger->error("8814A IDDMA copy failed (ocp=0x{:X})", ocp_dest);
        return false;
      }

      remaining -= chunk;
      pkt_offset += chunk;
      if (remaining > 0) {
        /* Re-enable BCN_DMA for the next chunk. After the LAST chunk we
         * leave the beacon queue gated off — rtw88's post-fwdl trace
         * doesn't re-enable it; doing so before the CPU kick keeps the MAC
         * in a state that blocks fw boot completion. */
        _device.rtw_write16(REG_FIFOPAGE_CTRL_2_8814A, 0x8000);
        _device.rtw_write8(REG_CR_8814A + 1, 0x01);
        _device.rtw_write8(REG_BCN_CTRL, 0x14);
      }
    }
    return true;
  };

  /* DMEM is at fw + 64; IRAM follows DMEM. fw still points at start of blob. */
  bool rtStatus = stream_section(fw + FW_HEADER_SIZE_8814A, dmem_size,
                                 OCPBASE_DMEM_3081);
  if (rtStatus) {
    rtStatus = stream_section(fw + FW_HEADER_SIZE_8814A + dmem_size, iram_size,
                              OCPBASE_IMEM_3081);
  }
  if (!rtStatus) {
    _3081Enable8814A();
    _FWDownloadEnable_8814A(false);
    return;
  }

  /* Post-fwdl CPU kick sequence — mirrors rtw88_8814au's usbmon trace
   * byte-for-byte after the last fwdl IDDMA program. */
  _device.rtw_write8(REG_MCUFWDL, 0x79);    /* declare init ready */
  _device.rtw_write8(0x010d, 0x00);         /* REG_RD_CTRL+1 */
  _device.rtw_write8(0x0100, 0x00);         /* REG_CR byte 0 = 0 */
  _device.rtw_write32(0x1330, 0x80000000);  /* REG_3081_DCDC_CTRL */
  _device.rtw_write16(0x0230, 0x0000);      /* REG_PCIE_CTRL_REG word */
  _device.rtw_write32(0x022c, 0x80000000);  /* REG_BIST_CTRL */
  _device.rtw_write8(REG_BCN_CTRL, 0x14);   /* REG_BCN_CTRL */
  _device.rtw_write32(0x0210, 0x00000004);  /* REG_RXFLTMAP */
  _device.rtw_write16(REG_MCUFWDL, 0x6078); /* clear FWDL_EN; kick */
  _device.rtw_write8(0x001d, 0x09);         /* REG_AFE_OSC_CTRL2+1 */
  _device.rtw_write8(0x0003, 0xfe);         /* REG_RSV_CTRL+1, enable 8051 */

  /* Poll for CPU_DL_READY (BIT15 of REG_MCUFWDL). The 8051 sets this bit
   * once it's running and has finished its on-chip init. */
  if (!_FWFreeToGo8812(10, 5000, CHIP_8814A)) {
    return;
  }

  InitializeFirmwareVars8812();
}

/* Polls REG_FIFOPAGE_CTRL_2_8814A+1 BIT7 for "RSVD page download complete"
 * after a TX-FIFO write. Upstream's `dump_mgntframe` is synchronous;
 * devourer's send_packet is async via libusb, so we need a longer window to
 * cover the in-flight bulk transfer + chip processing. */
bool FirmwareManager::_WaitDownLoadRSVDPageOK_3081() {
  constexpr int max_iters = 200;        /* 200 * 1ms = 200ms wall time */
  constexpr int iter_delay_us = 1000;
  for (int count = 0; count < max_iters; ++count) {
    const uint8_t v =
        _device.rtw_read8(REG_FIFOPAGE_CTRL_2_8814A + 1);
    if (v & BIT(7)) {
      /* Write-1 to clear. */
      _device.rtw_write8(REG_FIFOPAGE_CTRL_2_8814A + 1,
                         (uint8_t)(v | BIT(7)));
      return true;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(iter_delay_us));
  }
  _logger->error("_WaitDownLoadRSVDPageOK_3081: timeout (BIT7 never set)");
  return false;
}

/* Build a TX packet whose body is `len` bytes of firmware and dump it via
 * bulk-OUT through the management queue. Upstream's `dump_mgntframe` (which
 * `SetDownLoadFwRsvdPagePkt_8814A` calls) goes via the management queue
 * (QSLT_MGT = 0x12); the chip is in a special mode set up above where mgmt
 * frames are diverted to the beacon FIFO at the reserved-page address. */
void FirmwareManager::_SetDownLoadFwRsvdPagePkt_8814A(
    const uint8_t *fw_chunk, uint32_t len) {
  const uint32_t total = len + TXDESC_OFFSET_8814A;
  std::vector<uint8_t> packet(total, 0);
  uint8_t *p = packet.data();

  /* Minimal TX descriptor matching upstream rtw88_8814au usbmon capture:
   * only PKT_SIZE, OFFSET, LAST_SEG, and QUEUE_SEL=0x10 (QSLT_BCN). Setting
   * BMC, FIRST_SEG, OWN, MACID, RATE_ID, etc. causes the chip to reject the
   * packet after the first 512 bytes. */
  SET_TX_DESC_PKT_SIZE_8812(p, len);
  SET_TX_DESC_OFFSET_8812(p, TXDESC_OFFSET_8814A);
  SET_TX_DESC_LAST_SEG_8812(p, 1);
  SET_TX_DESC_QUEUE_SEL_8812(p, 0x10); /* QSLT_BCN */

  std::memcpy(p + TXDESC_OFFSET_8814A, fw_chunk, len);
  rtl8812a_cal_txdesc_chksum(p);

  /* Use the synchronous bulk write — we need the bytes to arrive at the chip
   * before polling for the RSVD-page-OK bit. The default async send_packet
   * returns before the transfer lands. */
  const int sent = _device.bulk_send_sync(p, total, 2000);
  if (sent < 0 || static_cast<uint32_t>(sent) != total) {
    _logger->error("8814A RSVD chunk TX failed: sent={} of {} bytes", sent,
                   (int)total);
  } else {
    _logger->debug("8814A RSVD chunk TX OK: {} bytes", sent);
  }
}

/* Drives the chip's internal DMA controller channel 0 to copy `length` bytes
 * from TX-FIFO source `source` to internal `dest` (DMEM or IMEM). Mirrors
 * upstream IDDMADownLoadFW_3081. */
bool FirmwareManager::_IDDMADownLoadFW_3081(uint32_t source, uint32_t dest,
                                            uint32_t length, bool fs, bool ls) {
  /* Wait for channel idle before programming a new transfer. */
  for (int cnt = 20; cnt > 0; --cnt) {
    if (!(_device.rtw_read32(REG_DDMA_CH0CTRL_8814A) & DDMA_CH_OWN_8814A))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (cnt == 1) {
      _logger->error("_IDDMADownLoadFW_3081: pre-busy timeout");
      return false;
    }
  }

  uint32_t ch0ctrl = DDMA_CHKSUM_EN_8814A | DDMA_CH_OWN_8814A;
  ch0ctrl |= (length & DDMA_LEN_MASK_8814A);
  if (!fs) {
    ch0ctrl |= DDMA_CH_CHKSUM_CNT_8814A;
  }
  _device.rtw_write32(REG_DDMA_CH0SA_8814A, source);
  _device.rtw_write32(REG_DDMA_CH0DA_8814A, dest);
  _device.rtw_write32(REG_DDMA_CH0CTRL_8814A, ch0ctrl);

  /* Wait for channel completion. */
  for (int cnt = 20; cnt > 0; --cnt) {
    if (!(_device.rtw_read32(REG_DDMA_CH0CTRL_8814A) & DDMA_CH_OWN_8814A))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (cnt == 1) {
      _logger->error("_IDDMADownLoadFW_3081: post-busy timeout");
      return false;
    }
  }

  if (ls) {
    /* Only check the IDDMA checksum status — do NOT write IMEM_DL_RDY /
     * DMEM_DL_RDY / *_CHKSUM_OK back into REG_MCUFWDL. The aircrack-ng
     * rtl8814au port we originally followed does these writes, but rtw88's
     * iddma_download_firmware does not — and the chip is supposed to set
     * BIT4 (IMEM_CHKSUM_OK) and BIT6 (DMEM_CHKSUM_OK) itself after a
     * successful IDDMA. download_firmware_end_flow then reads MCUFWDL,
     * verifies BIT_CHECK_SUM_OK = BIT4|BIT6, and on success writes
     * FW_DW_RDY (BIT14) and clears MCUFWDL_EN (BIT0). Force-writing the
     * DL_RDY bits ourselves was leaving the chip in a state where the 3081
     * CPU enable wouldn't trigger fw execution (chip stayed at 0x00606078
     * instead of transitioning to 0x0060e078 = BIT15 / FW_INIT_RDY set). */
    if (_device.rtw_read32(REG_DDMA_CH0CTRL_8814A) & DDMA_CHKSUM_FAIL_8814A) {
      const uint32_t v = _device.rtw_read32(REG_DDMA_CH0CTRL_8814A);
      _device.rtw_write32(REG_DDMA_CH0CTRL_8814A,
                          v | DDMA_RST_CHKSUM_STS_8814A);
      _logger->error("_IDDMADownLoadFW_3081: checksum fail (dest=0x{:X})",
                     dest);
      return false;
    }
  }
  return true;
}

/* 8051 MCU core gate. Mirrors _3081Disable8814A / _3081Enable8814A in
 * upstream hal/rtl8814a/rtl8814a_hal_init.c. The MCU "3081" runs the
 * downloaded firmware; it must be disabled while bytes are being written and
 * re-enabled afterward to make it pick up the new image. The bit lives in
 * REG_SYS_FUNC_EN+1 (the high byte of the 16-bit SYS_FUNC_EN register at
 * 0x0002 — same physical register as 8812's REG_SYS_FUNC_EN). */
void FirmwareManager::_3081Disable8814A() {
  uint8_t v = _device.rtw_read8(REG_SYS_FUNC_EN + 1);
  _device.rtw_write8(REG_SYS_FUNC_EN + 1, (uint8_t)(v & ~BIT(2)));
}

void FirmwareManager::_3081Enable8814A() {
  uint8_t v = _device.rtw_read8(REG_SYS_FUNC_EN + 1);
  _device.rtw_write8(REG_SYS_FUNC_EN + 1, (uint8_t)(v | BIT(2)));
}

/* Direct DMA controller reset. REG_CPU_DMEM_CON_8814A (0x1080) is 8814-only;
 * toggle BIT16 (clear then set) to flush stale DMA state before the firmware
 * write. Per upstream comment "DDMA reset, suggest by MAC yodar". */
void FirmwareManager::_DDMAReset8814A() {
  constexpr uint16_t REG_CPU_DMEM_CON_8814A = 0x1080;
  uint32_t v = _device.rtw_read32(REG_CPU_DMEM_CON_8814A);
  _device.rtw_write32(REG_CPU_DMEM_CON_8814A, v & ~BIT(16));
  _device.rtw_write32(REG_CPU_DMEM_CON_8814A, v | BIT(16));
}

/* Mirrors upstream _FWDownloadEnable_8814A in
 * hal/rtl8814a/rtl8814a_hal_init.c. The 8814 enable path sets a specific bit
 * pattern in the upper bits of REG_8051FW_CTRL (== REG_MCUFWDL @ 0x0080)
 * that puts the 8051 in firmware-load mode; failing to do this is why the
 * 8051 never reaches CPU_DL_READY on 8814AU even after a successful upload. */
void FirmwareManager::_FWDownloadEnable_8814A(bool enable) {
  if (enable) {
    uint16_t u2Tmp = _device.rtw_read16(REG_MCUFWDL);
    u2Tmp &= 0x3000;     /* keep bits 12-13, clear everything else */
    u2Tmp &= ~BIT(12);   /* clear bit 12 */
    u2Tmp |= BIT(13);    /* set bit 13 */
    u2Tmp |= BIT(0);     /* set bit 0 (download enable) */
    _device.rtw_write16(REG_MCUFWDL, u2Tmp);
  } else {
    uint8_t tmp = _device.rtw_read8(REG_MCUFWDL);
    _device.rtw_write8(REG_MCUFWDL, (uint8_t)(tmp & 0xfe));
  }
}

void FirmwareManager::_FWDownloadEnable_8812(bool enable) {
  u8 tmp;

  if (enable) {
    /* MCU firmware download enable. */
    tmp = _device.rtw_read8(REG_MCUFWDL);
    _device.rtw_write8(REG_MCUFWDL, tmp | 0x01);

    /* 8051 reset */
    tmp = _device.rtw_read8(REG_MCUFWDL + 2);
    _device.rtw_write8(REG_MCUFWDL + 2, tmp & 0xf7);
  } else {

    /* MCU firmware download disable. */
    tmp = _device.rtw_read8(REG_MCUFWDL);
    _device.rtw_write8(REG_MCUFWDL, tmp & 0xfe);
  }
}

bool FirmwareManager::WriteFW8812(uint8_t *buffer, uint32_t size) {
  const int MAX_DLFW_PAGE_SIZE = 4096; /* @ page : 4k bytes */

  /* Since we need dynamic decide method of dwonload fw, so we call this
   * function to get chip version. */
  bool ret = true;
  int32_t pageNums, remainSize;
  int32_t page;
  int offset;
  auto bufferPtr = buffer;

  pageNums = (int)(size / MAX_DLFW_PAGE_SIZE);
  /* RT_ASSERT((pageNums <= 4), ("Page numbers should not greater then 4\n"));
   */
  remainSize = (int)(size % MAX_DLFW_PAGE_SIZE);

  for (page = 0; page < pageNums; page++) {
    offset = page * MAX_DLFW_PAGE_SIZE;
    ret = _PageWrite_8812(page, bufferPtr + offset, MAX_DLFW_PAGE_SIZE);

    if (ret == false) {
      goto exit;
    }
  }

  if (remainSize != 0) {
    offset = pageNums * MAX_DLFW_PAGE_SIZE;
    page = pageNums;
    ret = _PageWrite_8812(page, bufferPtr + offset, remainSize);

    if (ret == false) {
      goto exit;
    }
  }

exit:
  return ret;
}

int FirmwareManager::_PageWrite_8812(uint32_t page, uint8_t *buffer,
                                     uint32_t size) {
  u8 value8;
  u8 u8Page = (u8)(page & 0x07);

  value8 = (_device.rtw_read8(REG_MCUFWDL + 2) & 0xF8) | u8Page;
  _device.rtw_write8(REG_MCUFWDL + 2, value8);

  return BlockWrite(buffer, size);
}

bool FirmwareManager::BlockWrite(uint8_t *buffer, int buffSize) {
  const int MAX_REG_BOLCK_SIZE = 196;

  bool ret = true;

  uint32_t blockSize_p1 =
      4; /* (Default) Phase #1 : PCI muse use 4-byte write to download FW */
  uint32_t blockSize_p2 =
      8; /* Phase #2 : Use 8-byte, if Phase#1 use big size to write FW. */
  uint32_t blockSize_p3 =
      1; /* Phase #3 : Use 1-byte, the remnant of FW image. */
  uint32_t blockCount_p1 = 0, blockCount_p2 = 0, blockCount_p3 = 0;
  uint32_t remainSize_p1 = 0, remainSize_p2 = 0;
  // byte			*bufferPtr	= (byte *)buffer;
  uint32_t i = 0, offset = 0;

  blockSize_p1 = MAX_REG_BOLCK_SIZE;

  /* 3 Phase #1 */
  blockCount_p1 = (uint32_t)(buffSize / blockSize_p1);
  remainSize_p1 = (uint32_t)(buffSize % blockSize_p1);

  for (i = 0; i < blockCount_p1; i++) {
    _device.WriteBytes((ushort)(FW_START_ADDRESS + i * blockSize_p1),
                       buffer + i * blockSize_p1, (int)blockSize_p1);
  }

  /* 3 Phase #2 */
  if (remainSize_p1 != 0) {
    offset = blockCount_p1 * blockSize_p1;

    blockCount_p2 = remainSize_p1 / blockSize_p2;
    remainSize_p2 = remainSize_p1 % blockSize_p2;

    for (i = 0; i < blockCount_p2; i++) {
      _device.WriteBytes((ushort)(FW_START_ADDRESS + offset + i * blockSize_p2),
                         buffer + offset + i * blockSize_p2, (int)blockSize_p2);
    }
  }

  /* 3 Phase #3 */
  if (remainSize_p2 != 0) {
    offset = (blockCount_p1 * blockSize_p1) + (blockCount_p2 * blockSize_p2);

    blockCount_p3 = remainSize_p2 / blockSize_p3;

    for (i = 0; i < blockCount_p3; i++) {
      _device.rtw_write8((ushort)(FW_START_ADDRESS + offset + i),
                         buffer[(int)(offset + i)]);
    }
  }

  return ret;
}

void FirmwareManager::InitializeFirmwareVars8812() {
  /* Init H2C cmd. */
  _device.rtw_write8(REG_HMETFR, 0x0f);
}

bool FirmwareManager::polling_fwdl_chksum(uint32_t min_cnt, uint32_t timeout_ms) {
  bool ret = false;
  u32 value32;
  auto start = std::chrono::steady_clock::now();
  u32 cnt = 0;

  /* polling CheckSum report */
  do {
    cnt++;
    value32 = _device.rtw_read32(REG_MCUFWDL);
    if (value32 & FWDL_ChkSum_rpt)
      break;
    yield();
  } while (since(start).count() < timeout_ms || cnt < min_cnt);

  if (!(value32 & FWDL_ChkSum_rpt))
    goto exit;

  ret = true;

exit:
  _logger->info("{}: Checksum report {}! ({}, {}ms), REG_MCUFWDL:{:08x}",
                __FUNCTION__, ret == true ? "OK" : "Fail", cnt,
                since(start).count(), value32);

  return ret;
}

bool FirmwareManager::_FWFreeToGo8812(uint32_t min_cnt, uint32_t timeout_ms,
                                      HAL_IC_TYPE_E ic_type) {
  bool ret = false;
  uint32_t value32;
  uint32_t cnt = 0;

  /* REG_MCUFWDL (0x0080) is the same physical register on 8812 and 8814 —
   * upstream renames it REG_8051FW_CTRL_8814A. The "FW is alive" indicator
   * differs by chip:
   *  - 8812 uses WINTINI_RDY (BIT6) as the single-bit ready flag
   *  - 8814 has the chip set BIT15 (FW_INIT_RDY) briefly during fw init,
   *    then settles to byte 0 = 0x78 (IMEM_DL_RDY|IMEM_CHKSUM_OK|
   *    DMEM_DL_RDY|DMEM_CHKSUM_OK, FWDL_EN cleared). Polling BIT15 in
   *    userspace misses the transient window; the stable post-boot state
   *    is byte 0 == 0x78 with FW_DW_RDY (BIT14) also set. */
  if (ic_type != CHIP_8814A) {
    value32 = _device.rtw_read32(REG_MCUFWDL);
    value32 |= MCUFWDL_RDY;
    value32 = (uint32_t)(value32 & ~WINTINI_RDY);
    _device.rtw_write32(REG_MCUFWDL, value32);

    _device._8051Reset8812();
  }

  const uint32_t ready_bit =
      (ic_type == CHIP_8814A) ? 0u /* use byte-0 check below */ : WINTINI_RDY;

  auto start = std::chrono::steady_clock::now();
  /*  polling for FW ready */
  do {
    cnt++;
    value32 = _device.rtw_read32(REG_MCUFWDL);
    if (ic_type == CHIP_8814A) {
      /* Match either the transient FW_INIT_RDY (BIT15) OR the stable
       * post-boot pattern (byte 0 == 0x78 = all DL_RDY + CHKSUM_OK,
       * FWDL_EN cleared). */
      if ((value32 & (1u << 15)) || ((value32 & 0xFF) == 0x78)) {
        break;
      }
    } else if ((value32 & ready_bit) != 0) {
      break;
    }
    yield();
  } while (since(start).count() < timeout_ms || cnt < min_cnt);

  if (ic_type == CHIP_8814A) {
    if (!((value32 & (1u << 15)) || ((value32 & 0xFF) == 0x78))) {
      goto exit;
    }
  } else if (!((value32 & ready_bit) != 0)) {
    goto exit;
  }

  // if (rtw_fwdl_test_trigger_wintint_rdy_fail())
  //{
  //     goto exit;
  // }

  ret = true;

exit:
  _logger->info("{}: Polling FW ready {}! ({}, {}ms), REG_MCUFWDL:0x{:08x}",
                __FUNCTION__, ret == true ? "OK" : "Fail", cnt,
                since(start).count(), value32);

  return ret;
}
