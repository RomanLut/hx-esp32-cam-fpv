#ifndef FIRMWARE_H
#define FIRMWARE_H

#define CONFIG_RTL8812A
#define LOAD_FW_HEADER_FROM_DRIVER
#define ODM_WIN 1
#define DM_ODM_SUPPORT_TYPE ODM_WIN
typedef uint8_t u8;
typedef uint32_t u32;
extern "C"{
#include "hal8812a_fw.h"
}

struct Firmware {
  constexpr static u8 *pFirmwareBuf = array_mp_8812a_fw_nic;
  constexpr static size_t FirmwareLen = sizeof(array_mp_8812a_fw_nic);
};

#endif /* FIRMWARE_H */
