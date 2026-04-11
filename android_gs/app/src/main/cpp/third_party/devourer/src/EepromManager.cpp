#include "EepromManager.h"

#include "phydm_pre_define.h"
#include "registry_priv.h"
#include "rtl8812a_hal.h"
#include "rtw_efuse.h"

#include <cstring>

EepromManager::EepromManager(RtlUsbAdapter device, Logger_t logger)
    : _device{device}, _logger{logger} {
  read_chip_version_8812a(device);

  hal_InitPGData_8812A();
  Hal_EfuseParseIDCode8812A();
  EEPROMVersion = Hal_ReadPROMVersion8812A(_device, efuse_eeprom_data);
  EEPROMRegulatory = Hal_ReadTxPowerInfo8812A(_device, efuse_eeprom_data);

  /*  */
  /* Read Bluetooth co-exist and initialize */
  /*  */
  Hal_EfuseParseBTCoexistInfo8812A();

  Hal_EfuseParseXtal_8812A();
  Hal_ReadThermalMeter_8812A();

  Hal_ReadAmplifierType_8812A();
  Hal_ReadRFEType_8812A();

#if 0
  // EEPROMUsbSwitch not used in our code
  pHalData.EEPROMUsbSwitch = ReadUsbModeSwitch8812AU(pHalData.efuse_eeprom_data,
                                                     pHalData.AutoloadFailFlag);
  RTW_INFO("Usb Switch: %d", pHalData.EEPROMUsbSwitch);
#endif

  /* 2013/04/15 MH Add for different board type recognize. */
  hal_ReadUsbType_8812AU();
}

void EepromManager::read_chip_version_8812a(RtlUsbAdapter device) {
  uint32_t value32 = device.rtw_read32(REG_SYS_CFG);
  _logger->info("read_chip_version_8812a SYS_CFG(0x{:X})=0x{:08X}", REG_SYS_CFG,
                value32);

  version_id = {
      .ICType = CHIP_8812,
      .ChipType = (value32 & RTL_ID) ? TEST_CHIP : NORMAL_CHIP,
      .VendorType = (value32 & VENDOR_ID) ? CHIP_VENDOR_UMC : CHIP_VENDOR_TSMC,
      .RFType = RF_TYPE_2T2R, /* RF_2T2R; */
  };

  if (registry_priv::special_rf_path == 1) {
    version_id.RFType = RF_TYPE_1T1R; /* RF_1T1R; */
  }

  version_id.CUTVersion = (HAL_CUT_VERSION_E)(((value32 & CHIP_VER_RTL_MASK) >>
                                               CHIP_VER_RTL_SHIFT) +
                                              1); /* IC version (CUT) */

  version_id.ROMVer = 0; /* ROM code version. */

  rtw_hal_config_rftype();

  dump_chip_info(version_id);
}

void EepromManager::rtw_hal_config_rftype() {
  if (IS_1T1R(version_id)) {
    rf_type = RF_TYPE_1T1R;
    numTotalRfPath = 1;
  } else if (IS_2T2R(version_id)) {
    rf_type = RF_TYPE_2T2R;
    numTotalRfPath = 2;
  } else if (IS_1T2R(version_id)) {
    rf_type = RF_TYPE_1T2R;
    numTotalRfPath = 2;
  } else if (IS_3T3R(version_id)) {
    rf_type = RF_TYPE_3T3R;
    numTotalRfPath = 3;
  } else if (IS_4T4R(version_id)) {
    rf_type = RF_TYPE_4T4R;
    numTotalRfPath = 4;
  } else {
    rf_type = RF_TYPE_1T1R;
    numTotalRfPath = 1;
  }

  _logger->info("RF_Type is {} TotalTxPath is {}", (int)rf_type,
                (int)numTotalRfPath);
}

void EepromManager::dump_chip_info(HAL_VERSION ChipVersion) {
  int cnt = 0;
  char buf[128] = {0};

  if (IS_8188E(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8188E_");
  else if (IS_8188F(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8188F_");
  else if (IS_8812_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8812_");
  else if (IS_8192E(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8192E_");
  else if (IS_8821_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8821_");
  else if (IS_8723B_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8723B_");
  else if (IS_8703B_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8703B_");
  else if (IS_8723D_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8723D_");
  else if (IS_8814A_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8814A_");
  else if (IS_8822B_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8822B_");
  else if (IS_8821C_SERIES(ChipVersion))
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_8821C_");
  else
    cnt += sprintf((buf + cnt), "Chip Version Info: CHIP_UNKNOWN_");

  cnt += sprintf((buf + cnt), "%s_",
                 IS_NORMAL_CHIP(ChipVersion) ? "Normal_Chip" : "Test_Chip");
  if (IS_CHIP_VENDOR_TSMC(ChipVersion))
    cnt += sprintf((buf + cnt), "%s_", "TSMC");
  else if (IS_CHIP_VENDOR_UMC(ChipVersion))
    cnt += sprintf((buf + cnt), "%s_", "UMC");
  else if (IS_CHIP_VENDOR_SMIC(ChipVersion))
    cnt += sprintf((buf + cnt), "%s_", "SMIC");

  if (IS_A_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "A_CUT_");
  else if (IS_B_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "B_CUT_");
  else if (IS_C_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "C_CUT_");
  else if (IS_D_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "D_CUT_");
  else if (IS_E_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "E_CUT_");
  else if (IS_F_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "F_CUT_");
  else if (IS_I_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "I_CUT_");
  else if (IS_J_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "J_CUT_");
  else if (IS_K_CUT(ChipVersion))
    cnt += sprintf((buf + cnt), "K_CUT_");
  else
    cnt += sprintf((buf + cnt), "UNKNOWN_CUT(%d)_", ChipVersion.CUTVersion);

  if (IS_1T1R(ChipVersion))
    cnt += sprintf((buf + cnt), "1T1R_");
  else if (IS_1T2R(ChipVersion))
    cnt += sprintf((buf + cnt), "1T2R_");
  else if (IS_2T2R(ChipVersion))
    cnt += sprintf((buf + cnt), "2T2R_");
  else if (IS_3T3R(ChipVersion))
    cnt += sprintf((buf + cnt), "3T3R_");
  else if (IS_3T4R(ChipVersion))
    cnt += sprintf((buf + cnt), "3T4R_");
  else if (IS_4T4R(ChipVersion))
    cnt += sprintf((buf + cnt), "4T4R_");
  else
    cnt += sprintf((buf + cnt), "UNKNOWN_RFTYPE(%d)_", ChipVersion.RFType);

  cnt += sprintf((buf + cnt), "RomVer(%d)", ChipVersion.ROMVer);

  _logger->info(buf);
}

uint8_t EepromManager::GetBoardType() {
  /* 1 ======= BoardType: ODM_CMNINFO_BOARD_TYPE ======= */
  uint32_t odm_board_type = ODM_BOARD_DEFAULT;

  if (ExternalLNA_2G) {
    odm_board_type |= ODM_BOARD_EXT_LNA;
  }

  if (external_lna_5g) {
    odm_board_type |= ODM_BOARD_EXT_LNA_5G;
  }

  if (ExternalPA_2G) {
    odm_board_type |= ODM_BOARD_EXT_PA;
  }

  if (external_pa_5g) {
    odm_board_type |= ODM_BOARD_EXT_PA_5G;
  }

  if (EEPROMBluetoothCoexist) {
    odm_board_type |= ODM_BOARD_BT;
  }

  return (uint8_t)odm_board_type;
}

void EepromManager::hal_InitPGData_8812A() {
  uint32_t i;

  if (false == _device.AutoloadFailFlag) {
    /* autoload OK. */
    if (_device.EepromOrEfuse) {
      /* Read all Content from EEPROM or EFUSE. */
      for (i = 0; i < HWSET_MAX_SIZE_JAGUAR; i += 2) {
        /* value16 = EF2Byte(ReadEEprom(pAdapterState, (u2Byte) (i>>1))); */
        /* *((UInt16*)(&PROMContent[i])) = value16; */
      }
    } else {
      /*  */
      /* 2013/03/08 MH Add for 8812A HW limitation, ROM code can only */
      /*  */
      uint8_t efuse_content[4] = {0};
      _device.efuse_OneByteRead(0x200, &efuse_content[0]);
      _device.efuse_OneByteRead(0x202, &efuse_content[1]);
      _device.efuse_OneByteRead(0x204, &efuse_content[2]);
      _device.efuse_OneByteRead(0x210, &efuse_content[3]);
      if (efuse_content[0] != 0xFF || efuse_content[1] != 0xFF ||
          efuse_content[2] != 0xFF || efuse_content[3] != 0xFF) {
        /* DbgPrint("Disable FW ofl load\n"); */
        /* pMgntInfo.RegFWOffload = FALSE; */
      }

      /* Read EFUSE real map to shadow. */
      EFUSE_ShadowMapUpdate(EFUSE_WIFI);
    }
  } else {
    /* autoload fail */
    /* pHalData.AutoloadFailFlag = true; */
    /*  */
    /* 2013/03/08 MH Add for 8812A HW limitation, ROM code can only */
    /*  */
    uint8_t efuse_content[4] = {0};
    _device.efuse_OneByteRead(0x200, &efuse_content[0]);
    _device.efuse_OneByteRead(0x202, &efuse_content[1]);
    _device.efuse_OneByteRead(0x204, &efuse_content[2]);
    _device.efuse_OneByteRead(0x210, &efuse_content[3]);
    if (efuse_content[0] != 0xFF || efuse_content[1] != 0xFF ||
        efuse_content[2] != 0xFF || efuse_content[3] != 0xFF) {
      _device.AutoloadFailFlag = false;
    }

    /* update to default value 0xFF */
    if (!_device.EepromOrEfuse) {
      EFUSE_ShadowMapUpdate(EFUSE_WIFI);
    }
  }

  if (IsEfuseTxPowerInfoValid(efuse_eeprom_data) == false) {
    throw std::logic_error("Hal_readPGDataFromConfigFile not yet implemented");
  }
}

void EepromManager::EFUSE_ShadowMapUpdate(uint8_t efuseType) {
  if (_device.AutoloadFailFlag) {
    for (int i = 0; i < sizeof(efuse_eeprom_data); i++) {
      efuse_eeprom_data[i] = 0xFF;
    }
  } else {
    Efuse_ReadAllMap(efuseType, efuse_eeprom_data);
  }

  rtw_dump_cur_efuse();
}

void EepromManager::Efuse_ReadAllMap(uint8_t efuseType, uint8_t *Efuse) {
  EfusePowerSwitch8812A(false, true);
  efuse_ReadEFuse(efuseType, 0, EFUSE_MAP_LEN_JAGUAR, Efuse);
  EfusePowerSwitch8812A(false, false);
}

enum {
  VOLTAGE_V25 = 0x03,
  LDOE25_SHIFT = 28,
};

void EepromManager::EfusePowerSwitch8812A(bool bWrite, bool pwrState) {
  uint16_t tmpV16;
  const uint8_t EFUSE_ACCESS_ON_JAGUAR = 0x69;
  const uint8_t EFUSE_ACCESS_OFF_JAGUAR = 0x00;
  if (pwrState) {
    _device.rtw_write8(REG_EFUSE_BURN_GNT_8812, EFUSE_ACCESS_ON_JAGUAR);

    /* 1.2V Power: From VDDON with Power Cut(0x0000h[15]), defualt valid */
    tmpV16 = _device.rtw_read16(REG_SYS_ISO_CTRL);
    if (!((tmpV16 & PWC_EV12V) == PWC_EV12V)) {
      tmpV16 |= PWC_EV12V;
      /* Write16(pAdapterState,REG_SYS_ISO_CTRL,tmpV16); */
    }

    /* Reset: 0x0000h[28], default valid */
    tmpV16 = _device.rtw_read16(REG_SYS_FUNC_EN);
    if (!((tmpV16 & FEN_ELDR) == FEN_ELDR)) {
      tmpV16 |= FEN_ELDR;
      _device.rtw_write16(REG_SYS_FUNC_EN, tmpV16);
    }

    /* Clock: Gated(0x0008h[5]) 8M(0x0008h[1]) clock from ANA, default valid */
    tmpV16 = _device.rtw_read16(REG_SYS_CLKR);
    if ((!((tmpV16 & LOADER_CLK_EN) == LOADER_CLK_EN)) ||
        (!((tmpV16 & ANA8M) == ANA8M))) {
      tmpV16 |= (LOADER_CLK_EN | ANA8M);
      _device.rtw_write16(REG_SYS_CLKR, tmpV16);
    }

    if (bWrite) {
      /* Enable LDO 2.5V before read/write action */
      auto tempval = _device.rtw_read8(REG_EFUSE_TEST + 3);
      // tempval &= ~(BIT3 | BIT4 | BIT5 | BIT6);
      // tempval &= (0b1111_0111 & 0b1110_1111 & 0b1101_1111 & 0b1011_1111);
      tempval &= 0b10000111;
      tempval |= (VOLTAGE_V25 << 3);
      tempval |= 0b10000000;
      _device.rtw_write8(REG_EFUSE_TEST + 3, tempval);
    }
  } else {
    _device.rtw_write8(REG_EFUSE_BURN_GNT_8812, EFUSE_ACCESS_OFF_JAGUAR);

    if (bWrite) {
      /* Disable LDO 2.5V after read/write action */
      auto tempval = _device.rtw_read8(REG_EFUSE_TEST + 3);
      _device.rtw_write8(REG_EFUSE_TEST + 3, (uint8_t)(tempval & 0x7F));
    }
  }
}

void EepromManager::efuse_ReadEFuse(uint8_t efuseType, uint16_t _offset,
                                    uint16_t _size_byte, uint8_t *pbuf) {
  if (efuseType == EFUSE_WIFI) {
    Hal_EfuseReadEFuse8812A(_offset, _size_byte, pbuf);
  } else {
    throw std::logic_error("hal_ReadEFuse_BT not yet implemented");
    // hal_ReadEFuse_BT(adapterState, _offset, _size_byte, pbuf, bPseudoTest);
  }
}

void EepromManager::Hal_EfuseReadEFuse8812A(uint16_t _offset,
                                            uint16_t _size_byte,
                                            uint8_t *pbuf) {
  uint16_t eFuse_Addr = 0;
  uint8_t offset, wren;
  uint16_t i, j;
  uint8_t u1temp = 0;

  /*  */
  /* Do NOT excess total size of EFuse table. Added by Roger, 2008.11.10. */
  /*  */
  if ((_offset + _size_byte) > EFUSE_MAP_LEN_JAGUAR) {
    /* total E-Fuse table is 512bytes */
    _logger->error("Hal_EfuseReadEFuse8812A(): Invalid offset({:x}) with read "
                   "bytes({:x})!!",
                   _offset, _size_byte);
    return;
  }

  uint8_t efuseTbl[EFUSE_MAP_LEN_JAGUAR] = {0};

  uint16_t eFuseWord[EFUSE_MAX_SECTION_JAGUAR][EFUSE_MAX_WORD_UNIT];
  /* 0. Refresh efuse init map as all oxFF. */
  memset(eFuseWord, 0xFF,
         EFUSE_MAX_SECTION_JAGUAR * EFUSE_MAX_WORD_UNIT * sizeof(uint16_t));

  /*  */
  /* 1. Read the first byte to check if efuse is empty!!! */
  /*  */
  /*  */
  uint8_t rtemp8;
  _device.ReadEFuseByte(eFuse_Addr, &rtemp8);
  if (rtemp8 != 0xFF) {
    _logger->info("efuse_Addr-{:x} efuse_data={:x}", eFuse_Addr, (int)rtemp8);
    eFuse_Addr++;
  } else {
    _logger->info("EFUSE is empty efuse_Addr-{:x} efuse_data={:x}", eFuse_Addr,
                  (int)rtemp8);
    return;
  }

  /*  */
  /* 2. Read real efuse content. Filter PG header and every section data. */
  /*  */
  while ((rtemp8 != 0xFF) && (eFuse_Addr < EFUSE_REAL_CONTENT_LEN_JAGUAR)) {
    /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("efuse_Addr-%d efuse_data=%x\n",
     * eFuse_Addr-1, *rtemp8)); */

    /* Check PG header for section num. */
    if ((rtemp8 & 0x1F) == 0x0F) {
      /* extended header */
      u1temp = (uint8_t)((rtemp8 & 0xE0) >> 5);
      /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("extended header u1temp=%x
       * *rtemp&0xE0 0x%x\n", u1temp, *rtemp8 & 0xE0)); */

      /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("extended header u1temp=%x\n",
       * u1temp)); */

      _device.ReadEFuseByte(eFuse_Addr, &rtemp8);

      /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("extended header efuse_Addr-%d
       * efuse_data=%x\n", eFuse_Addr, *rtemp8));	 */

      if ((rtemp8 & 0x0F) == 0x0F) {
        eFuse_Addr++;
        _device.ReadEFuseByte(eFuse_Addr, &rtemp8);

        if (rtemp8 != 0xFF && (eFuse_Addr < EFUSE_REAL_CONTENT_LEN_JAGUAR))
          eFuse_Addr++;
        continue;
      } else {
        offset = (uint8_t)(((rtemp8 & 0xF0) >> 1) | u1temp);
        wren = (uint8_t)(rtemp8 & 0x0F);
        eFuse_Addr++;
      }
    } else {
      offset = (uint8_t)((rtemp8 >> 4) & 0x0f);
      wren = (uint8_t)(rtemp8 & 0x0f);
    }

    if (offset < EFUSE_MAX_SECTION_JAGUAR) {
      /* Get word enable value from PG header */
      /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("Offset-%d Worden=%x\n", offset,
       * wren)); */

      for (i = 0; i < EFUSE_MAX_WORD_UNIT; i++) {
        /* Check word enable condition in the section */
        if (!((wren & 0x01) == 0x01)) {
          /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("Addr=%d\n", eFuse_Addr)); */
          _device.ReadEFuseByte(eFuse_Addr, &rtemp8);
          eFuse_Addr++;
          eFuseWord[offset][i] = (ushort)(rtemp8 & 0xff);

          if (eFuse_Addr >= EFUSE_REAL_CONTENT_LEN_JAGUAR)
            break;

          /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("Addr=%d", eFuse_Addr)); */
          _device.ReadEFuseByte(eFuse_Addr, &rtemp8);
          eFuse_Addr++;

          eFuseWord[offset][i] |= (ushort)(((rtemp8) << 8) & 0xff00);

          if (eFuse_Addr >= EFUSE_REAL_CONTENT_LEN_JAGUAR)
            break;
        }

        wren >>= 1;
      }
    } else {
      /* deal with error offset,skip error data		 */
      _logger->error("invalid offset:0x{:X}", offset);
      for (i = 0; i < EFUSE_MAX_WORD_UNIT; i++) {
        /* Check word enable condition in the section */
        if (!((wren & 0x01) == 0x01)) {
          eFuse_Addr++;
          if (eFuse_Addr >= EFUSE_REAL_CONTENT_LEN_JAGUAR)
            break;
          eFuse_Addr++;
          if (eFuse_Addr >= EFUSE_REAL_CONTENT_LEN_JAGUAR)
            break;
        }
      }
    }

    /* Read next PG header */
    _device.ReadEFuseByte(eFuse_Addr, &rtemp8);
    /* RTPRINT(FEEPROM, EFUSE_READ_ALL, ("Addr=%d rtemp 0x%x\n", eFuse_Addr,
     * *rtemp8)); */

    if (rtemp8 != 0xFF && (eFuse_Addr < EFUSE_REAL_CONTENT_LEN_JAGUAR)) {
      eFuse_Addr++;
    }
  }

  /*  */
  /* 3. Collect 16 sections and 4 word unit into Efuse map. */
  /*  */
  for (i = 0; i < EFUSE_MAX_SECTION_JAGUAR; i++) {
    for (j = 0; j < EFUSE_MAX_WORD_UNIT; j++) {
      efuseTbl[(i * 8) + (j * 2)] = (uint8_t)(eFuseWord[i][j] & 0xff);
      efuseTbl[(i * 8) + ((j * 2) + 1)] =
          (uint8_t)((eFuseWord[i][j] >> 8) & 0xff);
    }
  }

  /*  */
  /* 4. Copy from Efuse map to output pointer memory!!! */
  /*  */
  for (i = 0; i < _size_byte; i++) {
    pbuf[i] = efuseTbl[_offset + i];
  }

  /*  */
  /* 5. Calculate Efuse utilization. */
  /*  */
  // TODO: SetHwReg8812AU(HW_VARIABLES.HW_VAR_EFUSE_BYTES, (byte*)&eFuse_Addr);
  _logger->info("Hal_EfuseReadEFuse8812A: eFuse_Addr offset(0x{:X}) !!",
                eFuse_Addr);
}

#define EEPROM_TX_PWR_INX_8812 0x22

bool EepromManager::IsEfuseTxPowerInfoValid(uint8_t *efuseEepromData) {
  uint16_t tx_index_offset = EEPROM_TX_PWR_INX_8812;
  bool found= false;
  for (int index = 0; index < 11; index++) {
    if (efuseEepromData[tx_index_offset + index] != 0xFF) {
      found = true;
    }
  }

  return found;
}


void EepromManager::rtw_dump_cur_efuse() {
#if 0
#endif
}

#define RTL_EEPROM_ID 0x8129

void EepromManager::Hal_EfuseParseIDCode8812A() {
  u16 EEPROMId;

  /* Checl 0x8129 again for making sure autoload status!! */
  EEPROMId = ReadLE2Byte(&efuse_eeprom_data);
  if (EEPROMId != RTL_EEPROM_ID) {
    _logger->error("EEPROM ID({:x}) is invalid!!\n", EEPROMId);
    _device.AutoloadFailFlag = true;
  } else
    _device.AutoloadFailFlag = false;

  _logger->info("EEPROM ID=0x{:04x}", EEPROMId);
}

#define EEPROM_DEFAULT_VERSION 0
#define EEPROM_VERSION_8812 0xC4

uint8_t EepromManager::Hal_ReadPROMVersion8812A(RtlUsbAdapter device,
                                                uint8_t *efuse_eeprom_data) {
  uint8_t EEPROMVersion;
  if (device.AutoloadFailFlag) {
    EEPROMVersion = EEPROM_DEFAULT_VERSION;
  } else {
    EEPROMVersion = efuse_eeprom_data[EEPROM_VERSION_8812];

    if (EEPROMVersion == 0xFF) {
      EEPROMVersion = EEPROM_DEFAULT_VERSION;
    }
  }

  _logger->info("pHalData.EEPROMVersion is 0x{:X}", EEPROMVersion);
  return EEPROMVersion;
}

#define EEPROM_RF_BOARD_OPTION_8812 0xC1
#define EEPROM_DEFAULT_BOARD_OPTION 0x00

uint8_t EepromManager::Hal_ReadTxPowerInfo8812A(RtlUsbAdapter device,
                                                uint8_t *efuse_eeprom_data) {
  uint8_t EEPROMRegulatory;
  /* 2010/10/19 MH Add Regulator recognize for CU. */
  if (!device.AutoloadFailFlag) {

    if (efuse_eeprom_data[EEPROM_RF_BOARD_OPTION_8812] == 0xFF) {
      EEPROMRegulatory = (EEPROM_DEFAULT_BOARD_OPTION & 0x7); /* bit0~2 */
    } else {
      EEPROMRegulatory =
          (uint8_t)(efuse_eeprom_data[EEPROM_RF_BOARD_OPTION_8812] &
                    0x7); /* bit0~2 */
    }

  } else {
    EEPROMRegulatory = 0;
  }

  _logger->info("EEPROMRegulatory = 0x{:X}", (int)EEPROMRegulatory);

  return EEPROMRegulatory;
}

void EepromManager::Hal_EfuseParseBTCoexistInfo8812A() {
  if (!_device.AutoloadFailFlag) {
    auto tmp_u8 = efuse_eeprom_data[EEPROM_RF_BOARD_OPTION_8812];
    if (((tmp_u8 & 0xe0) >> 5) == 0x1) /* [7:5] */
    {
      EEPROMBluetoothCoexist = true;
    } else {
      EEPROMBluetoothCoexist = false;
    }
  } else {
    EEPROMBluetoothCoexist = false;
  }
}

#define EEPROM_XTAL_8812 0xB9
#define EEPROM_DEFAULT_CRYSTAL_CAP_8812 0x20

void EepromManager::Hal_EfuseParseXtal_8812A() {
  if (!_device.AutoloadFailFlag) {
    crystal_cap = efuse_eeprom_data[EEPROM_XTAL_8812];
    if (crystal_cap == 0xFF) {
      crystal_cap =
          EEPROM_DEFAULT_CRYSTAL_CAP_8812; /* what value should 8812 set? */
    }
  } else {
    crystal_cap = EEPROM_DEFAULT_CRYSTAL_CAP_8812;
  }

  _logger->info("crystal_cap: 0x{:X}", (int)crystal_cap);
}

#define EEPROM_THERMAL_METER_8812 0xBA
#define EEPROM_Default_ThermalMeter_8812 0x18

void EepromManager::Hal_ReadThermalMeter_8812A() {
  /*  */
  /* ThermalMeter from EEPROM */
  /*  */
  if (!_device.AutoloadFailFlag) {
    eeprom_thermal_meter = efuse_eeprom_data[EEPROM_THERMAL_METER_8812];
  } else {
    eeprom_thermal_meter = EEPROM_Default_ThermalMeter_8812;
  }
  /* pHalData.eeprom_thermal_meter = (tempval&0x1f);	 */ /* [4:0] */

  if (eeprom_thermal_meter == 0xff || _device.AutoloadFailFlag) {
    eeprom_thermal_meter = 0xFF;
  }

  /* pHalData.ThermalMeter[0] = pHalData.eeprom_thermal_meter;	 */
  _logger->info("ThermalMeter = 0x{:X}", (int)eeprom_thermal_meter);
}

void EepromManager::Hal_ReadAmplifierType_8812A() {
  uint8_t extTypePA_2G_A =
      ((efuse_eeprom_data[0xBD] & BIT2) >> 2); /* 0xBD[2] */
  uint8_t extTypePA_2G_B =
      ((efuse_eeprom_data[0xBD] & BIT6) >> 6); /* 0xBD[6] */
  uint8_t extTypePA_5G_A =
      ((efuse_eeprom_data[0xBF] & BIT2) >> 2); /* 0xBF[2] */
  uint8_t extTypePA_5G_B =
      ((efuse_eeprom_data[0xBF] & BIT6) >> 6); /* 0xBF[6] */
  uint8_t extTypeLNA_2G_A =
      ((efuse_eeprom_data[0xBD] & (BIT1 | BIT0)) >> 0); /* 0xBD[1:0] */
  uint8_t extTypeLNA_2G_B =
      ((efuse_eeprom_data[0xBD] & (BIT5 | BIT4)) >> 4); /* 0xBD[5:4] */
  uint8_t extTypeLNA_5G_A =
      ((efuse_eeprom_data[0xBF] & (BIT1 | BIT0)) >> 0); /* 0xBF[1:0] */
  uint8_t extTypeLNA_5G_B =
      ((efuse_eeprom_data[0xBF] & (BIT5 | BIT4)) >> 4); /* 0xBF[5:4] */

  hal_ReadPAType_8812A();

  if ((PAType_2G & (BIT5 | BIT4)) ==
      (BIT5 | BIT4)) /* [2.4G] Path A and B are both extPA */
  {
    TypeGPA = (ushort)(extTypePA_2G_B << 2 | extTypePA_2G_A);
  }

  if ((PAType_5G & (BIT1 | BIT0)) ==
      (BIT1 | BIT0)) /* [5G] Path A and B are both extPA */
  {
    TypeAPA = (ushort)(extTypePA_5G_B << 2 | extTypePA_5G_A);
  }

  if ((LNAType_2G & (BIT7 | BIT3)) ==
      (BIT7 | BIT3)) /* [2.4G] Path A and B are both extLNA */
  {
    TypeGLNA = (ushort)(extTypeLNA_2G_B << 2 | extTypeLNA_2G_A);
  }

  if ((LNAType_5G & (BIT7 | BIT3)) ==
      (BIT7 | BIT3)) /* [5G] Path A and B are both extLNA */
  {
    TypeALNA = (ushort)(extTypeLNA_5G_B << 2 | extTypeLNA_5G_A);
  }

  _logger->info("pHalData.TypeGPA = 0x{:X}", TypeGPA);
  _logger->info("pHalData.TypeAPA = 0x{:X}", TypeAPA);
  _logger->info("pHalData.TypeGLNA = 0x{:X}", TypeGLNA);
  _logger->info("pHalData.TypeALNA = 0x{:X}", TypeALNA);
}

#define EEPROM_PA_TYPE_8812AU 0xBC
#define EEPROM_LNA_TYPE_2G_8812AU 0xBD
#define EEPROM_LNA_TYPE_5G_8812AU 0xBF

void EepromManager::hal_ReadPAType_8812A() {
  if (!_device.AutoloadFailFlag) {
    if (registry_priv::AmplifierType_2G == 0) {
      /* AUTO */
      PAType_2G = efuse_eeprom_data[EEPROM_PA_TYPE_8812AU];
      LNAType_2G = efuse_eeprom_data[EEPROM_LNA_TYPE_2G_8812AU];
      if (PAType_2G == 0xFF) {
        PAType_2G = 0;
      }

      if (LNAType_2G == 0xFF) {
        LNAType_2G = 0;
      }

      ExternalPA_2G = ((PAType_2G & BIT5) != 0 && (PAType_2G & BIT4) != 0);
      ExternalLNA_2G = ((LNAType_2G & BIT7) != 0 && (LNAType_2G & BIT3) != 0);
    } else {
      ExternalPA_2G = (registry_priv::AmplifierType_2G & ODM_BOARD_EXT_PA) != 0;
      ExternalLNA_2G =
          (registry_priv::AmplifierType_2G & ODM_BOARD_EXT_LNA) != 0;
    }

    if (registry_priv::AmplifierType_5G == 0) {
      /* AUTO */
      PAType_5G = efuse_eeprom_data[EEPROM_PA_TYPE_8812AU];
      LNAType_5G = efuse_eeprom_data[EEPROM_LNA_TYPE_5G_8812AU];
      if (PAType_5G == 0xFF) {
        PAType_5G = 0;
      }

      if (LNAType_5G == 0xFF) {
        LNAType_5G = 0;
      }

      external_pa_5g = ((PAType_5G & BIT1) != 0 && (PAType_5G & BIT0) != 0);
      external_lna_5g = ((LNAType_5G & BIT7) != 0 && (LNAType_5G & BIT3) != 0);
    } else {
      external_pa_5g =
          (registry_priv::AmplifierType_5G & ODM_BOARD_EXT_PA_5G) != 0;
      external_lna_5g =
          (registry_priv::AmplifierType_5G & ODM_BOARD_EXT_LNA_5G) != 0;
    }
  } else {
    ExternalPA_2G = false;
    external_pa_5g = true;
    ExternalLNA_2G = false;
    external_lna_5g = true;

    if (registry_priv::AmplifierType_2G == 0) {
      /* AUTO */
      ExternalPA_2G = false;
      ExternalLNA_2G = false;
    } else {
      ExternalPA_2G = (registry_priv::AmplifierType_2G & ODM_BOARD_EXT_PA) != 0;
      ExternalLNA_2G =
          (registry_priv::AmplifierType_2G & ODM_BOARD_EXT_LNA) != 0;
    }

    if (registry_priv::AmplifierType_5G == 0) {
      /* AUTO */
      external_pa_5g = false;
      external_lna_5g = false;
    } else {
      external_pa_5g =
          (registry_priv::AmplifierType_5G & ODM_BOARD_EXT_PA_5G) != 0;
      external_lna_5g =
          (registry_priv::AmplifierType_5G & ODM_BOARD_EXT_LNA_5G) != 0;
    }
  }

  _logger->info("pHalData.PAType_2G is 0x{:X}, "
                "pHalData.ExternalPA_2G = {}",
                (int)PAType_2G, (int)ExternalPA_2G);
  _logger->info("pHalData.PAType_5G is 0x{:X}, "
                "pHalData.external_pa_5g = {}",
                (int)PAType_5G, (int)external_pa_5g);
  _logger->info("pHalData.LNAType_2G is 0x{:X}, "
                "pHalData.ExternalLNA_2G = {}",
                LNAType_2G, ExternalLNA_2G);
  _logger->info("pHalData.LNAType_5G is 0x{:X}, "
                "pHalData.external_lna_5g = {}",
                (int)LNAType_5G, (int)external_lna_5g);
}

#define EEPROM_RFE_OPTION_8812 0xCA

void EepromManager::Hal_ReadRFEType_8812A() {
  if (!_device.AutoloadFailFlag) {
    if ((registry_priv::RFE_Type != 64) ||
        0xFF == efuse_eeprom_data[EEPROM_RFE_OPTION_8812]) {
      if (registry_priv::RFE_Type != 64) {
        rfe_type = registry_priv::RFE_Type;
      } else {
        rfe_type = 0;
      }

    } else if ((efuse_eeprom_data[EEPROM_RFE_OPTION_8812] & BIT7) != 0) {
      if (external_lna_5g == true || external_lna_5g == 0 /*null*/) {
        if (external_pa_5g == true || external_pa_5g == 0 /*null*/) {
          if (ExternalLNA_2G && ExternalPA_2G) {
            rfe_type = 3;
          } else {
            rfe_type = 0;
          }
        } else {
          rfe_type = 2;
        }
      } else {
        rfe_type = 4;
      }
    } else {
      rfe_type = (ushort)(efuse_eeprom_data[EEPROM_RFE_OPTION_8812] & 0x3F);

      /* 2013/03/19 MH Due to othe customer already use incorrect EFUSE map */
      /* to for their product. We need to add workaround to prevent to modify */
      /* spec and notify all customer to revise the IC 0xca content. After */
      /* discussing with Willis an YN, revise driver code to prevent. */
      if (rfe_type == 4 &&
          (external_pa_5g == true || ExternalPA_2G == true ||
           external_lna_5g == true || ExternalLNA_2G == true)) {
        rfe_type = 0;
      }
    }
  } else {
    if (registry_priv::RFE_Type != 64) {
      rfe_type = registry_priv::RFE_Type;
    } else {
      rfe_type = 0;
    }
  }

  _logger->info("RFE Type: 0x{:X}", rfe_type);
}

#define EEPROM_USB_MODE_8812 0x08

void EepromManager::hal_ReadUsbType_8812AU() {
  /* if (IS_HARDWARE_TYPE_8812AU(adapterState) &&
   * adapterState.UsbModeMechanism.RegForcedUsbMode == 5) */
  {
    uint8_t reg_tmp, i, j, antenna = 0, wmode = 0;
    /* Read anenna type from EFUSE 1019/1018 */
    for (i = 0; i < 2; i++) {
      /*
        Check efuse address 1019
        Check efuse address 1018
      */
      _device.efuse_OneByteRead((ushort)(1019 - i), &reg_tmp);
      /*
        CHeck bit 7-5
        Check bit 3-1
      */
      if (((reg_tmp >> 5) & 0x7) != 0) {
        antenna = (uint8_t)((reg_tmp >> 5) & 0x7);
        break;
      } else if ((reg_tmp >> 1 & 0x07) != 0) {
        antenna = (uint8_t)((reg_tmp >> 1) & 0x07);
        break;
      }
    }

    /* Read anenna type from EFUSE 1021/1020 */
    for (i = 0; i < 2; i++) {
      /*
        Check efuse address 1021
        Check efuse address 1020
      */
      _device.efuse_OneByteRead((ushort)(1021 - i), &reg_tmp);

      /* CHeck bit 3-2 */
      if (((reg_tmp >> 2) & 0x3) != 0) {
        wmode = (uint8_t)((reg_tmp >> 2) & 0x3);
        break;
      }
    }

    _logger->info("antenna={}, wmode={}", (int)antenna, (int)wmode);
    /* Antenna == 1 WMODE = 3 RTL8812AU-VL 11AC + USB2.0 Mode */
    if (antenna == 1) {
      /* Config 8812AU as 1*1 mode AC mode. */
      version_id.RFType = RF_TYPE_1T1R;
      /* UsbModeSwitch_SetUsbModeMechOn(adapterState, FALSE); */
      /* pHalData.EFUSEHidden = EFUSE_HIDDEN_812AU_VL; */
      _logger->info("EFUSE_HIDDEN_812AU_VL");
    } else if (antenna == 2) {
      if (wmode == 3) {
        if (efuse_eeprom_data[EEPROM_USB_MODE_8812] == 0x2) {
          /* RTL8812AU Normal Mode. No further action. */
          /* pHalData.EFUSEHidden = EFUSE_HIDDEN_812AU; */
          _logger->info("EFUSE_HIDDEN_812AU");
        } else {
          /* Antenna == 2 WMODE = 3 RTL8812AU-VS 11AC + USB2.0 Mode */
          /* Driver will not support USB automatic switch */
          /* UsbModeSwitch_SetUsbModeMechOn(adapterState, FALSE); */
          /* pHalData.EFUSEHidden = EFUSE_HIDDEN_812AU_VS; */
          _logger->info("EFUSE_HIDDEN_8812AU_VS");
        }
      } else if (wmode == 2) {
        /* Antenna == 2 WMODE = 2 RTL8812AU-VN 11N only + USB2.0 Mode */
        /* UsbModeSwitch_SetUsbModeMechOn(adapterState, FALSE); */
        /* pHalData.EFUSEHidden = EFUSE_HIDDEN_812AU_VN; */
        _logger->info("EFUSE_HIDDEN_8812AU_VN");
      }
    }
  }
}

void EepromManager::efuse_ShadowRead1Byte(uint16_t Offset, uint8_t *Value) {
  *Value = efuse_eeprom_data[Offset];
}
