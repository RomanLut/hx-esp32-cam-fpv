#pragma once

/* IRtlTransport — the bus seam. USB (libusb) and PCIe (vfio) are independent
 * transports implementing this one interface; RtlAdapter (the copyable value
 * type every HAL holds) owns a shared_ptr to one of them and forwards. Nothing
 * here depends on libusb or vfio.
 *
 * Two planes, mirroring how the chips are built:
 *  - register plane: 8/16/32-bit accesses into the 0x0000..0xFFFF MAC/BB
 *    register space (USB: vendor control transfers; PCIe: BAR2 MMIO).
 *  - frame plane: TX submissions and the blocking RX delivery loop
 *    (USB: bulk endpoints; PCIe: 88xx buffer-descriptor DMA rings).
 *
 * hci_setup() is the rtw88-style pre-power hook: programming that must happen
 * before the power-on sequence each bring-up attempt (PCIe TRX ring
 * registers; nothing on USB). */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "TxStats.h"

namespace devourer {

/* USB-descriptor-derived link facts consumed by the HALs (queue mapping,
 * RX-aggregation sizing). A PCIe transport returns the defaults. */
/* libusb_speed numeric values, mirrored so HAL code comparing link speed does
 * not need libusb.h (the whole point of the transport seam). */
constexpr int kUsbSpeedHigh = 3;  /* LIBUSB_SPEED_HIGH  (480 MBit/s) */
constexpr int kUsbSpeedSuper = 4; /* LIBUSB_SPEED_SUPER (5 GBit/s)   */

struct UsbLinkInfo {
  bool valid = false;    /* true only on the USB transport */
  int speed = 0;         /* libusb_speed numeric value */
  uint16_t vid = 0, pid = 0;
  uint8_t bulk_in_ep = 0x81;
  std::vector<uint8_t> bulk_out_eps; /* descriptor order */
};

class IRtlTransport {
public:
  virtual ~IRtlTransport() = default;

  virtual bool is_usb() const = 0;

  /* ---- register plane ---- */
  virtual uint8_t read8(uint16_t reg) = 0;
  virtual uint16_t read16(uint16_t reg) = 0;
  virtual uint32_t read32(uint16_t reg) = 0;
  virtual bool write8(uint16_t reg, uint8_t v) = 0;
  virtual bool write16(uint16_t reg, uint16_t v) = 0;
  virtual bool write32(uint16_t reg, uint32_t v) = 0;
  virtual bool write_bytes(uint16_t reg, const uint8_t *p, size_t n) = 0;

  /* ---- frame plane ---- */
  /* Fire-and-forget data TX (the send_packet hot path). `ep` is the USB
   * bulk-OUT endpoint choice; the PCIe transport ignores it (the ring is
   * chosen from the descriptor's QSEL). */
  virtual bool tx_async(uint8_t ep, uint8_t *buf, size_t len,
                        unsigned timeout_ms) = 0;
  /* Synchronous TX that blocks until the transport confirms consumption
   * (USB: bulk completion; PCIe: hardware read-pointer / BCN kick). Returns
   * bytes submitted or a negative error. */
  virtual int tx_sync(uint8_t ep, uint8_t *buf, size_t len, int timeout_ms) = 0;
  /* Blocking RX delivery loop until should_stop(). buf_size/n_xfers are USB
   * URB-queue tuning; the PCIe ring depth is fixed at transport creation. */
  virtual void rx_loop(int buf_size, int n_xfers,
                       const std::function<void(const uint8_t *, int)> &on_data,
                       const std::function<bool()> &should_stop) = 0;
  /* Single-shot raw RX read (USB bulk-IN); unsupported (-1) on PCIe. */
  virtual int rx_raw(uint8_t *buf, int len, int timeout_ms) {
    (void)buf; (void)len; (void)timeout_ms;
    return -1;
  }
  virtual void clear_halt(uint8_t ep) { (void)ep; }

  /* ---- lifecycle / info ---- */
  /* Pre-power-on HCI programming, re-run per bring-up attempt (rtw88's
   * rtw_hci_setup slot: PCIe TRX buffer-descriptor ring registers; no-op on
   * USB). */
  virtual void hci_setup() {}
  virtual UsbLinkInfo usb_info() const { return {}; }
  virtual TxStats tx_stats() const { return {}; }
};

} /* namespace devourer */
