#include "WiFiDriver.h"

#include <utility>
#include "RtlJaguarDevice.h"

WiFiDriver::WiFiDriver(Logger_t logger)
    : _logger{std::move(logger)} {}

std::unique_ptr<RtlJaguarDevice>
WiFiDriver::CreateRtlDevice(libusb_device_handle *dev_handle) {
  _logger->info("Creating RtlJaguarDevice from LibUSB");

  return std::make_unique<RtlJaguarDevice>(RtlUsbAdapter(dev_handle, _logger), _logger);
}
