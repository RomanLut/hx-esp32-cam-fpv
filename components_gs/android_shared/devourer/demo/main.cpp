#include <cassert>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <libusb.h>

#include "FrameParser.h"
#include "RtlUsbAdapter.h"
#include "WiFiDriver.h"

#define USB_VENDOR_ID 0x0bda

/* Known USB product IDs for the Realtek Jaguar 802.11ac family driven by this
 * library: RTL8812AU (2T2R), RTL8811AU (1T1R cut), and RTL8814AU (4T4R RF /
 * 3-SS baseband). */
static constexpr uint16_t kRealtekProductIds[] = {
    0x8812, /* RTL8812AU (also seen on some 8811AU boards) */
    0x0811, /* RTL8811AU */
    0xa811, /* RTL8811AU */
    0xb811, /* RTL8811AU/8821AU variants */
    0x8813, /* RTL8814AU (Realtek demoboard PID, used by CF-938AC/CF-960AC) */
};

static int g_rx_count = 0;
static void packetProcessor(const Packet &packet) {
  ++g_rx_count;
  if (g_rx_count <= 10 || g_rx_count % 100 == 0) {
    printf("<devourer>RX pkt #%d (len=%zu)\n", g_rx_count, packet.Data.size());
    fflush(stdout);
  }
  /* TX-validation hook: detect frames whose SA matches the txdemo's hardcoded
   * injected beacon (57:42:75:05:d6:00). When running this RX demo against
   * one adapter while WiFiDriverTxDemo runs against another on the same
   * channel, each hit confirms an injected frame made it over the air. */
  if (packet.Data.size() >= 16) {
    static const uint8_t kTxSa[6] = {0x57, 0x42, 0x75, 0x05, 0xd6, 0x00};
    if (std::memcmp(packet.Data.data() + 10, kTxSa, 6) == 0) {
      static int hits = 0;
      ++hits;
      if (hits <= 10 || hits % 100 == 0) {
        printf("<devourer-tx-hit>txdemo SA match: hits=%d total_rx=%d len=%zu\n",
               hits, g_rx_count, packet.Data.size());
        fflush(stdout);
      }
    }
  }
}

int main() {
  libusb_context *ctx;
  int rc;

  auto logger = std::make_shared<Logger>();

  rc = libusb_init(&ctx);
  if (rc < 0) {
    return rc;
  }

  /* libusb log level: DEBUG by default (matches historic behaviour); a single
   * DEVOURER_USB_QUIET=1 knob downgrades to WARNING to keep capture logs
   * manageable. DEBUG output runs ~7 MB per 15s run, which has filled /tmp
   * and wedged the harness on previous sessions. */
  libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL,
                    std::getenv("DEVOURER_USB_QUIET")
                        ? LIBUSB_LOG_LEVEL_WARNING
                        : LIBUSB_LOG_LEVEL_DEBUG);

  /* DEVOURER_PID env var (hex, e.g. "0x8813") restricts the open loop to a
   * single PID. Useful when multiple Realtek adapters are plugged.
   * DEVOURER_VID overrides the VID (default 0x0bda Realtek) — needed to reach
   * OEM-rebadged Jaguar dongles like the TP-Link Archer T2U Plus (2357:0120). */
  const char *pid_env = std::getenv("DEVOURER_PID");
  uint16_t target_pid = 0;
  if (pid_env != nullptr) {
    target_pid = static_cast<uint16_t>(std::strtoul(pid_env, nullptr, 0));
    logger->info("DEVOURER_PID={:04x} (limiting to this PID)", target_pid);
  }
  uint16_t target_vid = USB_VENDOR_ID;
  if (const char *vid_env = std::getenv("DEVOURER_VID")) {
    target_vid = static_cast<uint16_t>(std::strtoul(vid_env, nullptr, 0));
    logger->info("DEVOURER_VID={:04x} (overriding default VID)", target_vid);
  }
  libusb_device_handle *dev_handle = nullptr;
  for (uint16_t pid : kRealtekProductIds) {
    if (target_pid != 0 && pid != target_pid) continue;
    dev_handle = libusb_open_device_with_vid_pid(ctx, target_vid, pid);
    if (dev_handle != NULL) {
      logger->info("Opened device {:04x}:{:04x}", target_vid, pid);
      break;
    }
  }
  /* DEVOURER_PID can name a PID not in kRealtekProductIds (e.g. 0x0120 for the
   * T2U Plus). Try that direct combination once before giving up. */
  if (dev_handle == NULL && target_pid != 0) {
    dev_handle = libusb_open_device_with_vid_pid(ctx, target_vid, target_pid);
    if (dev_handle != NULL) {
      logger->info("Opened device {:04x}:{:04x} (via DEVOURER_PID)",
                   target_vid, target_pid);
    }
  }
  if (dev_handle == NULL) {
    logger->error("Cannot find any supported device under VID {:04x}",
                  target_vid);
    libusb_exit(ctx);
    return 1;
  }

  // Check if the kernel driver attached
  if (libusb_kernel_driver_active(dev_handle, 0)) {
    rc = libusb_detach_kernel_driver(dev_handle, 0); // detach driver
  }

  /* Skip USB reset if DEVOURER_SKIP_RESET=1. Used when picking up a chip
   * with firmware already running (e.g. after a patched-rtw88 sysfs unbind):
   * USB reset would clobber fw state and force us to re-run fwdl. */
  if (!std::getenv("DEVOURER_SKIP_RESET")) {
    libusb_reset_device(dev_handle);
  } else {
    logger->info("DEVOURER_SKIP_RESET set — skipping libusb_reset_device");
  }
  rc = libusb_claim_interface(dev_handle, 0);
  assert(rc == 0);

  /* Probe MCUFWDL immediately after claim, before any other chip access.
   * Useful for diagnosing post-rtw88-unbind takeover: shows whether chip
   * state has transitioned away from rtw88's 0x0060e078 (fw running). */
  if (std::getenv("DEVOURER_PROBE_MCUFWDL")) {
    uint32_t mcufwdl = 0;
    for (int i = 0; i < 5; ++i) {
      int got = libusb_control_transfer(dev_handle, 0xC0, 5, 0x0080, 0,
                                        reinterpret_cast<uint8_t *>(&mcufwdl),
                                        4, 500);
      logger->info("PROBE MCUFWDL[{}]: rc={} val=0x{:08x}", i, got, mcufwdl);
    }
  }

  WiFiDriver wifi_driver(logger);
  auto rtlDevice = wifi_driver.CreateRtlDevice(dev_handle);
  /* Default channel 36 (5 GHz) for the 8812 reference. Override with
   * DEVOURER_CHANNEL=N env var (e.g. DEVOURER_CHANNEL=6 for busy 2.4 GHz). */
  int channel = 36;
  if (const char *ch_env = std::getenv("DEVOURER_CHANNEL")) {
    channel = std::atoi(ch_env);
    logger->info("DEVOURER_CHANNEL set — tuning to channel {}", channel);
  }
  rtlDevice->Init(packetProcessor, SelectedChannel{
                                       .Channel = static_cast<uint8_t>(channel),
                                       .ChannelOffset = 0,
                                       .ChannelWidth = CHANNEL_WIDTH_20,
                                   });

  rc = libusb_release_interface(dev_handle, 0);
  assert(rc == 0);

  libusb_close(dev_handle);

  libusb_exit(ctx);

  return 0;
}
