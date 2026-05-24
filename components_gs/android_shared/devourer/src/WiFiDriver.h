#ifndef WIFI_DRIVER_H
#define WIFI_DRIVER_H

#include <memory>

#include "RtlJaguarDevice.h"
#include "logger.h"

class WiFiDriver {
  Logger_t _logger;

public:
  explicit WiFiDriver(Logger_t logger);

  std::unique_ptr<RtlJaguarDevice>
  CreateRtlDevice(libusb_device_handle *dev_handle);
};

#endif /* WIFI_DRIVER_H */
