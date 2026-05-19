#ifndef FIRMWARE_H
#define FIRMWARE_H

#define CONFIG_RTL8812A
#define LOAD_FW_HEADER_FROM_DRIVER
#define ODM_WIN 1
#define DM_ODM_SUPPORT_TYPE ODM_WIN
typedef uint8_t u8;
typedef uint32_t u32;
extern "C" {
#include "hal8812a_fw.h"
#define CONFIG_RTL8821A
#include "hal8821a_fw.h"
#undef CONFIG_RTL8821A
}

#endif /* FIRMWARE_H */
