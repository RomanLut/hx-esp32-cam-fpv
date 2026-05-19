#include "WiFiDriver.h"

#include <utility>
#include "Rtl8812aDevice.h"

WiFiDriver::WiFiDriver(Logger_t logger)
    : _logger{std::move(logger)} {}

std::unique_ptr<Rtl8812aDevice>
WiFiDriver::CreateRtlDevice(libusb_device_handle *dev_handle) {
  _logger->info("Creating Rtl8812aDevice from LibUSB");

  return std::make_unique<Rtl8812aDevice>(RtlUsbAdapter(dev_handle, _logger), _logger);
}
