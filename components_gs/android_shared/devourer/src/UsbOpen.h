#ifndef DEVOURER_USB_OPEN_H
#define DEVOURER_USB_OPEN_H

#include <memory>
#include <string>

#include "logger.h"

struct libusb_device_handle;

namespace devourer {

class UsbDeviceLock;

/* Prepare an already-opened USB handle for use with the OS-friendly ordering —
 * *lock and claim before reset*:
 *
 *   1. take the advisory exclusive UsbDeviceLock (see UsbDeviceLock.h) FIRST,
 *      before touching the device. This is the cross-platform gate: on Windows
 *      (WinUSB) libusb_claim_interface is not a kernel arbitration point and
 *      does not report BUSY, so the lock — not the claim — is what turns a
 *      second devourer away there, and taking it up front means that second
 *      process resets nothing. `out_lock` receives it; the caller must keep it
 *      alive for as long as it drives the adapter (hand it to CreateRtlDevice,
 *      which then does NOT re-acquire);
 *   2. detach an attached kernel driver on `iface`;
 *   3. set configuration 1 (a cold, never-kernel-configured chip sits in config
 *      0, where claim / bulk I/O fail with ESRCH); no-op if already set;
 *   4. libusb_claim_interface(iface) — the kernel's exclusive per-interface lock
 *      (this reports BUSY on Linux usbfs, a redundant second gate);
 *   5. only now, holding both the lock and the interface, and only if `do_reset`,
 *      libusb_reset_device() — safe, since we are the sole owner — then re-claim.
 *
 * Returns 0 on success; a libusb error code otherwise (already logged via
 * `logger`). LIBUSB_ERROR_BUSY means "adapter in use by another process" (from
 * either the lock or the claim) — the caller should close the handle and exit
 * without touching the device. On success `out_lock` holds the exclusive lock
 * (or is null if the lock could only degrade to a warning — e.g. a read-only
 * tmpdir — in which case the kernel claim still guards). `lock_dir` is the
 * lock-file directory (empty = "/tmp"; see UsbDeviceLock::try_acquire). */
int claim_interface_then_reset(libusb_device_handle *handle, int iface,
                               const Logger_t &logger, bool do_reset,
                               std::shared_ptr<UsbDeviceLock> &out_lock,
                               const std::string &lock_dir = {});

} // namespace devourer

#endif /* DEVOURER_USB_OPEN_H */
