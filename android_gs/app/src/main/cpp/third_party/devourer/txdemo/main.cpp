#include <cassert>
#include <iostream>
#include <memory>
#include <string>

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
  #include <libusb-1.0/libusb.h>
#endif

#include "FrameParser.h"
#include "RtlUsbAdapter.h"
#include "WiFiDriver.h"
#include "logger.h"

#include <iomanip>


// #define USB_VENDOR_ID 0x0bda
// #define USB_PRODUCT_ID 0x8812

void printHexArray(const uint8_t *array, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    // Print each byte as a two-digit hexadecimal number
    std::cout << "0x" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<int>(array[i]);

    // Print a space between bytes, but not after the last byte
    if (i < length - 1) {
      std::cout << " ";
    }
  }
  std::cout << std::dec << std::endl; // Reset to decimal formatting
}

static void packetProcessor(const Packet &packet) {}

void usb_event_loop(Logger_t _logger,libusb_context* ctx){
  while (true) {
    int r = libusb_handle_events(ctx);
    if (r < 0) {
      _logger->error("Error handling events: {}", r);
      break;
    }
  }
}

int main(int argc, char **argv) {
  libusb_context *context;
  libusb_device_handle *handle;
  libusb_device *device;
  struct libusb_device_descriptor desc;
  uint8_t usb_frame[10000];
  unsigned char buffer[256];
  struct tx_desc *ptxdesc;
  int fd;

  auto logger = std::make_shared<Logger>();

  // fd from argv is provided by termux on Android
  // https://wiki.termux.com/wiki/Termux-usb
  fd = atoi(argv[1]);
  logger->info("got fd {}", fd);

  // libusb_set_option(context, LIBUSB_OPTION_LOG_LEVEL,
  // LIBUSB_LOG_LEVEL_DEBUG);
  libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
  libusb_set_option(NULL, LIBUSB_OPTION_WEAK_AUTHORITY);

  int rc = libusb_init(&context);

  rc = libusb_wrap_sys_device(context, (intptr_t)fd, &handle);

  device = libusb_get_device(handle);

  rc = libusb_get_device_descriptor(device, &desc);

  logger->info("Vendor/Product ID:{:04x}:{:04x}", desc.idVendor,
               desc.idProduct);
  if (libusb_kernel_driver_active(handle, 0)) {
    rc = libusb_detach_kernel_driver(handle, 0); // detach driver
    logger->error("libusb_detach_kernel_driver: {}", rc);
  }
  rc = libusb_claim_interface(handle, 0);

  WiFiDriver wifi_driver{logger};
  auto rtlDevice = wifi_driver.CreateRtlDevice(handle);
  pid_t fpid;
  fpid = fork();
  rtlDevice->SetTxPower(40);
  if (fpid == 0) {

    rtlDevice->Init(packetProcessor, SelectedChannel{
                                         .Channel = static_cast<uint8_t>(161),
                                         .ChannelOffset = 0,
                                         .ChannelWidth = CHANNEL_WIDTH_20,

                                     });

    return 1;
  }
  // Loop for sending packets


    rtlDevice->InitWrite(SelectedChannel{
                                         .Channel = static_cast<uint8_t>(161),
                                         .ChannelOffset = 0,
                                         .ChannelWidth = CHANNEL_WIDTH_20});


  sleep(5);

  // A new thread starts the libusb event loop
  std::thread usb_thread(usb_event_loop,logger,context);
  uint8_t beacon_frame[] = {
      0x00, 0x00, 0x0d, 0x00, 0x00, 0x80, 0x08, 0x00, 0x08, 0x00, 0x37,
      0x00, 0x01, // radiotap header
      0x08, 0x01, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x57,
      0x42, 0x75, 0x05, 0xd6, 0x00, 0x57, 0x42, 0x75, 0x05, 0xd6, 0x00,
      0x80, 0x00, // 80211 header
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x24, 0x4f,
      0xa0, 0xc5, 0x4a, 0xbb, 0x6a, 0x55, 0x03, 0x72, 0xf8, 0x4d, 0xc4,
      0x9d, 0x1a, 0x51, 0xb7, 0x3f, 0x98, 0xf1, 0xe7, 0x46, 0x4d, 0x1c,
      0x21, 0x86, 0x15, 0x21, 0x02, 0xf4, 0x88, 0x63, 0xff, 0x51, 0x66,
      0x34, 0xf2, 0x16, 0x71, 0xf5, 0x76, 0x0b, 0x35, 0xc0, 0xe1, 0x44,
      0xcd, 0xce, 0x4e, 0x35, 0xd9, 0x85, 0x9a, 0xcf, 0x4d, 0x48, 0x4c,
      0x8f, 0x28, 0x6f, 0x10, 0xb0, 0xa9, 0x5d, 0xbf, 0xcb, 0x6f};

  int actual_length = 0;

  while (true) {
    rc = rtlDevice->send_packet(beacon_frame, sizeof(beacon_frame));
  }
  rc = libusb_release_interface(handle, 0);
  assert(rc == 0);

  libusb_close(handle);

  libusb_exit(context);
  return 0;
}
