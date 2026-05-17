#ifndef RTLUSBADAPTER_H
#define RTLUSBADAPTER_H

#include <iostream>

#include <libusb.h>
#include <thread>

#include "FrameParser.h"
#include "drv_types.h"
#include "hal_com_reg.h"
#include "logger.h"

#define rtw_read8 rtw_read<uint8_t>
#define rtw_read16 rtw_read<uint16_t>
#define rtw_read32 rtw_read<uint32_t>

#define rtw_write8 rtw_write<uint8_t>
#define rtw_write16 rtw_write<uint16_t>
#define rtw_write32 rtw_write<uint32_t>

#define REALTEK_USB_VENQT_READ 0xC0
#define REALTEK_USB_VENQT_WRITE 0x40
#define USB_TIMEOUT 500

enum TxSele {
  TX_SELE_HQ = 1 << (0), /* High Queue */
  TX_SELE_LQ = 1 << (1), /* Low Queue */
  TX_SELE_NQ = 1 << (2), /* Normal Queue */
  TX_SELE_EQ = 1 << (3), /* Extern Queue */
};

class RtlUsbAdapter {
  libusb_device_handle *_dev_handle;
  Logger_t _logger;

  enum libusb_speed usbSpeed;
  uint8_t numOutPipes = 0;

public:
  RtlUsbAdapter(libusb_device_handle *dev_handle, Logger_t logger);

  bool AutoloadFailFlag = false;
  bool EepromOrEfuse = false;
  uint8_t OutEpQueueSel;
  uint8_t OutEpNumber;
  uint8_t rxagg_usb_size;
  uint8_t rxagg_usb_timeout;
  bool send_packet(uint8_t* packet, size_t length);
  std::vector<Packet> infinite_read();
  uint8_t efuse_OneByteRead(uint16_t addr, uint8_t *data);
  void phy_set_bb_reg(uint16_t regAddr, uint32_t bitMask, uint32_t data);

  template <typename T> T rtw_read(uint16_t reg_num) {
    T data = 0;
    if (libusb_control_transfer(_dev_handle, REALTEK_USB_VENQT_READ, 5, reg_num,
                                0, (uint8_t *)&data, sizeof(T),
                                USB_TIMEOUT) == sizeof(T)) {
      return data;
    }

    _logger->error("rtw_read({:04x}), sizeof(T) = {}", reg_num, sizeof(T));
    throw std::ios_base::failure("rtw_read");
    return 0;
  }

  template <typename T> bool rtw_write(uint16_t reg_num, T value) {
    if (libusb_control_transfer(_dev_handle, REALTEK_USB_VENQT_WRITE, 5,
                                reg_num, 0, (uint8_t *)&value, sizeof(T),
                                USB_TIMEOUT) == sizeof(T)) {
      return true;
    }
    return false;
  }

  bool WriteBytes(uint16_t reg_num, uint8_t *ptr, size_t size);

  void rtl8812au_hw_reset();
  void _8051Reset8812();
  void ReadEFuseByte(uint16_t _offset, uint8_t *pbuf);

private:
  void InitDvObj();
  const char *strUsbSpeed();
  void GetChipOutEP8812();
  void PHY_SetBBReg8812(uint16_t regAddr, uint32_t bitMask,
                        uint32_t dataOriginal);
};

#endif /* RTLUSBADAPTER_H */
