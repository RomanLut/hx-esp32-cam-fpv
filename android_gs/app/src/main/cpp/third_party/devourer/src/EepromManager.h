#ifndef EEPROMMANAGER_H
#define EEPROMMANAGER_H

#include "RtlUsbAdapter.h"
#include "logger.h"

#include "HalVerDef.h"
#include "phydm_pre_define.h"
#include "rtl8812a_hal.h"

typedef unsigned short ushort;

class EepromManager {
  RtlUsbAdapter _device;
  Logger_t _logger;

  uint8_t efuse_eeprom_data[EFUSE_MAP_LEN_JAGUAR]; /*92C:256bytes, 88E:512bytes,
                                     we use union set (512bytes)*/
  uint8_t EEPROMVersion;
  uint8_t EEPROMRegulatory;
  bool EEPROMBluetoothCoexist;
  uint8_t eeprom_thermal_meter;
  uint8_t PAType_2G;
  uint8_t PAType_5G;
  uint8_t LNAType_2G;
  uint8_t LNAType_5G;
  uint8_t external_pa_5g;
  uint8_t external_lna_5g;

public:
  EepromManager(RtlUsbAdapter device, Logger_t logger);
  uint8_t GetBoardType();
  void efuse_ShadowRead1Byte(uint16_t Offset, uint8_t *Value);

  HAL_VERSION version_id;
  odm_cut_version_e cut_version;
  uint8_t crystal_cap;
  uint16_t TypeGPA;
  uint16_t TypeAPA;
  uint16_t TypeGLNA;
  uint16_t TypeALNA;
  uint8_t numTotalRfPath;
  bool ExternalLNA_2G;
  bool ExternalPA_2G;
  HAL_RF_TYPE_E rf_type;
  uint16_t rfe_type;

private:
  void read_chip_version_8812a(RtlUsbAdapter device);
  void dump_chip_info(HAL_VERSION ChipVersion);
  void rtw_hal_config_rftype();
  void hal_InitPGData_8812A();
  void EFUSE_ShadowMapUpdate(uint8_t efuseType);
  void Efuse_ReadAllMap(uint8_t efuseType, uint8_t *Efuse);
  void EfusePowerSwitch8812A(bool bWrite, bool pwrState);
  bool IsEfuseTxPowerInfoValid(uint8_t *efuseEepromData);
  void efuse_ReadEFuse(uint8_t efuseType, uint16_t _offset, uint16_t _size_byte,
                       uint8_t *pbuf);
  void Hal_EfuseReadEFuse8812A(uint16_t _offset, uint16_t _size_byte,
                               uint8_t *pbuf);
  void rtw_dump_cur_efuse();
  void Hal_EfuseParseIDCode8812A();
  uint8_t Hal_ReadPROMVersion8812A(RtlUsbAdapter device,
                                   uint8_t *efuse_eeprom_data);
  uint8_t Hal_ReadTxPowerInfo8812A(RtlUsbAdapter device,
                                   uint8_t *efuse_eeprom_data);
  void Hal_EfuseParseBTCoexistInfo8812A();
  void Hal_EfuseParseXtal_8812A();
  void Hal_ReadThermalMeter_8812A();
  void Hal_ReadAmplifierType_8812A();
  void hal_ReadPAType_8812A();
  void Hal_ReadRFEType_8812A();
  void hal_ReadUsbType_8812AU();
};

#endif /* EEPROMMANAGER_H */
