#ifndef DEVOURER_USB_DEVICE_LOCK_H
#define DEVOURER_USB_DEVICE_LOCK_H

#include <string>

struct libusb_device;

namespace devourer {

/* Cross-platform advisory *exclusive* lock for a single physical USB adapter,
 * keyed to its bus number + port path (stable across a VID:PID re-enumeration).
 *
 * WHY: nothing in userspace stops two devourer instances from opening and
 * driving the same adapter at once. When that happens the two bring-ups race
 * the chip reset / firmware download / register writes and wedge it (observed:
 * processes stuck in uninterruptible USB I/O that even `kill -9` won't clear).
 * A real OS serialises access to a device node; this gives devourer the same
 * guarantee at the library boundary — the second instance's acquisition fails,
 * so `CreateRtlDevice` refuses instead of racing.
 *
 * LIFETIME: the lock is held for this object's lifetime and released
 * automatically when the owning process exits — normally, via SIGKILL, or on a
 * crash — because the kernel closes the backing fd (POSIX) / handle (Windows)
 * on process death. There are therefore never stale locks to clean up, which is
 * exactly the failure mode a PID-file scheme suffers after `kill -9`.
 *
 * FAIL-OPEN vs FAIL-CLOSED: genuine contention (another live process holds the
 * adapter) returns Busy — the caller should refuse. An *infrastructure* failure
 * (can't create the lock file, e.g. a read-only tmpdir) returns Error — the
 * caller should log and proceed without exclusivity, so a quirky environment
 * never bricks an otherwise-working open. */
class UsbDeviceLock {
public:
  enum class Result {
    Acquired, /* we now hold the exclusive lock */
    Busy,     /* another process holds it — caller should refuse */
    Error,    /* couldn't create/lock the primitive — caller may proceed */
  };

  UsbDeviceLock() = default;
  ~UsbDeviceLock();
  UsbDeviceLock(const UsbDeviceLock &) = delete;
  UsbDeviceLock &operator=(const UsbDeviceLock &) = delete;
  UsbDeviceLock(UsbDeviceLock &&) = delete;
  UsbDeviceLock &operator=(UsbDeviceLock &&) = delete;

  /* Try to take the exclusive lock for the adapter behind `dev`. On a non-null
   * `reason`, a human-readable explanation is written for Busy/Error. Idempotent
   * guard: calling twice on an already-held lock returns Acquired. `lock_dir`
   * is the lock-file directory (empty = "/tmp"; DeviceConfig usb.lock_dir — the
   * demos map TMPDIR onto it); ignored on Windows (named mutex, no file). */
  Result try_acquire(libusb_device *dev, std::string *reason,
                     const std::string &lock_dir = {});

  bool held() const;
  /* The lock key (e.g. "3-1.4" = bus 3, port path 1.4). Empty until acquired. */
  const std::string &key() const { return _key; }

private:
  std::string _key;
#if defined(_WIN32)
  void *_handle = nullptr; /* HANDLE from CreateMutex, kept opaque in the header */
#else
  int _fd = -1;
  std::string _path;
#endif
};

} // namespace devourer

#endif /* DEVOURER_USB_DEVICE_LOCK_H */
