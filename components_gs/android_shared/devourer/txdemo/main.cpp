#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#if defined(_MSC_VER)
  #include <windows.h>
  #include <process.h>
  typedef int pid_t;
  #define fork() (0)
  #define sleep(seconds) Sleep((seconds)*1000)
#elif defined(__ANDROID__)
  #include <libusb.h>
#elif defined(__APPLE__)
  #include <unistd.h>
  #include <libusb.h>
#else
  #include <unistd.h>
  #include <libusb-1.0/libusb.h>
#endif

#include "FrameParser.h"
#include "RtlUsbAdapter.h"
#include "WiFiDriver.h"
#include "logger.h"

#define USB_VENDOR_ID 0x0bda

/* Known USB product IDs for the Realtek Jaguar family — same set as the RX
 * demo (demo/main.cpp). */
static constexpr uint16_t kRealtekProductIds[] = {
    0x8812, 0x0811, 0xa811, 0xb811, 0x8813,
};

static int g_rx_count = 0;
static void packetProcessor(const Packet &packet) {
  ++g_rx_count;
  /* Surface frames whose source MAC matches the txdemo's injected beacon
   * (57:42:75:05:d6:00). The 802.11 header starts at packet.Data[0]; SA is
   * at bytes [10..15] for a non-FromDS, non-ToDS frame. */
  if (packet.Data.size() >= 16) {
    static const uint8_t kTxSa[6] = {0x57, 0x42, 0x75, 0x05, 0xd6, 0x00};
    if (std::memcmp(packet.Data.data() + 10, kTxSa, 6) == 0) {
      static int hits = 0;
      ++hits;
      printf("<devourer-tx-hit>RX from txdemo SA: hits=%d total_rx=%d len=%zu\n",
             hits, g_rx_count, packet.Data.size());
      fflush(stdout);
    }
  }
}

void usb_event_loop(Logger_t _logger, libusb_context *ctx) {
  while (true) {
    int r = libusb_handle_events(ctx);
    if (r < 0) {
      _logger->error("Error handling events: {}", r);
      break;
    }
  }
}

int main(int argc, char **argv) {
  libusb_context *context = nullptr;
  libusb_device_handle *handle = nullptr;
  int rc;

  auto logger = std::make_shared<Logger>();

  /* Two modes:
   *  1. Termux/Android: argv[1] = numeric USB fd (wrapped via
   *     libusb_wrap_sys_device).
   *  2. Linux/macOS: no argv (or non-numeric) — open by VID/PID using the
   *     same list as the RX demo. DEVOURER_PID=0xNNNN restricts to a single
   *     PID. */
  long fd = (argc >= 2) ? std::strtol(argv[1], nullptr, 0) : 0;
  const bool termux_mode = (fd > 0);

  if (termux_mode) {
    logger->info("Termux mode: wrapping fd {}", fd);
    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    libusb_set_option(NULL, LIBUSB_OPTION_WEAK_AUTHORITY);
    rc = libusb_init(&context);
    rc = libusb_wrap_sys_device(context, (intptr_t)fd, &handle);
  } else {
    rc = libusb_init(&context);
    if (rc < 0) return rc;

    const char *pid_env = std::getenv("DEVOURER_PID");
    uint16_t target_pid = 0;
    if (pid_env != nullptr) {
      target_pid = static_cast<uint16_t>(std::strtoul(pid_env, nullptr, 0));
      logger->info("DEVOURER_PID={:04x} (limiting to this PID)", target_pid);
    }
    /* DEVOURER_VID overrides the VID (default 0x0bda) — needed for OEM-rebadged
     * Jaguar dongles like the TP-Link Archer T2U Plus (2357:0120). */
    uint16_t target_vid = USB_VENDOR_ID;
    if (const char *vid_env = std::getenv("DEVOURER_VID")) {
      target_vid = static_cast<uint16_t>(std::strtoul(vid_env, nullptr, 0));
      logger->info("DEVOURER_VID={:04x} (overriding default VID)", target_vid);
    }
    for (uint16_t pid : kRealtekProductIds) {
      if (target_pid != 0 && pid != target_pid) continue;
      handle = libusb_open_device_with_vid_pid(context, target_vid, pid);
      if (handle != NULL) {
        logger->info("Opened device {:04x}:{:04x}", target_vid, pid);
        break;
      }
    }
    /* DEVOURER_PID can name a PID not in kRealtekProductIds (e.g. 0x0120 for
     * the T2U Plus). Try that direct combination once before giving up. */
    if (handle == NULL && target_pid != 0) {
      handle = libusb_open_device_with_vid_pid(context, target_vid, target_pid);
      if (handle != NULL) {
        logger->info("Opened device {:04x}:{:04x} (via DEVOURER_PID)",
                     target_vid, target_pid);
      }
    }
    if (handle == NULL) {
      logger->error("No supported device found under VID {:04x}", target_vid);
      libusb_exit(context);
      return 1;
    }
  }

  libusb_device *device = libusb_get_device(handle);
  libusb_device_descriptor desc{};
  libusb_get_device_descriptor(device, &desc);
  logger->info("Vendor/Product ID: {:04x}:{:04x}", desc.idVendor,
               desc.idProduct);

  if (libusb_kernel_driver_active(handle, 0)) {
    rc = libusb_detach_kernel_driver(handle, 0);
    if (rc != 0) logger->error("libusb_detach_kernel_driver: {}", rc);
  }

  if (!termux_mode && !std::getenv("DEVOURER_SKIP_RESET")) {
    libusb_reset_device(handle);
  }

  rc = libusb_claim_interface(handle, 0);
  assert(rc == 0);

  WiFiDriver wifi_driver{logger};
  auto rtlDevice = wifi_driver.CreateRtlDevice(handle);

  int channel = 161;
  if (const char *ch_env = std::getenv("DEVOURER_CHANNEL")) {
    channel = std::atoi(ch_env);
    logger->info("DEVOURER_CHANNEL set — tuning TX to channel {}", channel);
  }

  rtlDevice->SetTxPower(40);

  /* The original txdemo forked an RX child and a TX parent on the same
   * libusb handle. That pattern is Termux-specific (libusb_wrap_sys_device
   * keeps the kernel fd shared across fork); on a regular Linux libusb
   * context after fork(), both processes race on the same URB submission
   * queue and the first vendor request after fork tends to fail with
   * "rtw_read: iostream error". Skip the fork unless DEVOURER_TX_WITH_RX=1
   * is explicitly set (Termux callers can opt back in). For cross-adapter
   * TX validation a second RX process on a separate adapter is what you
   * want anyway. */
  if (std::getenv("DEVOURER_TX_WITH_RX")) {
    pid_t fpid = fork();
    if (fpid == 0) {
      rtlDevice->Init(packetProcessor,
                      SelectedChannel{
                          .Channel = static_cast<uint8_t>(channel),
                          .ChannelOffset = 0,
                          .ChannelWidth = CHANNEL_WIDTH_20,
                      });
      return 1;
    }
  }

  rtlDevice->InitWrite(SelectedChannel{
      .Channel = static_cast<uint8_t>(channel),
      .ChannelOffset = 0,
      .ChannelWidth = CHANNEL_WIDTH_20});

  sleep(5);

  std::thread usb_thread(usb_event_loop, logger, context);
  uint8_t beacon_frame[] = {
      0x00, 0x00, 0x0d, 0x00, 0x00, 0x80, 0x08, 0x00, 0x08, 0x00, 0x37,
      0x00, 0x01, // radiotap header
      /* Mgmt / probe-request frame (FC=0x40 0x00). Was DATA / ToDS=1
       * (FC=0x08 0x01) which requires an AP context the chip doesn't
       * have in monitor mode — the chip silently NAKed every bulk OUT. */
      0x40, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x57,
      0x42, 0x75, 0x05, 0xd6, 0x00, 0x57, 0x42, 0x75, 0x05, 0xd6, 0x00,
      0x80, 0x00, // 80211 header
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x24, 0x4f,
      0xa0, 0xc5, 0x4a, 0xbb, 0x6a, 0x55, 0x03, 0x72, 0xf8, 0x4d, 0xc4,
      0x9d, 0x1a, 0x51, 0xb7, 0x3f, 0x98, 0xf1, 0xe7, 0x46, 0x4d, 0x1c,
      0x21, 0x86, 0x15, 0x21, 0x02, 0xf4, 0x88, 0x63, 0xff, 0x51, 0x66,
      0x34, 0xf2, 0x16, 0x71, 0xf5, 0x76, 0x0b, 0x35, 0xc0, 0xe1, 0x44,
      0xcd, 0xce, 0x4e, 0x35, 0xd9, 0x85, 0x9a, 0xcf, 0x4d, 0x48, 0x4c,
      0x8f, 0x28, 0x6f, 0x10, 0xb0, 0xa9, 0x5d, 0xbf, 0xcb, 0x6f};

  long tx_count = 0;
  while (true) {
    rc = rtlDevice->send_packet(beacon_frame, sizeof(beacon_frame));
    ++tx_count;
    if (tx_count <= 10 || tx_count % 500 == 0) {
      printf("<devourer-tx>TX #%ld rc=%d\n", tx_count, rc);
      fflush(stdout);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2)); /* ~500 fps, gentle on USB bulk EP */
  }
  rc = libusb_release_interface(handle, 0);
  assert(rc == 0);

  libusb_close(handle);
  libusb_exit(context);
  return 0;
}
