#include "HalmacJaguar2Fw.h"

#include <array>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "FrameParserJaguar2.h"
#include "HalmacJaguar2Regs.h"
#if defined(DEVOURER_HAVE_JAGUAR2_8822B)
#include "hal8822b_fw.h" /* array_mp_8822b_fw_nic[] + _len */
#endif
#if defined(DEVOURER_HAVE_JAGUAR2_8821C)
#include "hal8821c_fw.h" /* array_mp_8821c_fw_nic[] + _len */
#endif

using namespace jaguar2::halmac;

namespace jaguar2 {

namespace {
uint32_t le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
void delay_us(uint32_t us) {
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}
} /* namespace */

HalmacJaguar2Fw::HalmacJaguar2Fw(RtlAdapter device, Logger_t logger,
                                 ChipVariant variant)
    : _device{device}, _logger{std::move(logger)}, _variant{variant} {}

bool HalmacJaguar2Fw::download_default_firmware() {
  /* Select the bundled NIC image for this chip. The HalMAC DLFW state machine
   * (download_firmware) is generation-shared; only the blob differs. */
  const uint8_t *fw = nullptr;
  size_t fw_len = 0;
#if defined(DEVOURER_HAVE_JAGUAR2_8821C)
  if (_variant == ChipVariant::C8821C) {
    fw = array_mp_8821c_fw_nic;
    fw_len = array_mp_8821c_fw_nic_len;
  }
#endif
#if defined(DEVOURER_HAVE_JAGUAR2_8822B)
  if (_variant == ChipVariant::C8822B) {
    fw = array_mp_8822b_fw_nic;
    fw_len = array_mp_8822b_fw_nic_len;
  }
#endif
  if (fw == nullptr) {
    _logger->error("HalmacJaguar2Fw: no firmware blob compiled for this variant");
    return false;
  }

  /* A cheap in-place CPU-reset retry catches easy transients (download_firmware
   * re-disables + platform-resets the WLAN CPU on entry). Deeper warm-state
   * hangs need a full power cycle, which the bring_up caller does around this. */
  constexpr int kMaxTries = 2;
  for (int t = 0; t < kMaxTries; t++) {
    if (download_firmware(fw, fw_len))
      return true;
    if (t + 1 < kMaxTries)
      _logger->error("Jaguar2 DLFW: cpu-reset retry {}/{}", t + 1, kMaxTries);
  }
  return false;
}

uint8_t HalmacJaguar2Fw::r8(uint16_t reg) { return _device.rtw_read8(reg); }
uint16_t HalmacJaguar2Fw::r16(uint16_t reg) { return _device.rtw_read16(reg); }
uint32_t HalmacJaguar2Fw::r32(uint16_t reg) { return _device.rtw_read32(reg); }
void HalmacJaguar2Fw::w8(uint16_t reg, uint8_t v) { _device.rtw_write8(reg, v); }
void HalmacJaguar2Fw::w16(uint16_t reg, uint16_t v) { _device.rtw_write16(reg, v); }
void HalmacJaguar2Fw::w32(uint16_t reg, uint32_t v) { _device.rtw_write32(reg, v); }
void HalmacJaguar2Fw::w32_set(uint16_t reg, uint32_t bits) {
  w32(reg, r32(reg) | bits);
}

bool HalmacJaguar2Fw::chk_fw_size(const uint8_t *fw_bin, size_t size) {
  if (size < WLAN_FW_HDR_SIZE) {
    _logger->error("Jaguar2 DLFW: fw size < header");
    return false;
  }
  uint32_t dmem = le32(fw_bin + WLAN_FW_HDR_DMEM_SIZE);
  uint32_t imem = le32(fw_bin + WLAN_FW_HDR_IMEM_SIZE);
  uint32_t emem = 0;
  if ((fw_bin[WLAN_FW_HDR_MEM_USAGE] & (1u << 4)) != 0)
    emem = le32(fw_bin + WLAN_FW_HDR_EMEM_SIZE);
  dmem += WLAN_FW_HDR_CHKSUM_SIZE;
  imem += WLAN_FW_HDR_CHKSUM_SIZE;
  if (emem != 0)
    emem += WLAN_FW_HDR_CHKSUM_SIZE;
  uint32_t real = WLAN_FW_HDR_SIZE + dmem + imem + emem;
  if (size != real) {
    _logger->error("Jaguar2 DLFW: size {} != computed {}", size, real);
    return false;
  }
  return true;
}

void HalmacJaguar2Fw::wlan_cpu_en(bool enable) {
  if (enable) {
    w8(REG_RSV_CTRL + 1, static_cast<uint8_t>(r8(REG_RSV_CTRL + 1) | 0x1));
    w8(REG_SYS_FUNC_EN + 1,
       static_cast<uint8_t>(r8(REG_SYS_FUNC_EN + 1) | (1u << 2)));
  } else {
    w8(REG_SYS_FUNC_EN + 1,
       static_cast<uint8_t>(r8(REG_SYS_FUNC_EN + 1) & ~(1u << 2)));
    w8(REG_RSV_CTRL + 1, static_cast<uint8_t>(r8(REG_RSV_CTRL + 1) & ~0x1));
  }
}

void HalmacJaguar2Fw::pltfm_reset() {
  /* Mirrors pltfm_reset_88xx (halmac_fw_88xx.c). The SYS_CLK_CTRL+1 BIT6
   * toggle bracketing the CPU_DMEM_CON reset is the 8822B/8821C clock-sync
   * fix — without it the download-CPU clock is not resynced and the rsvd-page
   * bcn-valid latch never asserts (DLFW hangs at the first chunk). */
  w8(REG_CPU_DMEM_CON + 2,
     static_cast<uint8_t>(r8(REG_CPU_DMEM_CON + 2) & ~0x1));
  w8(REG_SYS_CLK_CTRL + 1,
     static_cast<uint8_t>(r8(REG_SYS_CLK_CTRL + 1) & ~(1u << 6)));
  w8(REG_CPU_DMEM_CON + 2,
     static_cast<uint8_t>(r8(REG_CPU_DMEM_CON + 2) | 0x1));
  w8(REG_SYS_CLK_CTRL + 1,
     static_cast<uint8_t>(r8(REG_SYS_CLK_CTRL + 1) | (1u << 6)));
}

/* WL2LTECOEX indirect register access (ltecoex_reg_read/write_88xx,
 * halmac_common_88xx.c). Ctrl 0x1700, write-data 0x1704, read-data 0x1708; the
 * ready handshake is CTRL+3 BIT5. The DLFW flow reads offset 0x38 up front and
 * restores it at the end — the read's ready-poll also synchronises the LTE/BT
 * coex subsystem (which on this combo shares the WLAN CPU) before the CPU is
 * reset and re-booted, without which the FW-boot handshake (0x80=0xC078)
 * intermittently hangs. */
bool HalmacJaguar2Fw::ltecoex_read(uint16_t offset, uint32_t &val) {
  uint32_t cnt = 10000;
  while ((r8(0x1700 + 3) & (1u << 5)) == 0) {
    if (cnt-- == 0) {
      _logger->error("Jaguar2 DLFW: ltecoex not ready (R)");
      return false;
    }
    delay_us(50);
  }
  w32(0x1700, 0x800F0000u | offset);
  val = r32(0x1708);
  return true;
}

bool HalmacJaguar2Fw::ltecoex_write(uint16_t offset, uint32_t val) {
  uint32_t cnt = 10000;
  while ((r8(0x1700 + 3) & (1u << 5)) == 0) {
    if (cnt-- == 0) {
      _logger->error("Jaguar2 DLFW: ltecoex not ready (W)");
      return false;
    }
    delay_us(50);
  }
  w32(0x1704, val);
  w32(0x1700, 0xC00F0000u | offset);
  return true;
}

bool HalmacJaguar2Fw::iddma_en(uint32_t src, uint32_t dest, uint32_t ctrl) {
  uint32_t cnt = HALMC_DDMA_POLLING_COUNT;
  w32(REG_DDMA_CH0SA, src);
  w32(REG_DDMA_CH0DA, dest);
  w32(REG_DDMA_CH0CTRL, ctrl);
  while (r32(REG_DDMA_CH0CTRL) & BIT_DDMACH0_OWN) {
    if (--cnt == 0)
      return false;
  }
  return true;
}

bool HalmacJaguar2Fw::iddma_dlfw(uint32_t src, uint32_t dest, uint32_t len,
                                 bool first) {
  uint32_t cnt = HALMC_DDMA_POLLING_COUNT;
  uint32_t ch0_ctrl = BIT_DDMACH0_CHKSUM_EN | BIT_DDMACH0_OWN;
  while (r32(REG_DDMA_CH0CTRL) & BIT_DDMACH0_OWN) {
    if (--cnt == 0) {
      _logger->error("Jaguar2 DLFW: ch0 not ready (CH0CTRL=0x{:08x} "
                     "SA=0x{:08x} DA=0x{:08x})",
                     r32(REG_DDMA_CH0CTRL), r32(REG_DDMA_CH0SA),
                     r32(REG_DDMA_CH0DA));
      return false;
    }
  }
  ch0_ctrl |= (len & BIT_MASK_DDMACH0_DLEN);
  if (!first)
    ch0_ctrl |= BIT_DDMACH0_CHKSUM_CONT;
  if (!iddma_en(src, dest, ch0_ctrl)) {
    _logger->error("Jaguar2 DLFW: iddma_en failed");
    return false;
  }
  return true;
}

bool HalmacJaguar2Fw::check_fw_chksum(uint32_t mem_addr) {
  uint8_t fw_ctrl = r8(REG_MCUFW_CTRL);
  if (r32(REG_DDMA_CH0CTRL) & BIT_DDMACH0_CHKSUM_STS) {
    if (mem_addr < OCPBASE_DMEM_88XX) {
      fw_ctrl |= BIT_IMEM_DW_OK;
      fw_ctrl &= ~BIT_IMEM_CHKSUM_OK;
    } else {
      fw_ctrl |= BIT_DMEM_DW_OK;
      fw_ctrl &= ~BIT_DMEM_CHKSUM_OK;
    }
    w8(REG_MCUFW_CTRL, fw_ctrl);
    _logger->error("Jaguar2 DLFW: fw checksum fail");
    return false;
  }
  if (mem_addr < OCPBASE_DMEM_88XX)
    fw_ctrl |= (BIT_IMEM_DW_OK | BIT_IMEM_CHKSUM_OK);
  else
    fw_ctrl |= (BIT_DMEM_DW_OK | BIT_DMEM_CHKSUM_OK);
  w8(REG_MCUFW_CTRL, fw_ctrl);
  return true;
}

bool HalmacJaguar2Fw::send_fw_page(uint16_t pg_addr, const uint8_t *chunk,
                                   uint32_t size) {
  if (size == 0)
    return false;

  w16(REG_FIFOPAGE_CTRL_2,
      static_cast<uint16_t>((pg_addr & BIT_MASK_BCN_HEAD_1_V1) | (1u << 15)));

  uint8_t cr1 = r8(REG_CR + 1);
  w8(REG_CR + 1, static_cast<uint8_t>(cr1 | 0x1));
  uint8_t txq2 = r8(REG_FWHW_TXQ_CTRL + 2);
  w8(REG_FWHW_TXQ_CTRL + 2, static_cast<uint8_t>(txq2 & ~(1u << 6)));

  /* Minimal rsvd-page TX descriptor, matching the vendor rtl88x2bu DLFW golden
   * (and the in-tree rtw88_8822bu) exactly: TXPKTSIZE + OFFSET + QSEL_BEACON.
   * The rtl88x2cu path (jaguar3) additionally sets USE_RATE/DATARATE/DISDATAFB/
   * LS; those are harmless there but are not part of the 8822B download desc. */
  /* Packet-offset padding, ported verbatim from usb_write_data_not_xmitframe
   * (rtl8822bu_halmac.c): when (desc + payload) is an exact multiple of the USB
   * bulk max-packet size (512 covers both HS 512 and SS 1024), insert
   * PACKET_OFFSET_SZ (8) bytes BETWEEN the descriptor and the payload and mark
   * them in the descriptor (OFFSET = desc + 8, PKT_OFFSET = 1). This both breaks
   * the exact-multiple (so a short packet terminates the bulk) AND keeps the
   * chip's beacon-FIFO write pointer aligned — a trailing pad byte the chip is
   * not told about (via PKT_OFFSET) misaligns the FIFO by one byte, corrupting
   * the descriptor of the *next* segment's chunk so its bcn-valid never latches. */
  constexpr uint32_t PACKET_OFFSET_SZ = 8;
  const uint32_t desclen = TXDESC_SIZE_8822B;
  uint32_t len = desclen + size;
  /* The 512-multiple pad exists to break USB bulk max-packet alignment; on the
   * PCIe BCN-ring path there is no such framing (rtw88 never pads) — gate it. */
  const bool add_pkt_offset = _device.is_usb() && ((len % 512u) == 0);
  if (add_pkt_offset)
    len += PACKET_OFFSET_SZ;
  /* Carry the packet-offset to the iddma: the beacon download writes
   * desc + [pkt_offset] + payload into the TX FIFO, so the iddma source (which
   * skips the descriptor) must also skip the pkt_offset to copy the true
   * payload — otherwise the DDMA checksum sees the pad bytes and fails. */
  _last_pkt_offset = add_pkt_offset ? PACKET_OFFSET_SZ : 0;
  std::vector<uint8_t> frame(len, 0);
  uint8_t *d = frame.data();
  SET_TX_DESC_TXPKTSIZE_8822B(d, size);
  if (add_pkt_offset) {
    std::memcpy(d + desclen + PACKET_OFFSET_SZ, chunk, size);
    SET_TX_DESC_OFFSET_8822B(d, desclen + PACKET_OFFSET_SZ);
    SET_TX_DESC_PKT_OFFSET_8822B(d, 1);
  } else {
    std::memcpy(d + desclen, chunk, size);
    SET_TX_DESC_OFFSET_8822B(d, desclen);
  }
  SET_TX_DESC_QSEL_8822B(d, QSEL_BEACON);
  cal_txdesc_chksum_8822b(d);

  /* Submit the rsvd-page bulk. The vendor (usb_write_data_not_xmitframe ->
   * usb_submit_urb) treats the bcn-valid latch — not the bulk completion — as
   * the success criterion. With libusb sync transfers the chip delivers all the
   * data but does not always signal bulk completion the way libusb expects,
   * returning a timeout with the full byte count transferred; that is fine, so
   * long as every byte went out we proceed to poll bcn-valid. */
  int got = _device.bulk_send_sync_ep(_device.first_bulk_out_ep(), frame.data(),
                                      static_cast<int>(frame.size()), 1000);
  bool sent = (got == static_cast<int>(frame.size()));
  bool status = sent;

  if (sent) {
    uint32_t cnt = 1000;
    while ((r8(REG_FIFOPAGE_CTRL_2 + 1) & (1u << 7)) == 0) {
      if (--cnt == 0) {
        _logger->error("Jaguar2 DLFW: rsvd-page (bcn valid) poll failed "
                       "(pg={} size={})",
                       pg_addr, size);
        status = false;
        break;
      }
      delay_us(10);
    }
  } else {
    _logger->error("Jaguar2 DLFW: rsvd-page bulk-OUT failed");
  }

  w16(REG_FIFOPAGE_CTRL_2, static_cast<uint16_t>(_rsvd_boundary | (1u << 15)));
  w8(REG_FWHW_TXQ_CTRL + 2, txq2);
  w8(REG_CR + 1, cr1);
  return status;
}

bool HalmacJaguar2Fw::dlfw_to_mem(const uint8_t *fw_bin, uint32_t src,
                                  uint32_t dest, uint32_t size) {
  uint32_t mem_offset = 0;
  bool first = true;
  uint32_t residue = size;

  /* W32_SET (read-modify-write), NOT a plain write: the reference
   * dlfw_to_mem_88xx uses HALMAC_REG_W32_SET here. A plain write clobbers the
   * residual DDMA CH0CTRL bits left by the previous segment's last iddma, which
   * on the second (IMEM) segment wedges the rsvd-page engine so its bcn-valid
   * latch never asserts. */
  w32_set(REG_DDMA_CH0CTRL, BIT_DDMACH0_RESET_CHKSUM_STS);

  while (residue != 0) {
    uint32_t pkt = (residue >= _dlfw_pkt_size) ? _dlfw_pkt_size : residue;
    if (!send_fw_page(static_cast<uint16_t>(src >> 7), fw_bin + mem_offset, pkt))
      return false;
    if (!iddma_dlfw(OCPBASE_TXBUF_88XX + src + TX_DESC_SIZE_88XX +
                        _last_pkt_offset,
                    dest + mem_offset, pkt, first))
      return false;
    first = false;
    mem_offset += pkt;
    residue -= pkt;
  }
  return check_fw_chksum(dest);
}

bool HalmacJaguar2Fw::start_dlfw(const uint8_t *fw_bin, size_t /*size*/) {
  uint32_t dmem = le32(fw_bin + WLAN_FW_HDR_DMEM_SIZE) + WLAN_FW_HDR_CHKSUM_SIZE;
  uint32_t imem = le32(fw_bin + WLAN_FW_HDR_IMEM_SIZE) + WLAN_FW_HDR_CHKSUM_SIZE;
  uint32_t emem = 0;
  if ((fw_bin[WLAN_FW_HDR_MEM_USAGE] & (1u << 4)) != 0)
    emem = le32(fw_bin + WLAN_FW_HDR_EMEM_SIZE) + WLAN_FW_HDR_CHKSUM_SIZE;

  uint16_t v16 = static_cast<uint16_t>((r16(REG_MCUFW_CTRL) & 0x3800) | 0x1);
  w16(REG_MCUFW_CTRL, v16);

  const uint8_t *cur = fw_bin + WLAN_FW_HDR_SIZE;
  uint32_t addr = le32(fw_bin + WLAN_FW_HDR_DMEM_ADDR) & ~(1u << 31);
  if (!dlfw_to_mem(cur, 0, addr, dmem))
    return false;

  cur = fw_bin + WLAN_FW_HDR_SIZE + dmem;
  addr = le32(fw_bin + WLAN_FW_HDR_IMEM_ADDR) & ~(1u << 31);
  if (!dlfw_to_mem(cur, 0, addr, imem))
    return false;

  if (emem != 0) {
    cur = fw_bin + WLAN_FW_HDR_SIZE + dmem + imem;
    addr = le32(fw_bin + WLAN_FW_HDR_EMEM_ADDR) & ~(1u << 31);
    if (!dlfw_to_mem(cur, 0, addr, emem))
      return false;
  }
  return true;
}

bool HalmacJaguar2Fw::dlfw_end_flow() {
  w32(REG_TXDMA_STATUS, 1u << 2);

  uint16_t fw_ctrl = r16(REG_MCUFW_CTRL);
  if ((fw_ctrl & 0x50) != 0x50) {
    _logger->error("Jaguar2 DLFW: IMEM/DMEM checksum not ready (0x{:04x})",
                   fw_ctrl);
    return false;
  }
  _boot.checksum_ok = true;
  w16(REG_MCUFW_CTRL,
      static_cast<uint16_t>((fw_ctrl | BIT_FW_DW_RDY) & ~0x1));

  wlan_cpu_en(true);
  _logger->info("Jaguar2 DLFW: download OK, CPU enabled — polling FW boot");

  uint32_t cnt = 5000;
  while (r16(REG_MCUFW_CTRL) != 0xC078) {
    if (cnt == 0) {
      if ((r32(REG_FW_DBG7) & 0xFFFFFF00) == ILLEGAL_KEY_GROUP)
        _logger->error("Jaguar2 DLFW: illegal key");
      _logger->error("Jaguar2 DLFW: FW ready check failed (0x80 != 0xC078)");
      return false;
    }
    cnt--;
    delay_us(50);
  }
  _boot.ready_ok = true;
  _logger->info("Jaguar2 DLFW: FW ready (0x80 = 0xC078)");
  return true;
}

bool HalmacJaguar2Fw::download_firmware(const uint8_t *fw_bin, size_t size) {
  _boot = {};
  _boot.supported = true;
  _boot.attempted = true;

  if (!chk_fw_size(fw_bin, size))
    return false;

  /* Back up the LTE/BT-coex register 0x38 across the download (vendor
   * download_firmware_88xx). The read also polls the coex-ready handshake,
   * synchronising the subsystem before the CPU is reset. */
  uint32_t lte_coex_backup = 0;
  ltecoex_read(0x38, lte_coex_backup);

  wlan_cpu_en(false);

  struct Bkp { uint16_t reg; uint8_t len; uint32_t val; };
  std::array<Bkp, 6> bckp{};
  size_t n = 0;

  bckp[n] = {static_cast<uint16_t>(REG_TXDMA_PQ_MAP + 1), 1, r8(REG_TXDMA_PQ_MAP + 1)};
  n++;
  w8(REG_TXDMA_PQ_MAP + 1, static_cast<uint8_t>(HALMAC_DMA_MAPPING_HIGH << 6));

  bckp[n] = {REG_CR, 1, r8(REG_CR)};
  n++;
  bckp[n] = {REG_H2CQ_CSR, 4, (1u << 31)};
  n++;
  w8(REG_CR, static_cast<uint8_t>(BIT_HCI_TXDMA_EN | BIT_TXDMA_EN));
  w32(REG_H2CQ_CSR, (1u << 31));

  bckp[n] = {REG_FIFOPAGE_INFO_1, 2, r16(REG_FIFOPAGE_INFO_1)};
  n++;
  bckp[n] = {REG_RQPN_CTRL_2, 4, r32(REG_RQPN_CTRL_2) | (1u << 31)};
  n++;
  w16(REG_FIFOPAGE_INFO_1, 0x200);
  w32(REG_RQPN_CTRL_2, bckp[n - 1].val);

  uint8_t bcn = r8(REG_BCN_CTRL);
  bckp[n] = {REG_BCN_CTRL, 1, bcn};
  n++;
  w8(REG_BCN_CTRL, static_cast<uint8_t>((bcn & ~(1u << 3)) | (1u << 4)));

  pltfm_reset();

  bool ok = start_dlfw(fw_bin, size);

  for (size_t i = 0; i < n; i++) {
    if (bckp[i].len == 1)
      w8(bckp[i].reg, static_cast<uint8_t>(bckp[i].val));
    else if (bckp[i].len == 2)
      w16(bckp[i].reg, static_cast<uint16_t>(bckp[i].val));
    else
      w32(bckp[i].reg, bckp[i].val);
  }

  if (ok)
    ok = dlfw_end_flow();

  if (!ok) {
    w8(REG_MCUFW_CTRL, static_cast<uint8_t>(r8(REG_MCUFW_CTRL) & ~0x1));
    w8(REG_SYS_FUNC_EN + 1,
       static_cast<uint8_t>(r8(REG_SYS_FUNC_EN + 1) | (1u << 2)));
    ltecoex_write(0x38, lte_coex_backup);
    return false;
  }
  ltecoex_write(0x38, lte_coex_backup);
  return true;
}

} /* namespace jaguar2 */
