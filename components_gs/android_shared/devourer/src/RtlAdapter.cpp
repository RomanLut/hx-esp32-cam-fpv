/* UsbTransport.h (-> libusb.h -> windows.h on Windows) MUST precede the hal
 * headers: Hal8812PhyReg.h #defines identifiers like `bEnable` that
 * winuser.h uses as parameter names, so hal-first poisons the Windows headers
 * (MSVC C2143 inside winuser.h / mingw "expected ','" pointed at the
 * #define). The pre-seam RtlUsbAdapter.h guaranteed this order by including
 * libusb.h first; this TU is the only one mixing both after the split. */
#include "UsbTransport.h"

#include "RtlAdapter.h"

#include <chrono>
#include <thread>
#include <utility>

#include "Hal8812PhyReg.h"

using namespace std::chrono_literals;

RtlAdapter::RtlAdapter(libusb_device_handle *dev_handle, Logger_t logger,
                       libusb_context *ctx,
                       std::shared_ptr<devourer::UsbDeviceLock> usb_lock,
                       const devourer::DeviceConfig &cfg)
    : _transport{std::make_shared<devourer::UsbTransport>(
          dev_handle, logger, ctx, std::move(usb_lock))},
      _logger{std::move(logger)} {
  init_from_transport(cfg);
}

RtlAdapter::RtlAdapter(std::shared_ptr<devourer::IRtlTransport> transport,
                       Logger_t logger, const devourer::DeviceConfig &cfg)
    : _transport{std::move(transport)}, _logger{std::move(logger)} {
  init_from_transport(cfg);
}

void RtlAdapter::init_from_transport(const devourer::DeviceConfig &cfg) {
  _log_writes = cfg.debug.log_writes;
  _tx_ep_override = cfg.tx.ep;
  _tx_timeout_ms = cfg.tx.timeout_ms.value_or(500);
  _usb = _transport->usb_info();

  if (_usb.valid) {
    if (_usb.speed > 3 /* > LIBUSB_SPEED_HIGH: USB 3.0 */) {
      rxagg_usb_size = 0x3; /* 16KB */
      rxagg_usb_timeout = 0x01;
    } else {
      /* the setting to reduce RX FIFO overflow on USB2.0 and increase rx
       * throughput */
      rxagg_usb_size = 0x1; /* 8KB */
      rxagg_usb_timeout = 0x01;
    }

    /* Was GetChipOutEP8812: derive the TX queue selector from the number of
     * discovered bulk-OUT pipes. */
    switch (_usb.bulk_out_eps.size()) {
    case 4:
      OutEpQueueSel = TX_SELE_HQ | TX_SELE_LQ | TX_SELE_NQ | TX_SELE_EQ;
      OutEpNumber = 4;
      break;
    case 3:
      OutEpQueueSel = TX_SELE_HQ | TX_SELE_LQ | TX_SELE_NQ;
      OutEpNumber = 3;
      break;
    case 2:
      OutEpQueueSel = TX_SELE_HQ | TX_SELE_NQ;
      OutEpNumber = 2;
      break;
    case 1:
      OutEpQueueSel = TX_SELE_HQ;
      OutEpNumber = 1;
      break;
    default:
      break;
    }
    _logger->info("OutEpQueueSel({}), OutEpNumber({})", (int)OutEpQueueSel,
                  (int)OutEpNumber);
  }

  uint8_t eeValue = rtw_read8(REG_9346CR);
  EepromOrEfuse = (eeValue & BOOT_FROM_EEPROM) != 0;
  AutoloadFailFlag = (eeValue & EEPROM_EN) == 0;

  _logger->info("Boot from {}, Autoload {} !",
                EepromOrEfuse ? "EEPROM" : "EFUSE",
                (AutoloadFailFlag ? "Fail" : "OK"));
}

bool RtlAdapter::send_packet(uint8_t *packet, size_t length) {
  /* TX endpoint selection (USB semantics; the PCIe transport ignores it):
   * DeviceConfig tx.ep override > first discovered OUT endpoint > historic
   * 8812AU default (0x02). */
  const uint8_t tx_ep = _tx_ep_override ? *_tx_ep_override
                                        : first_bulk_out_ep();
  return _transport->tx_async(tx_ep, packet, length, _tx_timeout_ms);
}

void RtlAdapter::rtl8812au_hw_reset() {
  uint32_t reg_val = 0;

  if ((rtw_read8(REG_MCUFWDL) & BIT7) != 0) {
    _8051Reset8812();
    rtw_write8(REG_MCUFWDL, 0x00);

    /* before BB reset should do clock gated */
    rtw_write32(rFPGA0_XCD_RFPara, rtw_read32(rFPGA0_XCD_RFPara) | (BIT6));

    /* reset BB */
    reg_val = rtw_read8(REG_SYS_FUNC_EN);
    reg_val = (uint8_t)(reg_val & ~(BIT0 | BIT1));
    rtw_write8(REG_SYS_FUNC_EN, (uint8_t)reg_val);

    /* reset RF */
    rtw_write8(REG_RF_CTRL, 0);

    /* reset TRX path */
    rtw_write16(REG_CR, 0);

    /* reset MAC */
    reg_val = rtw_read8(REG_APS_FSMCO + 1);
    reg_val |= BIT1;
    rtw_write8(REG_APS_FSMCO + 1,
               (uint8_t)reg_val); /* reg0x5[1] ,auto FSM off */

    reg_val = rtw_read8(REG_APS_FSMCO + 1);

    /* check if   reg0x5[1] auto cleared */
    while ((reg_val & BIT1) != 0) {
      std::this_thread::sleep_for(1ms);
      reg_val = rtw_read8(REG_APS_FSMCO + 1);
    }

    reg_val |= BIT0;
    rtw_write8(REG_APS_FSMCO + 1,
               (uint8_t)reg_val); /* reg0x5[0] ,auto FSM on */

    reg_val = rtw_read8(REG_SYS_FUNC_EN + 1);
    reg_val = (uint8_t)(reg_val & ~(BIT4 | BIT7));
    rtw_write8(REG_SYS_FUNC_EN + 1, (uint8_t)reg_val);
    reg_val = rtw_read8(REG_SYS_FUNC_EN + 1);
    reg_val = (uint8_t)(reg_val | BIT4 | BIT7);
    rtw_write8(REG_SYS_FUNC_EN + 1, (uint8_t)reg_val);
  }
}

void RtlAdapter::_8051Reset8812() {
  uint8_t u1bTmp, u1bTmp2;

  /* Reset MCU IO Wrapper- sugggest by SD1-Gimmy */
  u1bTmp2 = rtw_read8(REG_RSV_CTRL);
  rtw_write8(REG_RSV_CTRL, (uint8_t)(u1bTmp2 & (~BIT1)));
  u1bTmp2 = rtw_read8(REG_RSV_CTRL + 1);
  rtw_write8(REG_RSV_CTRL + 1, (uint8_t)(u1bTmp2 & (~BIT3)));

  u1bTmp = rtw_read8(REG_SYS_FUNC_EN + 1);
  rtw_write8(REG_SYS_FUNC_EN + 1, (uint8_t)(u1bTmp & (~BIT2)));

  /* Enable MCU IO Wrapper */
  u1bTmp2 = rtw_read8(REG_RSV_CTRL);
  rtw_write8(REG_RSV_CTRL, (uint8_t)(u1bTmp2 & (~BIT1)));
  u1bTmp2 = rtw_read8(REG_RSV_CTRL + 1);
  rtw_write8(REG_RSV_CTRL + 1, (uint8_t)(u1bTmp2 | (BIT3)));

  rtw_write8(REG_SYS_FUNC_EN + 1, (uint8_t)(u1bTmp | (BIT2)));

  _logger->info("=====> _8051Reset8812(): 8051 reset success .");
}

/*  11/16/2008 MH Read one byte from real Efuse. */
uint8_t RtlAdapter::efuse_OneByteRead(uint16_t addr, uint8_t *data) {
  u32 tmpidx = 0;
  u8 bResult;
  u8 readbyte;

  /* -----------------e-fuse reg ctrl --------------------------------- */
  /* address			 */
  rtw_write8(EFUSE_CTRL + 1, (u8)(addr & 0xff));
  rtw_write8(EFUSE_CTRL + 2,
             ((u8)((addr >> 8) & 0x03)) | (rtw_read8(EFUSE_CTRL + 2) & 0xFC));

  /* rtw_write8(pAdapter, EFUSE_CTRL+3,  0x72); */ /* read cmd	 */
  /* Write bit 32 0 */
  readbyte = rtw_read8(EFUSE_CTRL + 3);
  rtw_write8(EFUSE_CTRL + 3, (readbyte & 0x7f));

  while (!(0x80 & rtw_read8(EFUSE_CTRL + 3)) && (tmpidx < 1000)) {
    std::this_thread::sleep_for(1ms);
    tmpidx++;
  }
  if (tmpidx < 100) {
    *data = rtw_read8(EFUSE_CTRL);
    bResult = true;
  } else {
    *data = 0xff;
    bResult = false;
    _logger->error("addr=0x{:x} bResult={} time out 1s !!!", addr, bResult);
    _logger->error("EFUSE_CTRL =0x{:08x} !!!", rtw_read32(EFUSE_CTRL));
  }

  return bResult;
}

void RtlAdapter::ReadEFuseByte(uint16_t _offset, uint8_t *pbuf) {
  uint32_t value32;
  uint8_t readbyte;
  uint16_t retry;

  /* Match the kernel `88XXau` driver's per-iteration EFUSE_TEST clear.
   * Cold-init usbmon diff (2026-05-28, devourer-testrig VM kernel-side
   * vs host devourer-side) shows the kernel does an RD-then-WR sequence
   * at REG_EFUSE_TEST (0x0034) = 0x0000 (16-bit) BEFORE every EFUSE byte
   * read, 312 times per init; devourer never touched 0x0034. We mirror
   * the sequence so the EFUSE state machine sees identical wire shape
   * across all 312 byte reads. Empirically harmless on its own (does
   * NOT fix the RTL8814AU TX-on-air gate per a sniffer run with this
   * patch + bulk-IN drainer enabled) but removes a known concrete
   * wire-level divergence flagged by tools/usbmon_pcap_diff.py. */
  (void)rtw_read16(REG_EFUSE_TEST);
  rtw_write16(REG_EFUSE_TEST, 0x0000);

  /* Write Address */
  rtw_write8(EFUSE_CTRL + 1, (uint8_t)(_offset & 0xff));
  readbyte = rtw_read8(EFUSE_CTRL + 2);
  rtw_write8(EFUSE_CTRL + 2,
             (uint8_t)(((_offset >> 8) & 0x03) | (readbyte & 0xfc)));

  /* Write bit 32 0 */
  readbyte = rtw_read8(EFUSE_CTRL + 3);
  rtw_write8(EFUSE_CTRL + 3, (uint8_t)(readbyte & 0x7f));

  /* Check bit 32 read-ready */
  retry = 0;
  value32 = rtw_read32(EFUSE_CTRL);
  /* while(!(((value32 >> 24) & 0xff) & 0x80)  && (retry<10)) */
  while ((((value32 >> 24) & 0xff) & 0x80) == 0 && (retry < 10000)) {
    value32 = rtw_read32(EFUSE_CTRL);
    retry++;
  }

  /* 20100205 Joseph: Add delay suggested by SD1 Victor. */
  /* This fix the problem that Efuse read error in high temperature condition.
   */
  /* Designer says that there shall be some delay after ready bit is set, or the
   */
  /* result will always stay on last data we read. */

  // TODO: decide to we really need it?
  // std::this_thread::sleep_for(50ms);
  value32 = rtw_read32(EFUSE_CTRL);

  pbuf[0] = (uint8_t)(value32 & 0xff);
}

void RtlAdapter::phy_set_bb_reg(uint16_t regAddr, uint32_t bitMask,
                                uint32_t data) {
  PHY_SetBBReg8812(regAddr, bitMask, data);
}

void RtlAdapter::PHY_SetBBReg8812(uint16_t regAddr, uint32_t bitMask,
                                  uint32_t dataOriginal) {
  uint32_t data = dataOriginal;
  if (bitMask != bMaskDWord) {
    /* if not "double word" write */
    auto OriginalValue = rtw_read32(regAddr);
    auto BitShift = PHY_CalculateBitShift(bitMask);
    data = ((OriginalValue) & (~bitMask)) |
           (((dataOriginal << (int)BitShift)) & bitMask);
  }

  rtw_write32(regAddr, data);

  /* RTW_INFO("BBW MASK=0x%x Addr[0x%x]=0x%x\n", BitMask, RegAddr, Data); */
}
