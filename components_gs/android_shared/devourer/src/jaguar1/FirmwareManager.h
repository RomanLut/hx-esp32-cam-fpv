#ifndef FIRMWAREMANAGER_H
#define FIRMWAREMANAGER_H

#include "AdapterHealth.h"
#include "HalVerDef.h"
#include "RtlAdapter.h"
#include "logger.h"

class FirmwareManager {
  RtlAdapter _device;
  Logger_t _logger;
  devourer::DeviceConfig::Tuning _tuning; /* fwdl_8814 / fwdl_8814_chunk */
  devourer::FwBootStatus _boot;           /* outcome of the last download */

public:
  FirmwareManager(RtlAdapter device, Logger_t logger,
                  const devourer::DeviceConfig &cfg = {});
  /* Downloads the NIC firmware blob matched to `ic_type`. Header parsing /
   * download protocol are common across Jaguar (8812 / 8811 / 8814) — only the
   * blob and the header-signature check differ. */
  void FirmwareDownload(HAL_IC_TYPE_E ic_type);
  /* Outcome of the last FirmwareDownload (see src/AdapterHealth.h) — a failed
   * FW boot is deliberately non-fatal on Jaguar1, so this is where it shows. */
  const devourer::FwBootStatus &GetBootStatus() const { return _boot; }

private:
  void _FWDownloadEnable_8812(bool enable);
  void _FWDownloadEnable_8814A(bool enable);
  void _3081Disable8814A();
  void _3081Enable8814A();
  void _DDMAReset8814A();
  void FirmwareDownload_8814A();
  /* Kernel-faithful fwdl bracket (verbatim port of aircrack-ng/rtl8814au
   * FirmwareDownload8814A + HalROMDownloadFWRSVDPage8814A). Default path. */
  void _Fwdl8814_KernelPath(const uint8_t *fw, uint32_t dmem_size,
                            uint32_t iram_size);
  /* Legacy rtw88-usbmon-mimic path, selectable via DEVOURER_8814_FWDL=rtw88.
   * Bit-identical on the wire to the pre-#95 sequence. */
  void _Fwdl8814_Rtw88Path(const uint8_t *fw, uint32_t dmem_size,
                           uint32_t iram_size);
  void _DumpFwdlState8814A(const char *tag);
  void _SetDownLoadFwRsvdPagePkt_8814A(const uint8_t *fw_chunk, uint32_t len);
  bool _WaitDownLoadRSVDPageOK_3081();
  bool _IDDMADownLoadFW_3081(uint32_t source, uint32_t dest, uint32_t length,
                             bool fs, bool ls, bool kernel_flags = false);
  bool WriteFW8812(uint8_t *buffer, uint32_t size);
  int _PageWrite_8812(uint32_t page, uint8_t *buffer, uint32_t size);
  bool BlockWrite(uint8_t *buffer, int buffSize);
  void InitializeFirmwareVars8812();
  bool polling_fwdl_chksum(uint32_t min_cnt, uint32_t timeout_ms);
  bool _FWFreeToGo8812(uint32_t min_cnt, uint32_t timeout_ms,
                       HAL_IC_TYPE_E ic_type);

};

#endif /* FIRMWAREMANAGER_H */
