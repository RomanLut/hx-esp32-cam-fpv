#include "UsbDeviceLock.h"

#if defined(__ANDROID__) || defined(_MSC_VER) || defined(__APPLE__)
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#include <cstdint>
#include <string>

namespace devourer {

namespace {
/* Stable identity of the physical adapter: bus number + USB port path, e.g.
 * "3-1.4" (bus 3, hub-port chain 1 -> 4). The port path is what a real OS keys a
 * device node to; it survives a VID:PID re-enumeration (unlike the device
 * address, which the kernel reassigns on every reset). Falls back to the device
 * address only when the backend can't report a port path. */
std::string device_key(libusb_device *dev) {
  if (dev == nullptr)
    return "unknown";
  std::string key = std::to_string(libusb_get_bus_number(dev));
  uint8_t ports[8] = {0};
  int pc = libusb_get_port_numbers(dev, ports, sizeof(ports));
  if (pc > 0) {
    for (int i = 0; i < pc; ++i)
      key += (i == 0 ? "-" : ".") + std::to_string(ports[i]);
  } else {
    key += "-a" + std::to_string(libusb_get_device_address(dev));
  }
  return key;
}
} // namespace

} // namespace devourer

#if defined(_WIN32)
/* ------------------------------------------------------------------ Windows */
#include <windows.h>

namespace devourer {

UsbDeviceLock::Result UsbDeviceLock::try_acquire(libusb_device *dev,
                                                 std::string *reason,
                                                 const std::string &) {
  if (held())
    return Result::Acquired;
  _key = device_key(dev);
  /* Named mutex in the session-local namespace (no "Global\\" prefix — one user
   * session's devourer instances are what must not collide). A named mutex is
   * released when the owning process exits; a crash leaves it ABANDONED, which
   * the next waiter still acquires — so no stale lock either way. */
  const std::string name = "devourer-usb-" + _key;
  _handle = ::CreateMutexA(nullptr, FALSE, name.c_str());
  if (_handle == nullptr) {
    if (reason != nullptr)
      *reason = "cannot create lock mutex for adapter " + _key;
    return Result::Error;
  }
  DWORD w = ::WaitForSingleObject(static_cast<HANDLE>(_handle), 0);
  if (w == WAIT_TIMEOUT) {
    ::CloseHandle(static_cast<HANDLE>(_handle));
    _handle = nullptr;
    if (reason != nullptr)
      *reason = "adapter " + _key +
                " is already in use by another devourer process";
    return Result::Busy;
  }
  /* WAIT_OBJECT_0 (clean) or WAIT_ABANDONED (prior owner crashed) -> we own it */
  return Result::Acquired;
}

bool UsbDeviceLock::held() const { return _handle != nullptr; }

UsbDeviceLock::~UsbDeviceLock() {
  if (_handle != nullptr) {
    ::ReleaseMutex(static_cast<HANDLE>(_handle));
    ::CloseHandle(static_cast<HANDLE>(_handle));
    _handle = nullptr;
  }
}

} // namespace devourer

#else
/* -------------------------------------------------------------------- POSIX */
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace devourer {

UsbDeviceLock::Result UsbDeviceLock::try_acquire(libusb_device *dev,
                                                 std::string *reason,
                                                 const std::string &lock_dir) {
  if (held())
    return Result::Acquired;
  _key = device_key(dev);

  std::string dir = lock_dir.empty() ? std::string("/tmp") : lock_dir;
  while (!dir.empty() && dir.back() == '/')
    dir.pop_back();
  _path = dir + "/devourer-usb-" + _key + ".lock";

  /* 0666 so a second user on the same host can still open O_RDWR and flock the
   * same inode (cross-user contention on one adapter must still be caught);
   * O_NOFOLLOW so a pre-planted symlink in the world-writable tmpdir can't
   * redirect the open. */
  _fd = ::open(_path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0666);
  if (_fd < 0) {
    if (reason != nullptr)
      *reason = "cannot open lock file " + _path;
    return Result::Error;
  }
  /* Non-blocking exclusive advisory lock. flock is tied to the open file
   * description and is dropped by the kernel when the fd is closed — including
   * the implicit close on process exit / SIGKILL — so it self-heals. */
  if (::flock(_fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(_fd);
    _fd = -1;
    if (reason != nullptr)
      *reason = "adapter " + _key +
                " is already in use by another devourer process";
    return Result::Busy;
  }
  /* Best-effort: record our pid in the file for a human debugging a refusal.
   * Purely diagnostic — the lock is the flock, not the file contents. */
  if (::ftruncate(_fd, 0) == 0) {
    const std::string pid = std::to_string(::getpid()) + "\n";
    ssize_t wr = ::write(_fd, pid.data(), pid.size());
    (void)wr;
  }
  return Result::Acquired;
}

bool UsbDeviceLock::held() const { return _fd >= 0; }

UsbDeviceLock::~UsbDeviceLock() {
  if (_fd >= 0) {
    /* Closing the fd releases the flock. Deliberately do NOT unlink the lock
     * file: unlinking races a concurrent waiter (it could lock a now-orphaned
     * inode while a third process re-creates and locks a fresh one). The file
     * is a 0-to-few-byte pid stamp reused across runs — the standard lockfile
     * trade-off. */
    ::flock(_fd, LOCK_UN);
    ::close(_fd);
    _fd = -1;
  }
}

} // namespace devourer

#endif
