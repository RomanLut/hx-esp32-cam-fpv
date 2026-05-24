#ifndef FIRMWAREMANAGER_H
#define FIRMWAREMANAGER_H

#include "HalVerDef.h"
#include "RtlUsbAdapter.h"
#include "logger.h"

class FirmwareManager {
  RtlUsbAdapter _device;
  Logger_t _logger;

public:
  FirmwareManager(RtlUsbAdapter device, Logger_t logger);
  /* Downloads the NIC firmware blob matched to `ic_type`. Header parsing /
   * download protocol are common across Jaguar (8812 / 8811 / 8814) — only the
   * blob and the header-signature check differ. */
  void FirmwareDownload(HAL_IC_TYPE_E ic_type);

private:
  void _FWDownloadEnable_8812(bool enable);
  void _FWDownloadEnable_8814A(bool enable);
  void _3081Disable8814A();
  void _3081Enable8814A();
  void _DDMAReset8814A();
  void FirmwareDownload_8814A();
  void _SetDownLoadFwRsvdPagePkt_8814A(const uint8_t *fw_chunk, uint32_t len);
  bool _WaitDownLoadRSVDPageOK_3081();
  bool _IDDMADownLoadFW_3081(uint32_t source, uint32_t dest, uint32_t length,
                             bool fs, bool ls);
  bool WriteFW8812(uint8_t *buffer, uint32_t size);
  int _PageWrite_8812(uint32_t page, uint8_t *buffer, uint32_t size);
  bool BlockWrite(uint8_t *buffer, int buffSize);
  void InitializeFirmwareVars8812();
  bool polling_fwdl_chksum(uint32_t min_cnt, uint32_t timeout_ms);
  bool _FWFreeToGo8812(uint32_t min_cnt, uint32_t timeout_ms,
                       HAL_IC_TYPE_E ic_type);

};

#endif /* FIRMWAREMANAGER_H */
