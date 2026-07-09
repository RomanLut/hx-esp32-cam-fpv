#include "UsbTransport.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "Event.h"
#include "UsbDeviceLock.h"

namespace devourer {

namespace {
/* Shared state for the async RX URB queue. */
struct AsyncRxShared {
  const std::function<void(const uint8_t *, int)> *cb;
  const std::function<bool()> *stop;
  /* Atomic: in co-running (TX + self-capture RX) mode both the RX loop's own
   * event pump and the TX event loop may run this callback, so `active` is
   * written from the pump thread while the loop below reads it. */
  std::atomic<int> active{0};
};
extern "C" void LIBUSB_CALL devourer_rx_cb(libusb_transfer *t) {
  auto *s = static_cast<AsyncRxShared *>(t->user_data);
  if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length > 0)
    (*s->cb)(t->buffer, t->actual_length);
  bool resubmit = !(*s->stop)() && (t->status == LIBUSB_TRANSFER_COMPLETED ||
                                    t->status == LIBUSB_TRANSFER_TIMED_OUT);
  if (resubmit && libusb_submit_transfer(t) == 0)
    return;
  s->active--; /* not resubmitted -> this URB is done */
}
} // namespace

UsbTransport::UsbTransport(libusb_device_handle *dev_handle, Logger_t logger,
                           libusb_context *ctx,
                           std::shared_ptr<devourer::UsbDeviceLock> usb_lock)
    : _dev_handle{dev_handle}, _ctx{ctx}, _logger{std::move(logger)},
      _usb_lock{std::move(usb_lock)} {
  libusb_device_descriptor desc{};
  if (libusb_get_device_descriptor(libusb_get_device(_dev_handle), &desc) ==
      LIBUSB_SUCCESS) {
    _info.vid = desc.idVendor;
    _info.pid = desc.idProduct;
    _logger->info("USB device {:04x}:{:04x}", _info.vid, _info.pid);
  }
  discover_endpoints();
  _info.valid = true;
}

bool UsbTransport::write_bytes(uint16_t reg_num, const uint8_t *ptr, size_t n) {
  return libusb_control_transfer(_dev_handle, REALTEK_USB_VENQT_WRITE, 5,
                                 reg_num, 0, const_cast<uint8_t *>(ptr), n,
                                 USB_TIMEOUT) == static_cast<int>(n);
}

void UsbTransport::rx_loop(
    int buf_size, int n_urbs,
    const std::function<void(const uint8_t *, int)> &on_data,
    const std::function<bool()> &should_stop) {
  AsyncRxShared sh{&on_data, &should_stop};
  std::vector<libusb_transfer *> xfers;
  std::vector<std::vector<uint8_t>> bufs(n_urbs,
                                         std::vector<uint8_t>(buf_size));
  for (int i = 0; i < n_urbs; i++) {
    libusb_transfer *t = libusb_alloc_transfer(0);
    /* timeout=0 (infinite): a persistent RX ring — each URB stays posted until
     * a frame arrives (COMPLETED), the queue is torn down (CANCELLED, below),
     * or the device errors. This is the kernel rtw88 RX-URB idiom. A finite
     * timeout here would fire once per idle interval on a quiet channel, and
     * libusb's darwin backend logs every LIBUSB_TRANSFER_TIMED_OUT at WARNING
     * level — a continuous "transfer error: timed out" flood that carries no
     * information (RX is healthy; bulk-IN is simply idle) and can bloat a long
     * capture's stderr. The devourer_rx_cb resubmit-on-TIMED_OUT branch is kept
     * as a defensive no-op should a backend still surface a timeout. */
    libusb_fill_bulk_transfer(t, _dev_handle, _info.bulk_in_ep, bufs[i].data(),
                              buf_size, devourer_rx_cb, &sh, 0);
    if (libusb_submit_transfer(t) == 0) {
      xfers.push_back(t);
      sh.active++;
    } else {
      libusb_free_transfer(t);
    }
  }
  _logger->info("RX: async queue of {} URBs submitted", sh.active.load());
  while (!should_stop() && sh.active > 0) {
    struct timeval tv {0, 100000};
    libusb_handle_events_timeout_completed(_ctx, &tv, nullptr);
  }
  for (auto *t : xfers)
    libusb_cancel_transfer(t);
  while (sh.active > 0) {
    struct timeval tv {0, 100000};
    libusb_handle_events_timeout_completed(_ctx, &tv, nullptr);
  }
  for (auto *t : xfers)
    libusb_free_transfer(t);
}

int UsbTransport::rx_raw(uint8_t *buf, int len, int timeout_ms) {
  int actual = 0;
  int rc = libusb_bulk_transfer(_dev_handle, _info.bulk_in_ep, buf, len,
                                &actual, timeout_ms);
  return rc < 0 ? rc : actual;
}

const char *UsbTransport::speed_str() const {
  switch (_info.speed) {
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

void UsbTransport::discover_endpoints() {
  libusb_device *dev = libusb_get_device(_dev_handle);
  _info.speed = libusb_get_device_speed(dev);
  _logger->info("Running USB bus at {}", speed_str());

  libusb_device_descriptor desc;
  int ret = libusb_get_device_descriptor(dev, &desc);
  if (ret < 0) {
    return;
  }

  for (uint8_t k = 0; k < desc.bNumConfigurations; k++) {
    libusb_config_descriptor *config;
    ret = libusb_get_config_descriptor(dev, k, &config);
    if (LIBUSB_SUCCESS != ret) {
      continue;
    }

    if (!config->bNumInterfaces) {
      continue;
    }
    const libusb_interface *interface = &config->interface[0];

    if (!interface->altsetting) {
      continue;
    }
    const libusb_interface_descriptor *interface_desc =
        &interface->altsetting[0];

    bool found_bulk_in = false;
    for (uint8_t j = 0; j < interface_desc->bNumEndpoints; j++) {
      const libusb_endpoint_descriptor *endpoint = &interface_desc->endpoint[j];
      uint8_t endPointAddr = endpoint->bEndpointAddress;
      const bool is_bulk = (endpoint->bmAttributes & 0b11) ==
                           LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK;
      _logger->info("endpoint[{}]: addr=0x{:X} attrs=0x{:X} bulk={} in={}",
                    (int)j, (int)endPointAddr, (int)endpoint->bmAttributes,
                    is_bulk ? 1 : 0,
                    (endPointAddr & LIBUSB_ENDPOINT_IN) ? 1 : 0);

      if (is_bulk && !(endPointAddr & LIBUSB_ENDPOINT_IN)) {
        _info.bulk_out_eps.push_back(endPointAddr);
      }
      /* First bulk IN endpoint wins. 8812AU/8814AU expose 0x81; 8821AU's
       * descriptor offers a different IN endpoint, so libusb's
       * submit_bulk_transfer to 0x81 would return "endpoint not found on any
       * open interface". Capture whatever IN endpoint the chip actually
       * exposes and use it in rx_raw()/rx_loop(). */
      if (is_bulk && (endPointAddr & LIBUSB_ENDPOINT_IN) && !found_bulk_in) {
        _info.bulk_in_ep = endPointAddr;
        found_bulk_in = true;
        _logger->info("selected bulk IN endpoint: 0x{:X}",
                      (int)_info.bulk_in_ep);
      }
    }
    if (!_info.bulk_out_eps.empty()) {
      std::string ep_list;
      for (auto ep : _info.bulk_out_eps) {
        char buf[8];
        snprintf(buf, sizeof(buf), "0x%02X ", ep);
        ep_list += buf;
      }
      _logger->info("bulk OUT endpoints: {}", ep_list);
    }
    /* Clear any HALT state on the bulk IN endpoint. The fwdl sequence and
     * USB reset can leave the IN EP in a stalled state from the chip side;
     * without clear_halt the chip's USB engine would never push RX bytes
     * even though the host's libusb_bulk_transfer succeeds at submission. */
    if (found_bulk_in) {
      int hr = libusb_clear_halt(_dev_handle, _info.bulk_in_ep);
      _logger->info("libusb_clear_halt(bulk IN 0x{:X}) rc={}",
                    (int)_info.bulk_in_ep, hr);
    }

    libusb_free_config_descriptor(config);
    break;
  }
}

void UsbTransport::transfer_callback(struct libusb_transfer *transfer) {
  auto *self = static_cast<UsbTransport *>(transfer->user_data);
  if (transfer->status == LIBUSB_TRANSFER_COMPLETED &&
      transfer->actual_length == transfer->length) {
    DVR_DEBUG(self->_logger, "Packet sent successfully, length: {}",
              transfer->length);
  } else {
    /* Flag the bulk-OUT as possibly halted so the next tx_async (on the TX
     * thread) re-clear_halts it before the following frame. */
    self->_tx_wedged.store(true, std::memory_order_relaxed);
    /* Async completion failure — the real drop for the async TX path. A
     * TIMED_OUT status is the FIFO-full back-pressure (congestion); anything
     * else is a hard error. Record the negated status as the rc. */
    self->_tx_failed.fetch_add(1, std::memory_order_relaxed);
    self->_tx_last_rc.store(-transfer->status, std::memory_order_relaxed);
    self->_tx_last_timeout.store(
        transfer->status == LIBUSB_TRANSFER_TIMED_OUT,
        std::memory_order_relaxed);
    self->_logger->error("Failed to send packet, status: {}, actual length: {}",
                         transfer->status, transfer->actual_length);
    /* machine-readable mirror of the failure (tests/regress.py keys on it) */
    devourer::Ev(self->_logger->events(), "tx.fail")
        .f("status", (long long)transfer->status)
        .f("actual_len", transfer->actual_length)
        .f("timeout", transfer->status == LIBUSB_TRANSFER_TIMED_OUT);
  }
  libusb_free_transfer(transfer);
}

bool UsbTransport::tx_async(uint8_t tx_ep, uint8_t *packet, size_t length,
                            unsigned timeout_ms) {
  libusb_transfer *transfer = libusb_alloc_transfer(0);
  if (!transfer) {
    _logger->error("Failed to allocate transfer");
    return false;
  }

  /* Recover a bulk-OUT that a prior async TX wedged (TIMED_OUT / stall). Only
   * the first send used to clear_halt; a mid-stream stall (e.g. hardware NDP
   * generation on some xhci hosts) then stayed wedged forever. */
  if (_tx_wedged.exchange(false)) {
    int hr = libusb_clear_halt(_dev_handle, tx_ep);
    _logger->info("TX EP 0x{:02X} re-clear_halt after wedge rc={}", (int)tx_ep,
                  hr);
  }

  /* On the FIRST send only, dump the bulk-OUT bytes to compare against
   * the OOT-driver wire trace. */
  static bool first_pkt_dump = true;
  if (first_pkt_dump) {
    first_pkt_dump = false;
    /* Clear any HALT state on the TX EP. The fwdl process can leave the
     * TX EP in a stalled state from the chip side; without clear_halt the
     * USB controller would NAK every subsequent bulk OUT URB. */
    int chr = libusb_clear_halt(_dev_handle, tx_ep);
    _logger->info("libusb_clear_halt(EP 0x{:02X}) rc={}", (int)tx_ep, chr);
    size_t dump_len = std::min<size_t>(length, 64);
    char hex[64 * 2 + 1] = {0};
    for (size_t k = 0; k < dump_len; ++k) {
      static const char hd[] = "0123456789abcdef";
      hex[2 * k] = hd[packet[k] >> 4];
      hex[2 * k + 1] = hd[packet[k] & 0xF];
    }
    _logger->info("first TX bulk-OUT len={} bytes: {}", length, hex);
  }

  /* On the FIRST send only, dump chip state via vendor reads. Surfaces any
   * register clobber between init-end and first TX (e.g. SetMonitorChannel
   * could be resetting REG_CR or related). */
  static bool first_dump = true;
  if (first_dump) {
    first_dump = false;
    uint16_t cr = ctrl_read<uint16_t>(0x0100);
    uint8_t txpause = ctrl_read<uint8_t>(0x0522);
    uint32_t txdma_off_chk = ctrl_read<uint32_t>(0x020C);
    uint32_t fwhw_txq = ctrl_read<uint32_t>(0x0420);
    uint32_t mcufwdl = ctrl_read<uint32_t>(0x0080);
    uint32_t hci_susp = ctrl_read<uint32_t>(0xFE10); /* USB suspend ctrl */
    _logger->info("pre-1st-TX: CR=0x{:04x} TXPAUSE=0x{:02x} TXDMA_OFFC=0x{:08x}",
                  cr, txpause, txdma_off_chk);
    _logger->info("pre-1st-TX: FWHW_TXQ=0x{:08x} MCUFWDL=0x{:08x} HCIPWR=0x{:08x}",
                  fwhw_txq, mcufwdl, hci_susp);
  }

  libusb_fill_bulk_transfer(transfer, _dev_handle, tx_ep, packet, length,
                            &UsbTransport::transfer_callback, (void *)this,
                            timeout_ms);
  /* Upstream OOT (rtl8814a/usb/rtl8814au_xmit.c) sets URB_ZERO_PACKET on
   * every TX URB. libusb equivalent: LIBUSB_TRANSFER_ADD_ZERO_PACKET.
   * Without it the chip's SuperSpeed bulk OUT controller can wait
   * indefinitely for transfer-end signaling and NAK every URB until libusb
   * cancels — matches the usbmon trace we captured: 6977 submitted URBs,
   * every completion with status=-2 (ENOENT/cancelled), data_len=0. */
  transfer->flags |= LIBUSB_TRANSFER_ADD_ZERO_PACKET;
  /* Count the submission here; async completion (incl. TIMED_OUT) is counted
   * in transfer_callback, a submit error just below. */
  _tx_submitted.fetch_add(1, std::memory_order_relaxed);
  int rc = libusb_submit_transfer(transfer);
  if (rc == LIBUSB_SUCCESS) {
    DVR_DEBUG(_logger, "Packet sent successfully, length: {}", length);
    return true;
  }
  _tx_failed.fetch_add(1, std::memory_order_relaxed);
  _tx_last_rc.store(rc, std::memory_order_relaxed);
  _tx_last_timeout.store(rc == LIBUSB_ERROR_TIMEOUT, std::memory_order_relaxed);
  _logger->error("Failed to send packet, error code: {}", rc);
  devourer::Ev(_logger->events(), "tx.fail")
      .f("rc", rc)
      .f("timeout", rc == LIBUSB_ERROR_TIMEOUT);
  libusb_free_transfer(transfer);
  return false;
}

int UsbTransport::tx_sync(uint8_t ep, uint8_t *packet, size_t length,
                          int timeout_ms) {
  /* No libusb_clear_halt here. rtw88_8814au's usbmon shows the first bulk
   * OUT is preceded by 0 CLEAR_FEATUREs; later CLEAR_FEATUREs happen during
   * normal TX-queue operation, not the per-send hot path. Resetting the
   * data toggle bit corrupts the chip's state machine. */
  int actual = 0;
  _tx_submitted.fetch_add(1, std::memory_order_relaxed);
  int rc = libusb_bulk_transfer(_dev_handle, ep, packet,
                                static_cast<int>(length), &actual, timeout_ms);
  if (rc != LIBUSB_SUCCESS) {
    _tx_failed.fetch_add(1, std::memory_order_relaxed);
    _tx_last_rc.store(rc, std::memory_order_relaxed);
    _tx_last_timeout.store(rc == LIBUSB_ERROR_TIMEOUT,
                           std::memory_order_relaxed);
    _logger->error("bulk_send EP {} FAIL rc={} got {}/{}", (int)ep, rc, actual,
                   (int)length);
    return rc;
  }
  _logger->info("bulk_send EP {} OK {} bytes", (int)ep, actual);
  return actual;
}

TxStats UsbTransport::tx_stats() const {
  TxStats s;
  s.submitted = _tx_submitted.load(std::memory_order_relaxed);
  s.failed = _tx_failed.load(std::memory_order_relaxed);
  s.last_error_rc = _tx_last_rc.load(std::memory_order_relaxed);
  s.last_was_timeout = _tx_last_timeout.load(std::memory_order_relaxed);
  return s;
}

} /* namespace devourer */
