#ifndef FIRMWARE_H
#define FIRMWARE_H

#include <cstddef>
#include <cstdint>

#include "HalVerDef.h"

#define CONFIG_RTL8812A
#define CONFIG_RTL8814A
#define LOAD_FW_HEADER_FROM_DRIVER
#define ODM_WIN 1
#define DM_ODM_SUPPORT_TYPE ODM_WIN
typedef uint8_t u8;
typedef uint32_t u32;
extern "C" {
#include "hal8812a_fw.h"
#include "hal8814a_fw.h"
/* CONFIG_RTL8821A is scoped tightly: hal/rtl8812a_spec.h has a vendor-pattern
 * `#ifdef CONFIG_RTL8821A #include "rtl8821a_spec.h"` block, and we don't
 * carry that spec header (8821AU register layout is covered by the shared
 * 8812 spec). Define just for the firmware-header include, then undef. */
#define CONFIG_RTL8821A
#include "hal8821a_fw.h"
#undef CONFIG_RTL8821A
}

struct FirmwareBlob {
  const u8 *buf;
  std::size_t len;
};

/* Pick the right firmware NIC blob for the given chip. CHIP_8814A and
 * CHIP_8821 ship their own compiled-in arrays; CHIP_8812 (and the 1T1R 8811AU
 * cut that shares its HAL) rides on the 8812 blob. The 8821 blob carries the
 * IS_FW_HEADER_EXIST_8821 signature (0x2100) — the FW header dispatch in
 * jaguar_fw_header_present picks the right header parse based on ic_type. */
inline FirmwareBlob PickFirmwareForChip(HAL_IC_TYPE_E ic_type) {
  if (ic_type == CHIP_8814A) {
    return {array_mp_8814a_fw_nic, sizeof(array_mp_8814a_fw_nic)};
  }
  if (ic_type == CHIP_8821) {
    return {array_mp_8821a_fw_nic, sizeof(array_mp_8821a_fw_nic)};
  }
  return {array_mp_8812a_fw_nic, sizeof(array_mp_8812a_fw_nic)};
}

#endif /* FIRMWARE_H */
