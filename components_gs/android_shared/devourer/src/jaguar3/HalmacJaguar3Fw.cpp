#include "HalmacJaguar3Fw.h"

#include <array>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "FrameParserJaguar3.h"
#include "HalmacJaguar3Regs.h"
#if defined(DEVOURER_HAVE_JAGUAR3_8822C)
#include "hal8822c_fw.h" /* array_mp_8822c_fw_nic[] + _len */
#endif
#if defined(DEVOURER_HAVE_JAGUAR3_8822E)
#include "hal8822e_fw.h" /* array_mp_8822e_fw_nic[] + _len */
#endif

using namespace jaguar3::halmac;

namespace jaguar3 {

namespace {
/* little-endian field reads from the firmware blob (matches rtk_le32_to_cpu). */
uint32_t le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
void delay_us(uint32_t us) {
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}
} /* namespace */

HalmacJaguar3Fw::HalmacJaguar3Fw(RtlAdapter device, Logger_t logger,
                             ChipVariant variant)
    : _device{device}, _logger{std::move(logger)}, _variant{variant} {}

bool HalmacJaguar3Fw::download_default_firmware() {
  /* The DLFW state machine is identical for 8822c/8822e (registers verified
   * byte-identical); only the firmware image differs. */
#if defined(DEVOURER_HAVE_JAGUAR3_8822E)
  if (_variant == ChipVariant::C8822E)
    return download_firmware(array_mp_8822e_fw_nic, array_mp_8822e_fw_nic_len);
#endif
#if defined(DEVOURER_HAVE_JAGUAR3_8822C)
  return download_firmware(array_mp_8822c_fw_nic, array_mp_8822c_fw_nic_len);
#else
  _logger->error("Jaguar3: no firmware image compiled for this chip variant");
  return false;
#endif
}

/* --- register helpers (HALMAC_REG_R/W*) --- */
uint8_t HalmacJaguar3Fw::r8(uint16_t reg) { return _device.rtw_read8(reg); }
uint16_t HalmacJaguar3Fw::r16(uint16_t reg) { return _device.rtw_read16(reg); }
uint32_t HalmacJaguar3Fw::r32(uint16_t reg) { return _device.rtw_read32(reg); }
void HalmacJaguar3Fw::w8(uint16_t reg, uint8_t v) { _device.rtw_write8(reg, v); }
void HalmacJaguar3Fw::w16(uint16_t reg, uint16_t v) { _device.rtw_write16(reg, v); }
void HalmacJaguar3Fw::w32(uint16_t reg, uint32_t v) { _device.rtw_write32(reg, v); }
void HalmacJaguar3Fw::w32_set(uint16_t reg, uint32_t bits) {
  w32(reg, r32(reg) | bits);
}

/* Port of chk_fw_size_88xx — validates header + section sizes vs total. */
bool HalmacJaguar3Fw::chk_fw_size(const uint8_t *fw_bin, size_t size) {
  if (size < WLAN_FW_HDR_SIZE) {
    _logger->error("Jaguar3 DLFW: fw size < header");
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
    _logger->error("Jaguar3 DLFW: size {} != computed {}", size, real);
    return false;
  }
  return true;
}

/* Port of wlan_cpu_en_88xx — enable/disable the 8051 + its IO interface. */
void HalmacJaguar3Fw::wlan_cpu_en(bool enable) {
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

/* Port of pltfm_reset_88xx (8822C/8822E path — the 8821C/8822B clock-sync
 * branch is N/A for our targets and intentionally omitted). */
void HalmacJaguar3Fw::pltfm_reset() {
  w8(REG_CPU_DMEM_CON + 2,
     static_cast<uint8_t>(r8(REG_CPU_DMEM_CON + 2) & ~0x1));
  w8(REG_CPU_DMEM_CON + 2,
     static_cast<uint8_t>(r8(REG_CPU_DMEM_CON + 2) | 0x1));
}

bool HalmacJaguar3Fw::iddma_en(uint32_t src, uint32_t dest, uint32_t ctrl) {
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

bool HalmacJaguar3Fw::iddma_dlfw(uint32_t src, uint32_t dest, uint32_t len,
                               bool first) {
  uint32_t cnt = HALMC_DDMA_POLLING_COUNT;
  uint32_t ch0_ctrl = BIT_DDMACH0_CHKSUM_EN | BIT_DDMACH0_OWN;
  while (r32(REG_DDMA_CH0CTRL) & BIT_DDMACH0_OWN) {
    if (--cnt == 0) {
      _logger->error("Jaguar3 DLFW: ch0 not ready (CH0CTRL=0x{:08x} "
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
    _logger->error("Jaguar3 DLFW: iddma_en failed");
    return false;
  }
  return true;
}

/* Port of check_fw_chksum_88xx — set IMEM/DMEM done+ok bits in MCUFW_CTRL. */
bool HalmacJaguar3Fw::check_fw_chksum(uint32_t mem_addr) {
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
    _logger->error("Jaguar3 DLFW: fw checksum fail");
    return false;
  }
  if (mem_addr < OCPBASE_DMEM_88XX)
    fw_ctrl |= (BIT_IMEM_DW_OK | BIT_IMEM_CHKSUM_OK);
  else
    fw_ctrl |= (BIT_DMEM_DW_OK | BIT_DMEM_CHKSUM_OK);
  w8(REG_MCUFW_CTRL, fw_ctrl);
  return true;
}

/* Port of send_fwpkt_88xx -> dl_rsvd_page_88xx. The halmac side is a register
 * bracket around the platform PLTFM_SEND_RSVD_PAGE; the platform side (build an
 * 8822C TX descriptor for the chunk and bulk-OUT it to the beacon/rsvd page) is
 * devourer's, now that FrameParserJaguar3 exists.
 *
 * NB: only reachable after power-on/queue-init. _rsvd_boundary and the
 * HIQ/beacon endpoint come from the queue allocation; QSEL_BEACON and OFFSET
 * match the rsvd-page download convention. */
bool HalmacJaguar3Fw::send_fw_page(uint16_t pg_addr, const uint8_t *chunk,
                                 uint32_t size) {
  if (size == 0)
    return false;

  /* Point the BCN/rsvd-page head at pg_addr and arm download (BIT15). */
  w16(REG_FIFOPAGE_CTRL_2,
      static_cast<uint16_t>((pg_addr & BIT_MASK_BCN_HEAD_1_V1) | (1u << 15)));

  uint8_t cr1 = r8(REG_CR + 1);
  w8(REG_CR + 1, static_cast<uint8_t>(cr1 | 0x1));
  uint8_t txq2 = r8(REG_FWHW_TXQ_CTRL + 2);
  w8(REG_FWHW_TXQ_CTRL + 2, static_cast<uint8_t>(txq2 & ~(1u << 6)));

  /* Build [48-byte TX desc][chunk] and bulk-OUT (PLTFM_SEND_RSVD_PAGE). */
  std::vector<uint8_t> frame(TXDESC_SIZE_8822C + size, 0);
  uint8_t *d = frame.data();
  SET_TX_DESC_TXPKTSIZE_8822C(d, size);
  SET_TX_DESC_OFFSET_8822C(d, static_cast<uint32_t>(TXDESC_SIZE_8822C));
  SET_TX_DESC_QSEL_8822C(d, QSEL_BEACON);
  SET_TX_DESC_USE_RATE_8822C(d, 1);
  SET_TX_DESC_DATARATE_8822C(d, 0); /* lowest rate */
  SET_TX_DESC_DISDATAFB_8822C(d, 1);
  SET_TX_DESC_LS_8822C(d, 1);
  std::memcpy(d + TXDESC_SIZE_8822C, chunk, size);
  cal_txdesc_chksum_8822c(d);

  bool sent = _device.bulk_send_sync_ep(_device.first_bulk_out_ep(),
                                        frame.data(),
                                        static_cast<int>(frame.size()),
                                        1000) >= 0;
  bool status = sent;

  if (sent) {
    /* Poll rsvd-page download OK (FIFOPAGE_CTRL_2+1 BIT7 = bcn valid). */
    uint32_t cnt = 1000;
    while ((r8(REG_FIFOPAGE_CTRL_2 + 1) & (1u << 7)) == 0) {
      if (--cnt == 0) {
        _logger->error("Jaguar3 DLFW: rsvd-page (bcn valid) poll failed");
        status = false;
        break;
      }
      delay_us(10);
    }
  } else {
    _logger->error("Jaguar3 DLFW: rsvd-page bulk-OUT failed");
  }

  /* Restore (rsvd_boundary head + the two saved bytes). */
  w16(REG_FIFOPAGE_CTRL_2,
      static_cast<uint16_t>(_rsvd_boundary | (1u << 15)));
  w8(REG_FWHW_TXQ_CTRL + 2, txq2);
  w8(REG_CR + 1, cr1);
  return status;
}

/* Port of dlfw_to_mem_88xx — chunked rsvd-page TX + IDDMA copy + checksum. */
bool HalmacJaguar3Fw::dlfw_to_mem(const uint8_t *fw_bin, uint32_t src,
                                uint32_t dest, uint32_t size) {
  uint32_t mem_offset = 0;
  bool first = true;
  uint32_t residue = size;

  /* Plain write (not RMW): clears a stale OWN bit so the per-chunk OWN-idle
   * poll in iddma_dlfw passes. Matches the kernel rtw88_8822cu, which writes
   * CH0CTRL = RESET_CHKSUM_STS directly here. */
  w32(REG_DDMA_CH0CTRL, BIT_DDMACH0_RESET_CHKSUM_STS);

  while (residue != 0) {
    uint32_t pkt = (residue >= _dlfw_pkt_size) ? _dlfw_pkt_size : residue;
    if (!send_fw_page(static_cast<uint16_t>(src >> 7), fw_bin + mem_offset, pkt))
      return false;
    if (!iddma_dlfw(OCPBASE_TXBUF_88XX + src + TX_DESC_SIZE_88XX,
                    dest + mem_offset, pkt, first))
      return false;
    first = false;
    mem_offset += pkt;
    residue -= pkt;
  }
  return check_fw_chksum(dest);
}

/* Port of start_dlfw_88xx — parse header, set FWDL_EN, push dmem/imem/emem. */
bool HalmacJaguar3Fw::start_dlfw(const uint8_t *fw_bin, size_t /*size*/) {
  uint32_t dmem = le32(fw_bin + WLAN_FW_HDR_DMEM_SIZE) + WLAN_FW_HDR_CHKSUM_SIZE;
  uint32_t imem = le32(fw_bin + WLAN_FW_HDR_IMEM_SIZE) + WLAN_FW_HDR_CHKSUM_SIZE;
  uint32_t emem = 0;
  if ((fw_bin[WLAN_FW_HDR_MEM_USAGE] & (1u << 4)) != 0)
    emem = le32(fw_bin + WLAN_FW_HDR_EMEM_SIZE) + WLAN_FW_HDR_CHKSUM_SIZE;

  /* FWDL_EN (preserve 0x3800 bits) */
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

/* Port of dlfw_end_flow_88xx — verify IMEM/DMEM chksum, set FW_DW_RDY, enable
 * CPU, poll REG_MCUFW_CTRL == 0xC078 (FW booted). */
bool HalmacJaguar3Fw::dlfw_end_flow() {
  w32(REG_TXDMA_STATUS, 1u << 2);

  uint16_t fw_ctrl = r16(REG_MCUFW_CTRL);
  if ((fw_ctrl & 0x50) != 0x50) {
    _logger->error("Jaguar3 DLFW: IMEM/DMEM checksum not ready (0x{:04x})",
                   fw_ctrl);
    return false;
  }
  _boot.checksum_ok = true;
  w16(REG_MCUFW_CTRL,
      static_cast<uint16_t>((fw_ctrl | BIT_FW_DW_RDY) & ~0x1));

  wlan_cpu_en(true);
  _logger->info("Jaguar3 DLFW: download OK, CPU enabled — polling FW boot");

  uint32_t cnt = 5000;
  while (r16(REG_MCUFW_CTRL) != 0xC078) {
    if (cnt == 0) {
      if ((r32(REG_FW_DBG7) & 0xFFFFFF00) == ILLEGAL_KEY_GROUP)
        _logger->error("Jaguar3 DLFW: illegal key");
      _logger->error("Jaguar3 DLFW: FW ready check failed (0x80 != 0xC078)");
      return false;
    }
    cnt--;
    delay_us(50);
  }
  _boot.ready_ok = true;
  _logger->info("Jaguar3 DLFW: FW ready (0x80 = 0xC078)");
  return true;
}

/* Port of download_firmware_88xx (top level). NB: the halmac LTE-coex backup/
 * restore around 0x38 is intentionally omitted — no LTE coex in devourer's
 * monitor/inject scope. */
bool HalmacJaguar3Fw::download_firmware(const uint8_t *fw_bin, size_t size) {
  _boot = {};
  _boot.supported = true;
  _boot.attempted = true;

  if (!chk_fw_size(fw_bin, size))
    return false;

  wlan_cpu_en(false);

  /* Backup the registers DLFW clobbers, set DLFW config, restore after. */
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

  /* restore_mac_reg_88xx */
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
    /* disable FWDL_EN + re-enable CPU (halmac DLFW_FAIL path) */
    w8(REG_MCUFW_CTRL, static_cast<uint8_t>(r8(REG_MCUFW_CTRL) & ~0x1));
    w8(REG_SYS_FUNC_EN + 1,
       static_cast<uint8_t>(r8(REG_SYS_FUNC_EN + 1) | (1u << 2)));
    return false;
  }
  return true;
}

} /* namespace jaguar3 */
