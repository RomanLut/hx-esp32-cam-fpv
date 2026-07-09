#include "WiFiDriver.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <libusb.h>

#include "RtlAdapter.h"
#include "UsbDeviceLock.h"
#include "UsbTransport.h" /* REALTEK_USB_VENQT_READ / USB_TIMEOUT for read_chip_id */
#if defined(DEVOURER_HAVE_PCIE)
#include "PcieTransport.h"
#endif
#if defined(DEVOURER_HAVE_JAGUAR1)
#include "jaguar1/RtlJaguarDevice.h"
#endif
#if defined(DEVOURER_HAVE_JAGUAR2)
#include "jaguar2/RtlJaguar2Device.h"
#endif
#if defined(DEVOURER_HAVE_JAGUAR3)
#include "jaguar3/RtlJaguar3Device.h"
#endif

namespace {

/* Hardware chip-id at REG_SYS_CFG2 (0x00FC), read with a single vendor control
 * transfer (no full RtlAdapter, so the Jaguar1 construction path is unchanged
 * and only one extra control read is added). Port of halmac get_chip_info
 * (chip_id = REG_READ_8(REG_SYS_CFG2)):
 *   0x04 / 0x05 / 0x08 = 8812A / 8821A / 8814A                -> Jaguar1
 *   0x09 = RTL8821C (RTL8811CU / RTL8821CU, 1T1R)             -> Jaguar2
 *   0x0a = RTL8822B (RTL8822BU, 2T2R)                         -> Jaguar2
 *   0x13 = RTL8822C, 0x17 = RTL8822E (RTL8812EU / RTL8822EU)  -> Jaguar3
 * The chip-id (not the USB PID) is authoritative because the rtl8822e RTL8812EU
 * shares PID 0x8812 with the Jaguar1 RTL8812AU. Returns 0 on a failed read,
 * which falls through to the Jaguar1 path. (8821C = 0x09 hardware-verified on a
 * CF-811AC; it is a HalMAC/phydm Jaguar2 chip, NOT the page-write Jaguar1 the
 * "8821C" name might suggest — routing it to Jaguar1 would fail at DLFW.) */
uint8_t read_chip_id(libusb_device_handle *dev_handle) {
  uint8_t id = 0;
  libusb_control_transfer(dev_handle, REALTEK_USB_VENQT_READ, 5, 0x00FC, 0, &id,
                          sizeof(id), USB_TIMEOUT);
  return id;
}

/* RTL8822B (Jaguar2) SYS_CFG2 chip-id. Read off a TP-Link Archer T3U
 * (RTL8822BU): REG_SYS_CFG2 (0x00FC) reads 0x0a in steady state (and in the
 * demo's post-libusb_reset path), with a transient 0x50 for a brief window
 * right after a cold VBUS power-cycle before the chip settles. Accept both.
 * Neither collides with the Jaguar1 (0x04/05/08/09) or Jaguar3 (0x13/0x17)
 * ids. (SYS_CFG1's low bytes are volatile, so 0xFC is the dispatch signal.) */
constexpr uint8_t kChipId8822B = 0x0a;
constexpr uint8_t kChipId8822B_cold = 0x50;
bool is_8822b_chip_id(uint8_t id) {
  return id == kChipId8822B || id == kChipId8822B_cold;
}

/* RTL8821C (Jaguar2, 1T1R) SYS_CFG2 chip-id, hardware-verified on a CF-811AC
 * (RTL8811CU, 0bda:c811): 0x00FC reads 0x09 in steady state. Does not collide
 * with the Jaguar1 ids (0x04/05/08) it used to be lumped with, nor 8822B (0x0a)
 * or Jaguar3 (0x13/0x17). */
constexpr uint8_t kChipId8821C = 0x09;

} /* namespace */

WiFiDriver::WiFiDriver(Logger_t logger) : _logger{std::move(logger)} {}

std::unique_ptr<IRtlDevice>
WiFiDriver::CreateRtlDevice(libusb_device_handle *dev_handle,
                            libusb_context *ctx,
                            std::shared_ptr<devourer::UsbDeviceLock> usb_lock,
                            const devourer::DeviceConfig &cfg) {
  uint16_t pid = 0;
  libusb_device *dev = libusb_get_device(dev_handle);
  if (dev != nullptr) {
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(dev, &desc) == 0)
      pid = desc.idProduct;
  }

  /* Exclusive per-adapter USB lock (see UsbDeviceLock.h). The recommended open
   * path (devourer::claim_interface_then_reset) already took this lock BEFORE
   * the reset and hands it in here — in that case we must NOT re-acquire (a
   * same-process flock on the same file would self-conflict); we just hold it
   * for the device lifetime. A caller that opened/claimed the handle itself
   * passes null, so acquire our own here as a best-effort second gate: genuine
   * contention -> refuse (return nullptr, same contract as an unsupported chip);
   * a lock-infrastructure error degrades to a warning. Either way the lock rides
   * into the RtlAdapter below and is released at device destruction. */
  if (!usb_lock) {
    auto lock = std::make_shared<devourer::UsbDeviceLock>();
    std::string lock_why;
    switch (lock->try_acquire(dev, &lock_why, cfg.usb.lock_dir)) {
    case devourer::UsbDeviceLock::Result::Busy:
      _logger->error("USB adapter in use — refusing to open ({})", lock_why);
      return nullptr;
    case devourer::UsbDeviceLock::Result::Error:
      _logger->warn("USB adapter lock unavailable ({}) — proceeding without "
                    "exclusive access",
                    lock_why);
      break;
    case devourer::UsbDeviceLock::Result::Acquired:
      _logger->info("USB adapter {} locked for exclusive access", lock->key());
      usb_lock = std::move(lock);
      break;
    }
  }

  uint8_t chip_id = read_chip_id(dev_handle);
  if (is_8822b_chip_id(chip_id)) {
#if defined(DEVOURER_HAVE_JAGUAR2)
    _logger->info("Creating RtlJaguar2Device (PID 0x{:04x}, chip-id 0x{:02x})",
                  pid, chip_id);
    return std::make_unique<RtlJaguar2Device>(
        RtlAdapter(dev_handle, _logger, ctx, usb_lock, cfg), _logger,
        jaguar2::ChipVariant::C8822B, cfg);
#else
    _logger->error("RTL8822B (chip-id 0x{:02x}) detected but Jaguar2 support "
                   "not compiled in (DEVOURER_JAGUAR2=OFF)",
                   chip_id);
    return nullptr;
#endif
  }

  if (chip_id == kChipId8821C) {
#if defined(DEVOURER_HAVE_JAGUAR2_8821C)
    _logger->info("Creating RtlJaguar2Device C8821C (PID 0x{:04x}, chip-id "
                  "0x{:02x})",
                  pid, chip_id);
    return std::make_unique<RtlJaguar2Device>(
        RtlAdapter(dev_handle, _logger, ctx, usb_lock, cfg), _logger,
        jaguar2::ChipVariant::C8821C, cfg);
#else
    _logger->error("RTL8821C (chip-id 0x09) detected but 8821C support not "
                   "compiled in (DEVOURER_JAGUAR2_8821C=OFF)");
    return nullptr;
#endif
  }

  if (chip_id == 0x13 || chip_id == 0x17) {
#if defined(DEVOURER_HAVE_JAGUAR3)
    /* Reject the variant whose support wasn't compiled in (0x13 = 8822C,
     * 0x17 = 8822E) rather than construct a device whose PHY-table /
     * calibration dispatchers were built out. */
#if !defined(DEVOURER_HAVE_JAGUAR3_8822C)
    if (chip_id == 0x13) {
      _logger->error("RTL8822C (chip-id 0x13) support not compiled in "
                     "(DEVOURER_JAGUAR3_8822C=OFF)");
      return nullptr;
    }
#endif
#if !defined(DEVOURER_HAVE_JAGUAR3_8822E)
    if (chip_id == 0x17) {
      _logger->error("RTL8822E (chip-id 0x17) support not compiled in "
                     "(DEVOURER_JAGUAR3_8822E=OFF)");
      return nullptr;
    }
#endif
    auto variant = chip_id == 0x17 ? jaguar3::ChipVariant::C8822E
                                   : jaguar3::ChipVariant::C8822C;
    _logger->info("Creating RtlJaguar3Device (PID 0x{:04x}, chip-id 0x{:02x})",
                  pid, chip_id);
    return std::make_unique<RtlJaguar3Device>(
        RtlAdapter(dev_handle, _logger, ctx, usb_lock, cfg), _logger, variant,
        cfg);
#else
    _logger->error("Jaguar3 chip (chip-id 0x{:02x}) detected but Jaguar3 "
                   "support not compiled in",
                   chip_id);
    return nullptr;
#endif
  }

#if defined(DEVOURER_HAVE_JAGUAR1)
  _logger->info("Creating RtlJaguarDevice (PID 0x{:04x}, chip-id 0x{:02x})", pid,
                chip_id);
  /* Pass ctx so RtlAdapter::_ctx is set: the Jaguar1 RX loop now drives an
   * async URB queue (bulk_read_async_loop) whose event pump needs the same
   * libusb context the handle was opened on (as Jaguar2/3 already do above). */
  return std::make_unique<RtlJaguarDevice>(
      RtlAdapter(dev_handle, _logger, ctx, usb_lock, cfg), _logger, cfg);
#else
  _logger->error("Jaguar1 chip (PID 0x{:04x}, chip-id 0x{:02x}) detected but "
                 "Jaguar1 support not compiled in",
                 pid, chip_id);
  return nullptr;
#endif
}

#if defined(DEVOURER_HAVE_PCIE)
std::unique_ptr<IRtlDevice> WiFiDriver::CreateRtlDevicePcie(
    std::shared_ptr<devourer::PcieTransport> transport,
    const devourer::DeviceConfig &cfg) {
  if (!transport) {
    _logger->error("CreateRtlDevicePcie: null transport");
    return nullptr;
  }
  /* Chip identity from SYS_CFG2 (0x00FC) over BAR2 MMIO — the same dispatch
   * signal the USB factory reads via a vendor control transfer. */
  const uint8_t chip_id =
      *reinterpret_cast<volatile uint8_t *>(transport->mmio() + 0x00FC);

  if (chip_id == kChipId8821C) {
#if defined(DEVOURER_HAVE_JAGUAR2_8821C)
    _logger->info("Creating RtlJaguar2Device C8821C over PCIe ({}; chip-id "
                  "0x{:02x})",
                  transport->bdf(), chip_id);
    return std::make_unique<RtlJaguar2Device>(
        RtlAdapter(std::move(transport), _logger, cfg), _logger,
        jaguar2::ChipVariant::C8821C, cfg);
#else
    _logger->error("RTL8821C[E] (chip-id 0x09) detected but 8821C support not "
                   "compiled in (DEVOURER_JAGUAR2_8821C=OFF)");
    return nullptr;
#endif
  }

  _logger->error("PCIe chip-id 0x{:02x} at {} is not supported over PCIe "
                 "(only RTL8821C[E], chip-id 0x09, for now)",
                 chip_id, transport->bdf());
  return nullptr;
}
#endif /* DEVOURER_HAVE_PCIE */
