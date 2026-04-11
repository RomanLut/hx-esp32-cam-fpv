#ifndef FIRMWAREMANAGER_H
#define FIRMWAREMANAGER_H

#include "RtlUsbAdapter.h"
#include "logger.h"

class FirmwareManager {
  RtlUsbAdapter _device;
  Logger_t _logger;

public:
  FirmwareManager(RtlUsbAdapter device, Logger_t logger);
  void FirmwareDownload8812();

private:
  void _FWDownloadEnable_8812(bool enable);
  bool WriteFW8812(uint8_t *buffer, uint32_t size);
  int _PageWrite_8812(uint32_t page, uint8_t *buffer, uint32_t size);
  bool BlockWrite(uint8_t *buffer, int buffSize);
  void InitializeFirmwareVars8812();
  bool polling_fwdl_chksum(uint32_t min_cnt, uint32_t timeout_ms);
     bool _FWFreeToGo8812(uint32_t min_cnt, uint32_t timeout_ms);

};

#endif /* FIRMWAREMANAGER_H */
