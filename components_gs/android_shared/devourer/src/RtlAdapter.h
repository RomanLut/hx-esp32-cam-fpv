#ifndef RTLADAPTER_H
#define RTLADAPTER_H

/* RtlAdapter — the bus-neutral register/frame accessor every HAL component
 * holds BY VALUE (a copyable value type; every copy shares the one transport
 * via shared_ptr, so the transport — and with it the USB device lock or the
 * vfio fds + DMA rings — lives until the last copy dies).
 *
 * The bus specifics live behind IRtlTransport (src/RtlTransport.h): USB =
 * devourer::UsbTransport (libusb vendor-control registers + bulk endpoints),
 * PCIe = devourer::PcieTransport (BAR2 MMIO registers + 88xx DMA rings). The
 * adapter itself carries only bus-neutral chip helpers built on the register
 * plane (EFUSE byte reads, phy_set_bb_reg, the 8812 resets) plus the
 * config-derived knobs, and forwards the frame plane.
 *
 * (Historically this class WAS the USB transport; the old `RtlUsbAdapter`
 * name remains as a deprecated alias in RtlUsbAdapter.h.) */

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#ifdef _WIN32
/* The hal headers below lean on Windows types (BOOLEAN/VOID in rtw_efuse.h)
 * and #define identifiers winuser.h uses as parameter names (bEnable in
 * Hal8812PhyReg.h) — windows.h must be included BEFORE them, project-wide.
 * The pre-seam RtlUsbAdapter.h guaranteed this implicitly by including
 * libusb.h (-> windows.h) first; with libusb gone from this header, make the
 * dependency explicit. */
#include <windows.h>
#endif

#include "DeviceConfig.h"
#include "RtlTransport.h"
#include "TxStats.h"
#include "drv_types.h"
#include "hal_com_reg.h"
#include "logger.h"

struct libusb_device_handle;
struct libusb_context;
namespace devourer {
class UsbDeviceLock;
}

#define rtw_read8 rtw_read<uint8_t>
#define rtw_read16 rtw_read<uint16_t>
#define rtw_read32 rtw_read<uint32_t>

#define rtw_write8 rtw_write<uint8_t>
#define rtw_write16 rtw_write<uint16_t>
#define rtw_write32 rtw_write<uint32_t>

enum TxSele {
  TX_SELE_HQ = 1 << (0), /* High Queue */
  TX_SELE_LQ = 1 << (1), /* Low Queue */
  TX_SELE_NQ = 1 << (2), /* Normal Queue */
  TX_SELE_EQ = 1 << (3), /* Extern Queue */
};

class RtlAdapter {
  std::shared_ptr<devourer::IRtlTransport> _transport;
  Logger_t _logger;

  /* USB-descriptor-derived facts (defaults on PCIe), mirrored at construction
   * for the HALs that consume them (Jaguar1 queue mapping, RX-agg sizing). */
  devourer::UsbLinkInfo _usb;

  /* From DeviceConfig (see there for semantics). Plain values — RtlAdapter
   * is a copyable value type, every copy carries them. */
  bool _log_writes = false;               /* debug.log_writes */
  std::optional<uint8_t> _tx_ep_override; /* tx.ep */
  unsigned _tx_timeout_ms = 500;          /* tx.timeout_ms */

public:
  /* USB: wraps the handle in a devourer::UsbTransport. The signature is the
   * historic one so USB construction sites are unchanged. */
  RtlAdapter(libusb_device_handle *dev_handle, Logger_t logger,
             libusb_context *ctx = nullptr,
             std::shared_ptr<devourer::UsbDeviceLock> usb_lock = nullptr,
             const devourer::DeviceConfig &cfg = {});
  /* Any transport (the PCIe factory path; also the seam for future buses). */
  RtlAdapter(std::shared_ptr<devourer::IRtlTransport> transport,
             Logger_t logger, const devourer::DeviceConfig &cfg = {});

  bool is_usb() const { return _transport->is_usb(); }
  /* Pre-power-on HCI programming (rtw88 rtw_hci_setup slot): PCIe TRX ring
   * registers; no-op on USB. Call per bring-up attempt, before power-on. */
  void hci_setup() { _transport->hci_setup(); }

  /* Kernel-style async RX: keep n_urbs concurrent bulk-IN transfers in flight
   * (USB) or reap the RX buffer-descriptor ring (PCIe), invoking
   * on_data(buf,len) per delivery until `stop`. */
  void bulk_read_async_loop(int buf_size, int n_urbs,
                            const std::function<void(const uint8_t *, int)> &on_data,
                            const std::function<bool()> &should_stop) {
    _transport->rx_loop(buf_size, n_urbs, on_data, should_stop);
  }
  /* Convenience overload for callers whose stop condition is a single flag. */
  void bulk_read_async_loop(int buf_size, int n_urbs,
                            const std::function<void(const uint8_t *, int)> &on_data,
                            const volatile bool &stop) {
    bulk_read_async_loop(buf_size, n_urbs, on_data,
                         [&stop]() -> bool { return stop; });
  }

  uint16_t idVendor() const { return _usb.vid; }
  uint16_t idProduct() const { return _usb.pid; }
  int speed() const { return _usb.speed; }
  /* First discovered bulk-OUT endpoint (HQ-equivalent), or 0x02 if none were
   * discovered. Used by the DLFW rsvd-page download; ignored by the PCIe
   * transport (its ring is chosen from the descriptor QSEL). */
  uint8_t first_bulk_out_ep() const {
    return _usb.bulk_out_eps.empty() ? 0x02 : _usb.bulk_out_eps[0];
  }

  bool AutoloadFailFlag = false;
  bool EepromOrEfuse = false;
  uint8_t OutEpQueueSel = 0;
  uint8_t OutEpNumber = 0;
  uint8_t rxagg_usb_size = 0;
  uint8_t rxagg_usb_timeout = 0;

  bool send_packet(uint8_t *packet, size_t length);
  /* Synchronous TX that blocks until completion or timeout. Returns bytes
   * submitted, or negative on error. */
  int bulk_send_sync(uint8_t *packet, size_t length, int timeout_ms) {
    return bulk_send_sync_ep(0x02, packet, length, timeout_ms);
  }
  int bulk_send_sync_ep(uint8_t ep, uint8_t *packet, size_t length,
                        int timeout_ms) {
    return _transport->tx_sync(ep, packet, length, timeout_ms);
  }
  void bulk_clear_halt(uint8_t ep) { _transport->clear_halt(ep); }

  /* Snapshot of the TX submission counters (see TxStats.h). */
  devourer::TxStats GetTxStats() const { return _transport->tx_stats(); }

  /* Raw single-shot RX read (USB bulk-IN; -1 on PCIe — its RX is the ring
   * reap in bulk_read_async_loop). */
  int bulk_read_raw(uint8_t *buf, int len, int timeout_ms) {
    return _transport->rx_raw(buf, len, timeout_ms);
  }

  uint8_t efuse_OneByteRead(uint16_t addr, uint8_t *data);
  void phy_set_bb_reg(uint16_t regAddr, uint32_t bitMask, uint32_t data);

  template <typename T> T rtw_read(uint16_t reg_num) {
    if constexpr (sizeof(T) == 1)
      return _transport->read8(reg_num);
    else if constexpr (sizeof(T) == 2)
      return _transport->read16(reg_num);
    else
      return _transport->read32(reg_num);
  }

  template <typename T> bool rtw_write(uint16_t reg_num, T value) {
    /* debug.log_writes: emit every register write as a debug.wreg event
     * (addr/width/val mirror tests/decode_wseq.py's tuple) so devourer's
     * bring-up write set can be diffed against the kernel golden — on either
     * bus. */
    if (_log_writes)
      devourer::Ev(_logger->events(), "debug.wreg")
          .hexf("addr", reg_num, 4)
          .f("width", sizeof(T))
          .hexf("val", (unsigned long long)value, (int)(sizeof(T) * 2));
    if constexpr (sizeof(T) == 1)
      return _transport->write8(reg_num, value);
    else if constexpr (sizeof(T) == 2)
      return _transport->write16(reg_num, value);
    else
      return _transport->write32(reg_num, value);
  }

  bool WriteBytes(uint16_t reg_num, uint8_t *ptr, size_t size) {
    return _transport->write_bytes(reg_num, ptr, size);
  }

  void rtl8812au_hw_reset();
  void _8051Reset8812();
  void ReadEFuseByte(uint16_t _offset, uint8_t *pbuf);

private:
  void init_from_transport(const devourer::DeviceConfig &cfg);
  void PHY_SetBBReg8812(uint16_t regAddr, uint32_t bitMask,
                        uint32_t dataOriginal);
};

#endif /* RTLADAPTER_H */
