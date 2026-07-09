#pragma once

/* UsbTransport — the libusb implementation of IRtlTransport. Everything
 * USB-wire-specific that used to live inside the adapter is here: vendor
 * control transfers for the register plane, sync/async bulk-OUT TX with the
 * wedge (mid-stream stall) recovery and TX submission counters, the
 * kernel-rtw88-style async RX URB queue, and the interface-descriptor walk
 * that discovers the bulk endpoints. The exclusive per-adapter UsbDeviceLock
 * rides here too — its lifetime is the transport's. */

#include <atomic>
#include <memory>

#include <libusb.h>

#include "DeviceConfig.h"
#include "RtlTransport.h"
#include "logger.h"

namespace devourer {
class UsbDeviceLock;
}

#define REALTEK_USB_VENQT_READ 0xC0
#define REALTEK_USB_VENQT_WRITE 0x40
#define USB_TIMEOUT 500

namespace devourer {

class UsbTransport final : public IRtlTransport {
public:
  UsbTransport(libusb_device_handle *dev_handle, Logger_t logger,
               libusb_context *ctx = nullptr,
               std::shared_ptr<devourer::UsbDeviceLock> usb_lock = nullptr);

  bool is_usb() const override { return true; }

  uint8_t read8(uint16_t reg) override { return ctrl_read<uint8_t>(reg); }
  uint16_t read16(uint16_t reg) override { return ctrl_read<uint16_t>(reg); }
  uint32_t read32(uint16_t reg) override { return ctrl_read<uint32_t>(reg); }
  bool write8(uint16_t reg, uint8_t v) override { return ctrl_write(reg, v); }
  bool write16(uint16_t reg, uint16_t v) override { return ctrl_write(reg, v); }
  bool write32(uint16_t reg, uint32_t v) override { return ctrl_write(reg, v); }
  bool write_bytes(uint16_t reg, const uint8_t *p, size_t n) override;

  bool tx_async(uint8_t ep, uint8_t *buf, size_t len,
                unsigned timeout_ms) override;
  int tx_sync(uint8_t ep, uint8_t *buf, size_t len, int timeout_ms) override;
  void rx_loop(int buf_size, int n_urbs,
               const std::function<void(const uint8_t *, int)> &on_data,
               const std::function<bool()> &should_stop) override;
  int rx_raw(uint8_t *buf, int len, int timeout_ms) override;
  void clear_halt(uint8_t ep) override { libusb_clear_halt(_dev_handle, ep); }

  UsbLinkInfo usb_info() const override { return _info; }
  TxStats tx_stats() const override;

private:
  template <typename T> T ctrl_read(uint16_t reg);
  template <typename T> bool ctrl_write(uint16_t reg, T value);
  void discover_endpoints(); /* was InitDvObj */
  const char *speed_str() const;
  static void transfer_callback(struct libusb_transfer *transfer);

  libusb_device_handle *_dev_handle;
  libusb_context *_ctx = nullptr;
  Logger_t _logger;
  UsbLinkInfo _info;

  /* Set by transfer_callback when an async TX bulk-OUT completes non-OK
   * (TIMED_OUT / stall). Consumed at the top of the next tx_async on the TX
   * thread to re-clear_halt the endpoint — a mid-stream stall (e.g. hardware
   * NDP generation on some xhci hosts) would otherwise stay wedged, since the
   * first-send clear_halt only runs once. */
  std::atomic<bool> _tx_wedged{false};

  /* TX submission counters (the driver-drop / congestion signal, TxStats.h).
   * The async transfer_callback increments them from the libusb event thread. */
  std::atomic<uint64_t> _tx_submitted{0};
  std::atomic<uint64_t> _tx_failed{0};
  std::atomic<int> _tx_last_rc{0};
  std::atomic<bool> _tx_last_timeout{false};

  /* Exclusive per-adapter USB lock (UsbDeviceLock.h), held for the transport
   * lifetime; released when the device (and thus the transport) dies. */
  std::shared_ptr<devourer::UsbDeviceLock> _usb_lock;
};

template <typename T> T UsbTransport::ctrl_read(uint16_t reg_num) {
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

template <typename T> bool UsbTransport::ctrl_write(uint16_t reg_num, T value) {
  return libusb_control_transfer(_dev_handle, REALTEK_USB_VENQT_WRITE, 5,
                                 reg_num, 0, (uint8_t *)&value, sizeof(T),
                                 USB_TIMEOUT) == sizeof(T);
}

} /* namespace devourer */
