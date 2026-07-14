#ifndef WIFI_DRIVER_H
#define WIFI_DRIVER_H

#include <memory>

#include "DeviceConfig.h"
#include "IRtlDevice.h"
#include "logger.h"

struct libusb_device_handle;
struct libusb_context;

namespace devourer {
class UsbDeviceLock;
class PcieTransport;
}

class WiFiDriver {
  Logger_t _logger;

public:
  explicit WiFiDriver(Logger_t logger);

  /* Constructs the right device for the chip behind `dev_handle`:
   * RtlJaguarDevice for Jaguar wave-1 (8812/8811/8821/8814AU) or
   * RtlJaguar3Device for Jaguar3 (8822CU/8812EU/8822EU). Returns the common
   * IRtlDevice interface. See CreateRtlDevice() for how the family is chosen.
   *
   * `usb_lock` is the exclusive per-adapter lock. Pass the one returned by
   * devourer::claim_interface_then_reset (the recommended open path) so it is
   * held for the device lifetime and not re-acquired. When null (a caller that
   * opened/claimed the handle itself), CreateRtlDevice acquires its own as a
   * best-effort second gate, and returns nullptr if the adapter is already in
   * use.
   *
   * `cfg` is the construction-time configuration (see DeviceConfig.h); the
   * default gives stock behaviour. Fixed for the device's lifetime. */
  std::unique_ptr<IRtlDevice>
  CreateRtlDevice(libusb_device_handle *dev_handle,
                  libusb_context *ctx = nullptr,
                  std::shared_ptr<devourer::UsbDeviceLock> usb_lock = nullptr,
                  const devourer::DeviceConfig &cfg = {});

#if defined(DEVOURER_HAVE_PCIE)
  /* PCIe counterpart (DEVOURER_PCIE=ON, Linux/vfio only). The caller owns
   * vfio, mirroring the USB doctrine: open the transport first —
   * devourer::PcieTransport::Open("0000:01:00.0", logger) is the recommended
   * path (the device must be bound to vfio-pci, see tests/pcie_vfio_bind.sh) —
   * then hand it in here. Chip identity is read from SYS_CFG2 over MMIO;
   * currently only chip-id 0x09 (RTL8821C — the RTL8821CE) is accepted,
   * anything else logs and returns nullptr (same contract as an unsupported
   * chip on USB). No UsbDeviceLock: a vfio device fd is exclusive by
   * construction (a second open fails). */
  std::unique_ptr<IRtlDevice>
  CreateRtlDevicePcie(std::shared_ptr<devourer::PcieTransport> transport,
                      const devourer::DeviceConfig &cfg = {});
#endif
};

#endif /* WIFI_DRIVER_H */
