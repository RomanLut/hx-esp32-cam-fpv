#include "RtlUsbAdapter.h"

#include <chrono>
#if defined(__ANDROID__) || defined(_MSC_VER) || defined(__APPLE__)
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif
#include "FrameParser.h"
#include "Hal8812PhyReg.h"
#include "logger.h"
#include <iomanip>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

//===================================================================================
//===================================================================================
// Initializes the Realtek USB adapter state from the claimed libusb device.
RtlUsbAdapter::RtlUsbAdapter(libusb_device_handle *dev_handle, Logger_t logger)
    : _dev_handle{dev_handle}, _logger{logger}
{
  InitDvObj();

  if (usbSpeed > LIBUSB_SPEED_HIGH) // USB 3.0
  {
      // Match the working rtl8812au USB RX aggregation settings. Smaller
      // thresholds can leave RTL8821AU monitor traffic stuck in the RXDMA path.
      rxagg_usb_size = 0x7;
      rxagg_usb_timeout = 0x1a;
  }
  else
  {
      // Working rtl8812au uses this USB2.0 setting when preallocated RX buffers
      // are not enabled.
      rxagg_usb_size = 0x5;
      rxagg_usb_timeout = 0x20;
  }

  GetChipOutEP8812();

  uint8_t eeValue = rtw_read8(REG_9346CR);
  EepromOrEfuse = (eeValue & BOOT_FROM_EEPROM) != 0;
  AutoloadFailFlag = (eeValue & EEPROM_EN) == 0;

  _logger->info("Boot from {}, Autoload {} !",
                EepromOrEfuse ? "EEPROM" : "EFUSE",
                (AutoloadFailFlag ? "Fail" : "OK"));
}

//===================================================================================
//===================================================================================
// Returns true when this USB ID needs the RTL8821A HAL and firmware path.
bool RtlUsbAdapter::IsRtl8821A() const
{
  return chipType == RtlChipType::RTL8821;
}

/*
$ lsusb -v -d 0bda:8812
      Endpoint Descriptor:
        bLength                 7
        bDescriptorType         5
        bEndpointAddress     0x81  EP 1 IN
        bmAttributes            2
          Transfer Type            Bulk
          Synch Type               None
          Usage Type               Data
        wMaxPacketSize     0x0200  1x 512 bytes
        bInterval               0
*/

//===================================================================================
//===================================================================================
// Reads pending packets from the adapter bulk IN endpoint and parses RX frames.
std::vector<Packet> RtlUsbAdapter::infinite_read()
{
  static constexpr int BUF_SIZE = 32 * 1024;
  uint8_t buffer[BUF_SIZE] = {};
  int actual_length = 0;
  int rc;

  rc = libusb_bulk_transfer(_dev_handle, _bulk_in_ep, buffer, sizeof(buffer),
                            &actual_length, USB_TIMEOUT * 10);

  if (rc < 0)
  {
    std::this_thread::sleep_for(50ms);
  }

  std::vector<Packet> packets;
  FrameParser fp{_logger};
  packets =
      fp.recvbuf2recvframe(std::span<uint8_t>{buffer, (size_t)actual_length});
  return packets;
}

bool RtlUsbAdapter::WriteBytes(uint16_t reg_num, uint8_t *ptr, size_t size) {
  if (libusb_control_transfer(_dev_handle, REALTEK_USB_VENQT_WRITE, 5, reg_num,
                              0, ptr, size, USB_TIMEOUT) == size) {
    return true;
  }
  return false;
}

void RtlUsbAdapter::rtl8812au_hw_reset() {
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

void RtlUsbAdapter::_8051Reset8812() {
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
uint8_t RtlUsbAdapter::efuse_OneByteRead(uint16_t addr, uint8_t *data) {
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

void RtlUsbAdapter::ReadEFuseByte(uint16_t _offset, uint8_t *pbuf) {
  uint32_t value32;
  uint8_t readbyte;
  uint16_t retry;

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

const char *RtlUsbAdapter::strUsbSpeed() {
  switch (usbSpeed) {
  case LIBUSB_SPEED_UNKNOWN:
    return "UNKNOWN";
  case LIBUSB_SPEED_LOW:
    return "1.5MBit/s";
  case LIBUSB_SPEED_FULL:
    return "12MBit/s";
  case LIBUSB_SPEED_HIGH:
    return "480MBit/s";
  case LIBUSB_SPEED_SUPER:
    return "5000MBit/s";
  case LIBUSB_SPEED_SUPER_PLUS:
    return "10000MBit/s";
  default:
    return NULL;
  }
}

//===================================================================================
//===================================================================================
// Reads USB descriptors, selects the chip family, and records bulk endpoints.
void RtlUsbAdapter::InitDvObj()
{
  libusb_device *dev = libusb_get_device(_dev_handle);
  usbSpeed = (enum libusb_speed)libusb_get_device_speed(dev);
  _logger->info("Running USB bus at {}", strUsbSpeed());

  libusb_device_descriptor desc;
  int ret = libusb_get_device_descriptor(dev, &desc);
  if (ret < 0)
  {
    return;
  }

  UsbVendorId = desc.idVendor;
  UsbProductId = desc.idProduct;

  // Keep the RTL8821A USB ID split aligned with svpcom/rtl8812au
  // v5.2.20-rssi-fix-but-sometimes-crash. 
  switch ((uint32_t(UsbVendorId) << 16) | UsbProductId)
  {
  case 0x0BDA0811:
  case 0x0BDA0821:
  case 0x0BDA8822:
  case 0x0BDAA811:
  case 0x0BDA0820:
  case 0x0BDA0823:
  case 0x04110242:
  case 0x0411029B:
  case 0x04BB0953:
  case 0x056E4007:
  case 0x056E400E:
  case 0x056E400F:
  case 0x08469052:
  case 0x0E660023:
  case 0x20013314:
  case 0x20013318:
  case 0x2019AB32:
  case 0x2357011E:
  case 0x23570120:
  case 0x23570122:
  case 0x38236249:
  case 0x7392A811:
  case 0x7392A812:
  case 0x7392A813:
    chipType = RtlChipType::RTL8821;
    break;
  default:
    chipType = RtlChipType::RTL8812;
    break;
  }

  for (uint8_t k = 0; k < desc.bNumConfigurations; k++)
  {
    libusb_config_descriptor *config;
    ret = libusb_get_config_descriptor(dev, k, &config);
    if (LIBUSB_SUCCESS != ret)
    {
      continue;
    }

    if (!config->bNumInterfaces)
    {
      libusb_free_config_descriptor(config);
      continue;
    }
    const libusb_interface *interface = &config->interface[0];

    if (!interface->altsetting)
    {
      libusb_free_config_descriptor(config);
      continue;
    }
    const libusb_interface_descriptor *interface_desc =
        &interface->altsetting[0];

    bool foundBulkIn = false;
    for (uint8_t j = 0; j < interface_desc->bNumEndpoints; j++)
    {
      const libusb_endpoint_descriptor *endpoint = &interface_desc->endpoint[j];
      uint8_t endPointAddr = endpoint->bEndpointAddress;
      bool isBulk = (endpoint->bmAttributes & 0b11) ==
                    LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK;

      if (isBulk && !(endPointAddr & LIBUSB_ENDPOINT_IN))
      {
        numOutPipes++;
        _bulk_out_eps.push_back(endPointAddr);
      }

      if (isBulk && (endPointAddr & LIBUSB_ENDPOINT_IN) && !foundBulkIn)
      {
        _bulk_in_ep = endPointAddr;
        foundBulkIn = true;
      }
    }

    if (foundBulkIn)
    {
      // 8821AU exposes bulk IN 0x84 on known 2357:0120 hardware. Clear any
      // stale HALT on the discovered endpoint before the RX loop starts.
      libusb_clear_halt(_dev_handle, _bulk_in_ep);
    }

    libusb_free_config_descriptor(config);
    break;
  }
}

void RtlUsbAdapter::GetChipOutEP8812() {
  OutEpQueueSel = 0;
  OutEpNumber = 0;

  switch (numOutPipes) {
  case 4:
    OutEpQueueSel = TxSele::TX_SELE_HQ | TxSele::TX_SELE_LQ |
                    TxSele::TX_SELE_NQ | TxSele::TX_SELE_EQ;
    OutEpNumber = 4;
    break;
  case 3:
    OutEpQueueSel =
        TxSele::TX_SELE_HQ | TxSele::TX_SELE_LQ | TxSele::TX_SELE_NQ;
    OutEpNumber = 3;
    break;
  case 2:
    OutEpQueueSel = TxSele::TX_SELE_HQ | TxSele::TX_SELE_NQ;
    OutEpNumber = 2;
    break;
  case 1:
    OutEpQueueSel = TxSele::TX_SELE_HQ;
    OutEpNumber = 1;
    break;
  default:
    break;
  }

  _logger->info("OutEpQueueSel({}), OutEpNumber({})", (int)OutEpQueueSel,
                (int)OutEpNumber);
}

void transfer_callback(struct libusb_transfer *transfer) {
  Logger *_logger = (Logger *)(transfer->user_data);
  if (transfer->status == LIBUSB_TRANSFER_COMPLETED &&
      transfer->actual_length == transfer->length) {
    _logger->debug("Packet {} sent successfully, length: {}", _logger,
                  transfer->length);
  } else {
    _logger->error("Failed to send packet {}, status: {}, actual length: {}",
                   _logger, transfer->status, transfer->actual_length);
  }
  libusb_free_transfer(transfer);
}

//===================================================================================
//===================================================================================
// Submits one packet to the selected bulk OUT endpoint.
bool RtlUsbAdapter::send_packet(uint8_t *packet, size_t length)
{
  libusb_transfer *transfer = libusb_alloc_transfer(0);
  if (!transfer)
  {
    _logger->error("Failed to allocate transfer");
    return false;
  }

  uint8_t txEp = !_bulk_out_eps.empty() ? _bulk_out_eps[0] : 0x02;
  static bool firstPktSetup = true;
  if (firstPktSetup)
  {
    firstPktSetup = false;
    libusb_clear_halt(_dev_handle, txEp);
  }

  libusb_fill_bulk_transfer(transfer, _dev_handle, txEp, packet, length,
                            transfer_callback, (void *)(_logger.get()),
                            USB_TIMEOUT);
  auto start = std::chrono::high_resolution_clock::now();
  int rc = rc = libusb_submit_transfer(transfer);
  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> elapsed = end - start;
  if (rc == LIBUSB_SUCCESS) {
    _logger->debug("Packet sent successfully, length: {},used time {}ms", length,
                  elapsed.count());
    return true;
  } else {
    _logger->error("Failed to send packet, error code: {}", rc);
    libusb_free_transfer(transfer);
    return false;
  }
}

void RtlUsbAdapter::phy_set_bb_reg(uint16_t regAddr, uint32_t bitMask,
                                   uint32_t data) {
  PHY_SetBBReg8812(regAddr, bitMask, data);
}

void RtlUsbAdapter::PHY_SetBBReg8812(uint16_t regAddr, uint32_t bitMask,
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
