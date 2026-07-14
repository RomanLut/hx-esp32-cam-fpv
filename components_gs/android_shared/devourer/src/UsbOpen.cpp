#include "UsbOpen.h"

#include "UsbDeviceLock.h"

#if defined(__ANDROID__) || defined(_MSC_VER) || defined(__APPLE__)
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#include <string>

namespace devourer {

int claim_interface_then_reset(libusb_device_handle *handle, int iface,
                               const Logger_t &logger, bool do_reset,
                               std::shared_ptr<UsbDeviceLock> &out_lock,
                               const std::string &lock_dir) {
  if (handle == nullptr)
    return LIBUSB_ERROR_NO_DEVICE;
  libusb_device *dev = libusb_get_device(handle);

  /* Advisory exclusive lock FIRST — before detach / claim / reset — so a second
   * devourer is turned away without disturbing the device at all. This is the
   * gate that actually holds on platforms where the kernel claim below does not
   * arbitrate (WinUSB). Genuine contention -> refuse; a lock-infrastructure
   * failure (e.g. read-only tmpdir) degrades to a warning and we lean on the
   * kernel claim instead. */
  auto lock = std::make_shared<UsbDeviceLock>();
  std::string why;
  switch (lock->try_acquire(dev, &why, lock_dir)) {
  case UsbDeviceLock::Result::Busy:
    logger->error("USB adapter in use — refusing to open ({})", why);
    return LIBUSB_ERROR_BUSY;
  case UsbDeviceLock::Result::Error:
    logger->warn("USB adapter lock unavailable ({}) — leaning on the kernel "
                 "interface claim for exclusivity",
                 why);
    lock.reset();
    break;
  case UsbDeviceLock::Result::Acquired:
    logger->info("USB adapter {} locked for exclusive access", lock->key());
    break;
  }

  if (libusb_kernel_driver_active(handle, iface) == 1)
    libusb_detach_kernel_driver(handle, iface);

  /* Configure before claiming: a chip that was never kernel-configured is in
   * config 0, where claim / bulk I/O fail with ESRCH. No-op if already set. */
  int cfg = 0;
  if (libusb_get_configuration(handle, &cfg) != 0 || cfg != 1)
    libusb_set_configuration(handle, 1);

  /* The kernel's exclusive claim — a redundant second gate on Linux usbfs
   * (reports BUSY); a no-op success on WinUSB, where the lock above is the
   * real guard. */
  int rc = libusb_claim_interface(handle, iface);
  if (rc != 0) {
    if (rc == LIBUSB_ERROR_BUSY)
      logger->error("USB interface {} is BUSY — another process is already "
                    "using this adapter; refusing to open",
                    iface);
    else
      logger->error("libusb_claim_interface({}) failed: {} ({})", iface,
                    libusb_error_name(rc), rc);
    return rc;
  }

  if (do_reset) {
    /* We hold the lock and the interface, so this re-enumeration can't disturb
     * anyone else. If the reset invalidates the handle, report it; otherwise
     * re-assert the claim (the reset may have released it). */
    int rrc = libusb_reset_device(handle);
    if (rrc == LIBUSB_ERROR_NOT_FOUND) {
      logger->error("libusb_reset_device: device re-enumerated, handle is "
                    "stale — close and reopen");
      return rrc;
    }
    rc = libusb_claim_interface(handle, iface);
    if (rc != 0) {
      logger->error("re-claim of interface {} after reset failed: {} ({})",
                    iface, libusb_error_name(rc), rc);
      return rc;
    }
  }

  out_lock = std::move(lock);
  return 0;
}

} // namespace devourer
