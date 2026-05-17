#include "FirmwareManager.h"

#include "Firmware.h"
#include "rtl8812a_hal.h"

#include <chrono>

template <class result_t = std::chrono::milliseconds,
          class clock_t = std::chrono::steady_clock,
          class duration_t = std::chrono::milliseconds>
auto since(std::chrono::time_point<clock_t, duration_t> const &start) {
  return std::chrono::duration_cast<result_t>(clock_t::now() - start);
}

FirmwareManager::FirmwareManager(RtlUsbAdapter device, Logger_t logger)
    : _device{device}, _logger{logger} {}

void FirmwareManager::FirmwareDownload8812() {
  bool rtStatus = true;
  uint8_t write_fw = 0;

  auto pFirmwareBuf = Firmware::pFirmwareBuf;
  auto FirmwareLen = Firmware::FirmwareLen;

  auto firmwareVersion = (uint16_t)GET_FIRMWARE_HDR_VERSION_8812(pFirmwareBuf);
  auto firmwareSubVersion =
      (uint16_t)GET_FIRMWARE_HDR_SUB_VER_8812(pFirmwareBuf);
  auto firmwareSignature =
      (uint16_t)GET_FIRMWARE_HDR_SIGNATURE_8812(pFirmwareBuf);

  _logger->info("FirmwareDownload8812: fw_ver={} "
                "fw_subver={} sig=0x{:X}",
                firmwareVersion, firmwareSubVersion, firmwareSignature);

  if (IS_FW_HEADER_EXIST_8812(pFirmwareBuf)) {
    /* Shift 32 bytes for FW header */
    pFirmwareBuf = pFirmwareBuf + 32;
    FirmwareLen -= 32;
  }

  /* Suggested by Filen. If 8051 is running in RAM code, driver should inform Fw
   * to reset by itself, */
  /* or it will cause download Fw fail. 2010.02.01. by tynli. */
  if ((_device.rtw_read8(REG_MCUFWDL) & BIT7) != 0) {
    /* 8051 RAM code */
    _device.rtw_write8(REG_MCUFWDL, 0x00);
    _device._8051Reset8812();
  }

  _FWDownloadEnable_8812(true);

  auto fwdl_start_time = std::chrono::steady_clock::now();
  while ((write_fw++ < 3 || (since(fwdl_start_time).count()) < 500)) {
    /* reset FWDL chksum */
    _device.rtw_write8(REG_MCUFWDL, (uint8_t)(_device.rtw_read8(REG_MCUFWDL) |
                                              FWDL_ChkSum_rpt));

    rtStatus = WriteFW8812(pFirmwareBuf, FirmwareLen);
    if (rtStatus != true) {
      continue;
    }

    rtStatus = polling_fwdl_chksum(5, 50);
    if (rtStatus == true) {
      break;
    }
  }

  _FWDownloadEnable_8812(false);
  if (true != rtStatus) {
    return;
  }

  rtStatus = _FWFreeToGo8812(10, 200);
  if (true != rtStatus) {
    return;
  }

  InitializeFirmwareVars8812();
}

// TODO: now does nothing
static void yield() {}

void FirmwareManager::_FWDownloadEnable_8812(bool enable) {
  u8 tmp;

  if (enable) {
    /* MCU firmware download enable. */
    tmp = _device.rtw_read8(REG_MCUFWDL);
    _device.rtw_write8(REG_MCUFWDL, tmp | 0x01);

    /* 8051 reset */
    tmp = _device.rtw_read8(REG_MCUFWDL + 2);
    _device.rtw_write8(REG_MCUFWDL + 2, tmp & 0xf7);
  } else {

    /* MCU firmware download disable. */
    tmp = _device.rtw_read8(REG_MCUFWDL);
    _device.rtw_write8(REG_MCUFWDL, tmp & 0xfe);
  }
}

bool FirmwareManager::WriteFW8812(uint8_t *buffer, uint32_t size) {
  const int MAX_DLFW_PAGE_SIZE = 4096; /* @ page : 4k bytes */

  /* Since we need dynamic decide method of dwonload fw, so we call this
   * function to get chip version. */
  bool ret = true;
  int32_t pageNums, remainSize;
  int32_t page;
  int offset;
  auto bufferPtr = buffer;

  pageNums = (int)(size / MAX_DLFW_PAGE_SIZE);
  /* RT_ASSERT((pageNums <= 4), ("Page numbers should not greater then 4\n"));
   */
  remainSize = (int)(size % MAX_DLFW_PAGE_SIZE);

  for (page = 0; page < pageNums; page++) {
    offset = page * MAX_DLFW_PAGE_SIZE;
    ret = _PageWrite_8812(page, bufferPtr + offset, MAX_DLFW_PAGE_SIZE);

    if (ret == false) {
      goto exit;
    }
  }

  if (remainSize != 0) {
    offset = pageNums * MAX_DLFW_PAGE_SIZE;
    page = pageNums;
    ret = _PageWrite_8812(page, bufferPtr + offset, remainSize);

    if (ret == false) {
      goto exit;
    }
  }

exit:
  return ret;
}

int FirmwareManager::_PageWrite_8812(uint32_t page, uint8_t *buffer,
                                     uint32_t size) {
  u8 value8;
  u8 u8Page = (u8)(page & 0x07);

  value8 = (_device.rtw_read8(REG_MCUFWDL + 2) & 0xF8) | u8Page;
  _device.rtw_write8(REG_MCUFWDL + 2, value8);

  return BlockWrite(buffer, size);
}

bool FirmwareManager::BlockWrite(uint8_t *buffer, int buffSize) {
  const int MAX_REG_BOLCK_SIZE = 196;

  bool ret = true;

  uint32_t blockSize_p1 =
      4; /* (Default) Phase #1 : PCI muse use 4-byte write to download FW */
  uint32_t blockSize_p2 =
      8; /* Phase #2 : Use 8-byte, if Phase#1 use big size to write FW. */
  uint32_t blockSize_p3 =
      1; /* Phase #3 : Use 1-byte, the remnant of FW image. */
  uint32_t blockCount_p1 = 0, blockCount_p2 = 0, blockCount_p3 = 0;
  uint32_t remainSize_p1 = 0, remainSize_p2 = 0;
  // byte			*bufferPtr	= (byte *)buffer;
  uint32_t i = 0, offset = 0;

  blockSize_p1 = MAX_REG_BOLCK_SIZE;

  /* 3 Phase #1 */
  blockCount_p1 = (uint32_t)(buffSize / blockSize_p1);
  remainSize_p1 = (uint32_t)(buffSize % blockSize_p1);

  for (i = 0; i < blockCount_p1; i++) {
    _device.WriteBytes((ushort)(FW_START_ADDRESS + i * blockSize_p1),
                       buffer + i * blockSize_p1, (int)blockSize_p1);
  }

  /* 3 Phase #2 */
  if (remainSize_p1 != 0) {
    offset = blockCount_p1 * blockSize_p1;

    blockCount_p2 = remainSize_p1 / blockSize_p2;
    remainSize_p2 = remainSize_p1 % blockSize_p2;

    for (i = 0; i < blockCount_p2; i++) {
      _device.WriteBytes((ushort)(FW_START_ADDRESS + offset + i * blockSize_p2),
                         buffer + offset + i * blockSize_p2, (int)blockSize_p2);
    }
  }

  /* 3 Phase #3 */
  if (remainSize_p2 != 0) {
    offset = (blockCount_p1 * blockSize_p1) + (blockCount_p2 * blockSize_p2);

    blockCount_p3 = remainSize_p2 / blockSize_p3;

    for (i = 0; i < blockCount_p3; i++) {
      _device.rtw_write8((ushort)(FW_START_ADDRESS + offset + i),
                         buffer[(int)(offset + i)]);
    }
  }

  return ret;
}

void FirmwareManager::InitializeFirmwareVars8812() {
  /* Init H2C cmd. */
  _device.rtw_write8(REG_HMETFR, 0x0f);
}

bool FirmwareManager::polling_fwdl_chksum(uint32_t min_cnt, uint32_t timeout_ms) {
  bool ret = false;
  u32 value32;
  auto start = std::chrono::steady_clock::now();
  u32 cnt = 0;

  /* polling CheckSum report */
  do {
    cnt++;
    value32 = _device.rtw_read32(REG_MCUFWDL);
    if (value32 & FWDL_ChkSum_rpt)
      break;
    yield();
  } while (since(start).count() < timeout_ms || cnt < min_cnt);

  if (!(value32 & FWDL_ChkSum_rpt))
    goto exit;

  ret = true;

exit:
  _logger->info("{}: Checksum report {}! ({}, {}ms), REG_MCUFWDL:{:08x}",
                __FUNCTION__, ret == true ? "OK" : "Fail", cnt,
                since(start).count(), value32);

  return ret;
}

bool FirmwareManager::_FWFreeToGo8812(uint32_t min_cnt, uint32_t timeout_ms) {
  bool ret = false;
  uint32_t value32;
  uint32_t cnt = 0;

  value32 = _device.rtw_read32(REG_MCUFWDL);
  value32 |= MCUFWDL_RDY;
  value32 = (uint32_t)(value32 & ~WINTINI_RDY);
  _device.rtw_write32(REG_MCUFWDL, value32);

  _device._8051Reset8812();

  auto start = std::chrono::steady_clock::now();
  /*  polling for FW ready */
  do {
    cnt++;
    value32 = _device.rtw_read32(REG_MCUFWDL);
    if ((value32 & WINTINI_RDY) != 0) {
      break;
    }
    yield();
  } while (since(start).count() < timeout_ms || cnt < min_cnt);

  if (!((value32 & WINTINI_RDY) != 0)) {
    goto exit;
  }

  // if (rtw_fwdl_test_trigger_wintint_rdy_fail())
  //{
  //     goto exit;
  // }

  ret = true;

exit:
  _logger->info("{}: Polling FW ready {}! ({}, {}ms), REG_MCUFWDL:0x{:08x}",
                __FUNCTION__, ret == true ? "OK" : "Fail", cnt,
                since(start).count(), value32);

  return ret;
}
