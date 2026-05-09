#include "HalModule.h"

#include "FirmwareManager.h"
#include "Hal8812PhyReg.h"
#include "phydm_pre_define.h"
#include "registry_priv.h"
#include "rtl8812a_hal.h"
#include "rtl8812a_spec.h"

#include <chrono>
#include <memory>
#include <thread>

#define DRVINFO_SZ 4

#define ARRAY_LENGTH(x) (sizeof(x) / sizeof((x)[0]))

HalModule::HalModule(
    RtlUsbAdapter device, std::shared_ptr<EepromManager> eepromManager,
    std::shared_ptr<RadioManagementModule> radioManagementModule,
    Logger_t logger)
    : _device{device}, _radioManagementModule{radioManagementModule},
      _eepromManager{eepromManager}, _logger{logger} {}

bool HalModule::rtw_hal_init(SelectedChannel selectedChannel) {
  auto status = rtl8812au_hal_init();

  if (status) {
    _radioManagementModule->init_hw_mlme_ext(selectedChannel);
    _radioManagementModule->SetMonitorMode();
  } else {
    _logger->error("rtw_hal_init: fail");
  }

  return status;
}

bool HalModule::rtl8812au_hal_init() {
  // Check if MAC has already power on. by tynli. 2011.05.27.
  auto value8 = _device.rtw_read8(REG_SYS_CLKR + 1);
  auto regCr = _device.rtw_read8(REG_CR);
  _logger->info("power-on :REG_SYS_CLKR 0x09=0x{:X}. REG_CR 0x100=0x{:X}",
                (int)value8, (int)regCr);
  if ((value8 & BIT3) != 0 && (regCr != 0 && regCr != 0xEA)) {
    /* pHalData.bMACFuncEnable = TRUE; */
    _logger->info("MAC has already power on");
  } else {
    /* pHalData.bMACFuncEnable = FALSE; */
    /* Set FwPSState to ALL_ON mode to prevent from the I/O be return because of
     * 32k */
    /* state which is set before sleep under wowlan mode. 2012.01.04. by tynli.
     */
    /* pHalData.FwPSState = FW_PS_STATE_ALL_ON_88E; */
    _logger->info("MAC has not been powered on yet");
  }

  _device.rtw_write8(REG_RF_CTRL, 5);
  _device.rtw_write8(REG_RF_CTRL, 7);
  _device.rtw_write8(REG_RF_B_CTRL_8812, 5);
  _device.rtw_write8(REG_RF_B_CTRL_8812, 7);

  // If HW didn't go through a complete de-initial procedure,
  // it probably occurs some problem for double initial procedure.
  // Like "CONFIG_DEINIT_BEFORE_INIT" in 92du chip
  _device.rtl8812au_hw_reset();

  auto initPowerOnStatus = InitPowerOn();
  if (initPowerOnStatus == false) {
    return false;
  }

  // ATTENTION!! BOUNDARY size depends on wifi_spec aka WMM or not WMM
  auto initLltTable8812AStatus = InitLLTTable8812A(TX_PAGE_BOUNDARY_8812);
  if (initLltTable8812AStatus == false) {
    return false;
  }

  _InitHardwareDropIncorrectBulkOut_8812A();

  auto fwManager = std::make_unique<FirmwareManager>(_device, _logger);
  fwManager->FirmwareDownload8812();

  PHY_MACConfig8812();

  _InitQueueReservedPage_8812AUsb();
  _InitTxBufferBoundary_8812AUsb();

  _InitQueuePriority_8812AUsb();
  _InitPageBoundary_8812AUsb();

  _InitTransferPageSize_8812AUsb();

  // Get Rx PHY status in order to report RSSI and others.
  _InitDriverInfoSize_8812A(DRVINFO_SZ);

  _InitInterrupt_8812AU();
  _InitNetworkType_8812A(); /* set msr	 */
  _InitWMACSetting_8812A();
  _InitAdaptiveCtrl_8812AUsb();
  _InitEDCA_8812AUsb();

  _InitRetryFunction_8812A();
  init_UsbAggregationSetting_8812A();

  _InitBeaconParameters_8812A();
  _InitBeaconMaxError_8812A();

  _InitBurstPktLen(); // added by page. 20110919

  // Init CR MACTXEN, MACRXEN after setting RxFF boundary REG_TRXFF_BNDY to
  // patch Hw bug which Hw initials RxFF boundry size to a value which is larger
  // than the real Rx buffer size in 88E. 2011.08.05. by tynli.
  value8 = _device.rtw_read8(REG_CR);
  _device.rtw_write8(REG_CR, (uint8_t)(value8 | MACTXEN | MACRXEN));

  _device.rtw_write16(REG_PKT_VO_VI_LIFE_TIME, 0x0400); /* unit: 256us. 256ms */
  _device.rtw_write16(REG_PKT_BE_BK_LIFE_TIME, 0x0400); /* unit: 256us. 256ms */

  auto bbConfig8812Status = PHY_BBConfig8812();
  if (bbConfig8812Status == false) {
    return false;
  }

  PHY_RF6052_Config_8812();

  if (_eepromManager->version_id.RFType == RF_TYPE_1T1R) {
    PHY_BB8812_Config_1T();
  }

  if (registry_priv::rf_config == RF_TYPE_1T2R) {
    _device.phy_set_bb_reg(rTxPath_Jaguar, bMaskLWord, 0x1111);
  }

  if (registry_priv::channel <= 14) {
    _radioManagementModule->PHY_SwitchWirelessBand8812(BandType::BAND_ON_2_4G);
  } else {
    _radioManagementModule->PHY_SwitchWirelessBand8812(BandType::BAND_ON_5G);
  }

  _radioManagementModule->rtw_hal_set_chnl_bw(
      registry_priv::channel, ChannelWidth_t::CHANNEL_WIDTH_20,
      HAL_PRIME_CHNL_OFFSET_DONT_CARE, HAL_PRIME_CHNL_OFFSET_DONT_CARE);

  // HW SEQ CTRL
  // set 0x0 to 0xFF by tynli. Default enable HW SEQ NUM.
  _device.rtw_write8(REG_HWSEQ_CTRL, 0xFF);

  // Disable BAR, suggested by Scott
  // 2010.04.09 add by hpfan
  _device.rtw_write32(REG_BAR_MODE_CTRL, 0x0201ffff);

  if (registry_priv::wifi_spec) {
    _device.rtw_write16(REG_FAST_EDCA_CTRL, 0);
  }

  // Nav limit , suggest by scott
  _device.rtw_write8(0x652, 0x0);

  /* 0x4c6[3] 1: RTS BW = Data BW */
  /* 0: RTS BW depends on CCA / secondary CCA result. */
  _device.rtw_write8(REG_QUEUE_CTRL,
                     (uint8_t)(_device.rtw_read8(REG_QUEUE_CTRL) & 0xF7));

  /* enable Tx report. */
  _device.rtw_write8(REG_FWHW_TXQ_CTRL + 1, 0x0F);

  /* Suggested by SD1 pisa. Added by tynli. 2011.10.21. */
  _device.rtw_write8(REG_EARLY_MODE_CONTROL_8812 + 3,
                     0x01); /* Pretx_en, for WEP/TKIP SEC */

  /* tynli_test_tx_report. */
  _device.rtw_write16(REG_TX_RPT_TIME, 0x3DF0);

  /* Reset USB mode switch setting */
  _device.rtw_write8(REG_SDIO_CTRL_8812, 0x0);
  _device.rtw_write8(REG_ACLK_MON, 0x0);

  _device.rtw_write8(REG_USB_HRPWM, 0);

  // TODO:
  ///* ack for xmit mgmt frames. */
  _device.rtw_write32(REG_FWHW_TXQ_CTRL,
                      _device.rtw_read32(REG_FWHW_TXQ_CTRL) | BIT12);
  return true;
}

bool HalModule::InitPowerOn() {
  if (_macPwrCtrlOn) {
    return true;
  }

  if (!HalPwrSeqCmdParsing(Rtl8812_NIC_ENABLE_FLOW)) {
    _logger->error("InitPowerOn: run power on flow fail");
    return false;
  }

  /* Enable MAC DMA/WMAC/SCHEDULE/SEC block */
  /* Set CR bit10 to enable 32k calibration. Suggested by SD1 Gimmy. Added by
   * tynli. 2011.08.31. */
  _device.rtw_write16(REG_CR,
                      0x00); /* suggseted by zhouzhou, by page, 20111230 */
  uint16_t u2btmp = _device.rtw_read16(REG_CR);
  u2btmp |= (ushort)(HCI_TXDMA_EN | HCI_RXDMA_EN | TXDMA_EN | RXDMA_EN |
                     PROTOCOL_EN | SCHEDULE_EN | ENSEC | CALTMR_EN);
  _device.rtw_write16(REG_CR, u2btmp);

  _macPwrCtrlOn = true;
  return true;
}

bool HalModule::InitLLTTable8812A(uint8_t txpktbuf_bndy) {
  bool status;
  for (uint32_t i = 0; i < (txpktbuf_bndy - 1); i++) {
    status = _LLTWrite_8812A(i, i + 1);
    if (true != status) {
      return false;
    }
  }

  /* end of list */
  status = _LLTWrite_8812A((uint32_t)(txpktbuf_bndy - 1), 0xFF);
  if (status == false) {
    return false;
  }

  /* Make the other pages as ring buffer */
  /* This ring buffer is used as beacon buffer if we config this MAC as two MAC
   * transfer. */
  /* Otherwise used as local loopback buffer. */
  uint32_t Last_Entry_Of_TxPktBuf = LAST_ENTRY_OF_TX_PKT_BUFFER_8812;
  for (uint32_t i = txpktbuf_bndy; i < Last_Entry_Of_TxPktBuf; i++) {
    status = _LLTWrite_8812A(i, (i + 1));
    if (status == false) {
      return false;
    }
  }

  /* Let last entry point to the start entry of ring buffer */
  status = _LLTWrite_8812A(Last_Entry_Of_TxPktBuf, txpktbuf_bndy);
  if (status == false) {
    return false;
  }

  return true;
}

bool HalModule::_LLTWrite_8812A(uint32_t address, uint32_t data) {
  bool status = true;
  int32_t count = 0;
  uint32_t value = _LLT_INIT_ADDR(address) | _LLT_INIT_DATA(data) |
                   _LLT_OP(_LLT_WRITE_ACCESS);

  _device.rtw_write32(REG_LLT_INIT, value);

  /* polling */
  do {
    value = _device.rtw_read32(REG_LLT_INIT);
    if (_LLT_NO_ACTIVE == _LLT_OP_VALUE(value)) {
      break;
    }

    if (count > POLLING_LLT_THRESHOLD) {
      status = false;
      break;
    }

    ++count;
  } while (true);

  return status;
}

void HalModule::_InitHardwareDropIncorrectBulkOut_8812A() {
  uint32_t value32 = _device.rtw_read32(REG_TXDMA_OFFSET_CHK);
  value32 |= DROP_DATA_EN;
  _device.rtw_write32(REG_TXDMA_OFFSET_CHK, value32);
}

bool HalModule::HalPwrSeqCmdParsing(WLAN_PWR_CFG *PwrSeqCmd) {
  bool bHWICSupport = false;
  uint32_t AryIdx = 0;
  // UInt16 offset = 0;
  uint32_t pollingCount = 0; /* polling autoload done. */

  do {
    auto PwrCfgCmd = PwrSeqCmd[AryIdx];

    /* 2 Only Handle the command whose FAB, CUT, and Interface are matched */
    // if ((GET_PWR_CFG_FAB_MASK(PwrCfgCmd) & FabVersion) &&
    //     (GET_PWR_CFG_CUT_MASK(PwrCfgCmd) & CutVersion) &&
    //     (GET_PWR_CFG_INTF_MASK(PwrCfgCmd) & InterfaceType))
    switch (PwrCfgCmd.cmd) {
    case PWR_CMD_READ:
      break;

    case PWR_CMD_WRITE: {
      auto offset = PwrCfgCmd.offset;
      /* Read the value from system register */
      auto currentOffsetValue = _device.rtw_read8(offset);

      currentOffsetValue =
          (uint8_t)(currentOffsetValue & (uint8_t)(~PwrCfgCmd.msk));
      currentOffsetValue =
          (uint8_t)(currentOffsetValue | ((PwrCfgCmd.value) & (PwrCfgCmd.msk)));

      /* Write the value back to sytem register */
      _device.rtw_write8(offset, currentOffsetValue);
    } break;

    case PWR_CMD_POLLING:

    {
      auto bPollingBit = false;
      auto offset = (PwrCfgCmd.offset);
      uint32_t maxPollingCnt = 5000;
      bool flag = false;

      maxPollingCnt = 5000;

      do {
        auto value = _device.rtw_read8(offset);

        value = (uint8_t)(value & PwrCfgCmd.msk);
        if (value == ((PwrCfgCmd.value) & PwrCfgCmd.msk)) {
          bPollingBit = true;
        } else {
          using namespace std::chrono_literals;
          std::this_thread::sleep_for(10ms);
        }

        if (pollingCount++ > maxPollingCnt) {
          // TODO: RTW_ERR("HalPwrSeqCmdParsing: Fail to polling
          // Offset[%#x]=%02x\n", offset, value);

          /* For PCIE + USB package poll power bit timeout issue only modify
           * 8821AE and 8723BE */
          if (bHWICSupport && offset == 0x06 && flag == false) {

            // TODO: RTW_ERR("[WARNING] PCIE polling(0x%X) timeout(%d), Toggle
            // 0x04[3] and try again.\n", offset, maxPollingCnt);

            _device.rtw_write8(0x04, (uint8_t)(_device.rtw_read8(0x04) | BIT3));
            _device.rtw_write8(0x04,
                               (uint8_t)(_device.rtw_read8(0x04) & ~BIT3));

            /* Retry Polling Process one more time */
            pollingCount = 0;
            flag = true;
          } else {
            return false;
          }
        }
      } while (!bPollingBit);
    }

    break;

    case PWR_CMD_DELAY: {
      if (PwrCfgCmd.value == (uint8_t)PWRSEQ_DELAY_US) {
        std::this_thread::sleep_for(
            std::chrono::microseconds(PwrCfgCmd.offset));
      } else {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(PwrCfgCmd.offset));
      }
    } break;

    case PWR_CMD_END:
      /* When this command is parsed, end the process */
      return true;
      break;

    default:
      break;
    }

    AryIdx++; /* Add Array Index */
  } while (true);

  return true;
}

void HalModule::PHY_MACConfig8812() { odm_read_and_config_mp_8812a_mac_reg(); }

/******************************************************************************
 *                           mac_reg.TXT
 ******************************************************************************/

static u32 array_mp_8812a_mac_reg[] = {
    0x010,      0x0000000C, 0x80000200, 0x00000000, 0x40000000, 0x00000000,
    0x011,      0x00000066, 0xA0000000, 0x00000000, 0x011,      0x0000005A,
    0xB0000000, 0x00000000, 0x025,      0x0000000F, 0x072,      0x00000000,
    0x420,      0x00000080, 0x428,      0x0000000A, 0x429,      0x00000010,
    0x430,      0x00000000, 0x431,      0x00000000, 0x432,      0x00000000,
    0x433,      0x00000001, 0x434,      0x00000002, 0x435,      0x00000003,
    0x436,      0x00000005, 0x437,      0x00000007, 0x438,      0x00000000,
    0x439,      0x00000000, 0x43A,      0x00000000, 0x43B,      0x00000001,
    0x43C,      0x00000002, 0x43D,      0x00000003, 0x43E,      0x00000005,
    0x43F,      0x00000007, 0x440,      0x0000005D, 0x441,      0x00000001,
    0x442,      0x00000000, 0x444,      0x00000010, 0x445,      0x00000000,
    0x446,      0x00000000, 0x447,      0x00000000, 0x448,      0x00000000,
    0x449,      0x000000F0, 0x44A,      0x0000000F, 0x44B,      0x0000003E,
    0x44C,      0x00000010, 0x44D,      0x00000000, 0x44E,      0x00000000,
    0x44F,      0x00000000, 0x450,      0x00000000, 0x451,      0x000000F0,
    0x452,      0x0000000F, 0x453,      0x00000000, 0x45B,      0x00000080,
    0x460,      0x00000066, 0x461,      0x00000066, 0x4C8,      0x000000FF,
    0x4C9,      0x00000008, 0x4CC,      0x000000FF, 0x4CD,      0x000000FF,
    0x4CE,      0x00000001, 0x500,      0x00000026, 0x501,      0x000000A2,
    0x502,      0x0000002F, 0x503,      0x00000000, 0x504,      0x00000028,
    0x505,      0x000000A3, 0x506,      0x0000005E, 0x507,      0x00000000,
    0x508,      0x0000002B, 0x509,      0x000000A4, 0x50A,      0x0000005E,
    0x50B,      0x00000000, 0x50C,      0x0000004F, 0x50D,      0x000000A4,
    0x50E,      0x00000000, 0x50F,      0x00000000, 0x512,      0x0000001C,
    0x514,      0x0000000A, 0x516,      0x0000000A, 0x525,      0x0000004F,
    0x550,      0x00000010, 0x551,      0x00000010, 0x559,      0x00000002,
    0x55C,      0x00000050, 0x55D,      0x000000FF, 0x604,      0x00000009,
    0x605,      0x00000030, 0x607,      0x00000003, 0x608,      0x0000000E,
    0x609,      0x0000002A, 0x620,      0x000000FF, 0x621,      0x000000FF,
    0x622,      0x000000FF, 0x623,      0x000000FF, 0x624,      0x000000FF,
    0x625,      0x000000FF, 0x626,      0x000000FF, 0x627,      0x000000FF,
    0x638,      0x00000050, 0x63C,      0x0000000A, 0x63D,      0x0000000A,
    0x63E,      0x0000000E, 0x63F,      0x0000000E, 0x640,      0x00000080,
    0x642,      0x00000040, 0x643,      0x00000000, 0x652,      0x000000C8,
    0x66E,      0x00000005, 0x700,      0x00000021, 0x701,      0x00000043,
    0x702,      0x00000065, 0x703,      0x00000087, 0x708,      0x00000021,
    0x709,      0x00000043, 0x70A,      0x00000065, 0x70B,      0x00000087,
    0x718,      0x00000040,

};

#define COND_ELSE 2
#define COND_ENDIF 3

void HalModule::odm_read_and_config_mp_8812a_mac_reg() {
  u32 i = 0;
  u8 c_cond;
  bool is_matched = true, is_skipped = false;
  u32 array_len = sizeof(array_mp_8812a_mac_reg) / sizeof(u32);
  u32 *array = array_mp_8812a_mac_reg;

  u32 v1 = 0, v2 = 0, pre_v1 = 0, pre_v2 = 0;

  // PHYDM_DBG(p_dm, ODM_COMP_INIT,
  //("===> odm_read_and_config_mp_8812a_mac_reg\n"));

  while ((i + 1) < array_len) {
    v1 = array[i];
    v2 = array[i + 1];

    if (v1 & (BIT(31) | BIT(30))) { /*positive & negative condition*/
      if (v1 & BIT(31)) {           /* positive condition*/
        c_cond = (u8)((v1 & (BIT(29) | BIT(28))) >> 28);
        if (c_cond == COND_ENDIF) { /*end*/
          is_matched = true;
          is_skipped = false;
          // PHYDM_DBG(p_dm, ODM_COMP_INIT, ("ENDIF\n"));
        } else if (c_cond == COND_ELSE) { /*else*/
          is_matched = is_skipped ? false : true;
          // PHYDM_DBG(p_dm, ODM_COMP_INIT, ("ELSE\n"));
        } else { /*if , else if*/
          pre_v1 = v1;
          pre_v2 = v2;
          // PHYDM_DBG(p_dm, ODM_COMP_INIT, ("IF or ELSE IF\n"));
        }
      } else if (v1 & BIT(30)) { /*negative condition*/
        if (is_skipped == false) {
          if (check_positive(pre_v1, pre_v2, v2)) {
            is_matched = true;
            is_skipped = true;
          } else {
            is_matched = false;
            is_skipped = false;
          }
        } else
          is_matched = false;
      }
    } else {
      if (is_matched) {
        ushort addr = (uint16_t)v1;
        uint8_t data = (uint8_t)v2;
        odm_write_1byte(addr, data);
      }
    }
    i = i + 2;
  }
}

void HalModule::odm_write_1byte(uint16_t reg_addr, uint8_t data) {
  _device.rtw_write8(reg_addr, data);
}

#define RTL871X_HCI_TYPE_RTW_USB BIT1

bool HalModule::check_positive(int32_t condition1, int32_t condition2,
                               int32_t condition4) {
  auto originalBoardType = _eepromManager->GetBoardType();

  u8 boardType = ((originalBoardType & BIT(4)) >> 4) << 0 | /* _GLNA*/
                 ((originalBoardType & BIT(3)) >> 3) << 1 | /* _GPA*/
                 ((originalBoardType & BIT(7)) >> 7) << 2 | /* _ALNA*/
                 ((originalBoardType & BIT(6)) >> 6) << 3 | /* _APA */
                 ((originalBoardType & BIT(2)) >> 2) << 4 | /* _BT*/
                 ((originalBoardType & BIT(1)) >> 1) << 5 | /* _NGFF*/
                 ((originalBoardType & BIT(5)) >> 5) << 6;  /* _TRSWT*/

  uint32_t cond1 = condition1;
  uint32_t cond2 = condition2;
  uint32_t cond4 = condition4;

  uint32_t cut_version_for_para = (_eepromManager->cut_version == ODM_CUT_A)
                                  ? (uint32_t)15
                                  : (uint32_t)_eepromManager->version_id.CUTVersion;
  uint32_t pkg_type_for_para = 15;

  uint32_t driver1 = cut_version_for_para << 24 |
                     ((uint32_t)RTL871X_HCI_TYPE_RTW_USB & 0xF0) << 16 |
                     pkg_type_for_para << 12 |
                     ((uint32_t)RTL871X_HCI_TYPE_RTW_USB & 0x0F) << 8 | boardType;

  uint32_t driver2 = ((uint32_t)_eepromManager->TypeGLNA & 0xFF) << 0 |
                     ((uint32_t)_eepromManager->TypeGPA & 0xFF) << 8 |
                     ((uint32_t)_eepromManager->TypeALNA & 0xFF) << 16 |
                     ((uint32_t)_eepromManager->TypeAPA & 0xFF) << 24;

  uint32_t driver4 = ((uint32_t)_eepromManager->TypeGLNA & 0xFF00) >> 8 |
                     ((uint32_t)_eepromManager->TypeGPA & 0xFF00) |
                     ((uint32_t)_eepromManager->TypeALNA & 0xFF00) << 8 |
                     ((uint32_t)_eepromManager->TypeAPA & 0xFF00) << 16;

  /*============== value Defined Check ===============*/
  /*QFN type [15:12] and cut version [27:24] need to do value check*/

  if (((cond1 & 0x0000F000) != 0) &&
      ((cond1 & 0x0000F000) != (driver1 & 0x0000F000))) {
    return false;
  }

  if (((cond1 & 0x0F000000) != 0) &&
      ((cond1 & 0x0F000000) != (driver1 & 0x0F000000))) {
    return false;
  }

  /*=============== Bit Defined Check ================*/
  /* We don't care [31:28] */

  cond1 &= 0x00FF0FFF;
  driver1 &= 0x00FF0FFF;

  if ((cond1 & driver1) == cond1) {
    uint32_t bit_mask = 0;

    if ((cond1 & 0x0F) == 0) /* board_type is DONTCARE*/
    {
      return true;
    }

    if ((cond1 & BIT0) != 0) /*GLNA*/
    {
      bit_mask |= 0x000000FF;
    }
    if ((cond1 & BIT1) != 0) /*GPA*/
    {
      bit_mask |= 0x0000FF00;
    }
    if ((cond1 & BIT2) != 0) /*ALNA*/
    {
      bit_mask |= 0x00FF0000;
    }
    if ((cond1 & BIT3) != 0) /*APA*/
    {
      bit_mask |= 0xFF000000;
    }

    if (((cond2 & bit_mask) == (driver2 & bit_mask)) &&
        ((cond4 & bit_mask) ==
         (driver4 & bit_mask))) /* board_type of each RF path is matched*/
    {
      return true;
    } else {
      return false;
    }
  } else {
    return false;
  }
}

void HalModule::_InitQueueReservedPage_8812AUsb() {
  uint32_t numHQ = 0;
  uint32_t numLQ = 0;
  uint32_t numNQ = 0;
  uint32_t value32;
  uint8_t value8;
  bool bWiFiConfig = registry_priv::wifi_spec;

  if (!bWiFiConfig) {
    if (_device.OutEpQueueSel & TxSele::TX_SELE_HQ) {
      numHQ = NORMAL_PAGE_NUM_HPQ_8812;
    }

    if (_device.OutEpQueueSel & TxSele::TX_SELE_LQ) {
      numLQ = NORMAL_PAGE_NUM_LPQ_8812;
    }

    /* NOTE: This step shall be proceed before writting REG_RQPN.
     */
    if (_device.OutEpQueueSel & TxSele::TX_SELE_NQ) {
      numNQ = NORMAL_PAGE_NUM_NPQ_8812;
    }
  } else {
    /* WMM		 */
    if (_device.OutEpQueueSel & TxSele::TX_SELE_HQ) {
      numHQ = WMM_NORMAL_PAGE_NUM_HPQ_8812;
    }

    if (_device.OutEpQueueSel & TxSele::TX_SELE_LQ) {
      numLQ = WMM_NORMAL_PAGE_NUM_LPQ_8812;
    }

    /* NOTE: This step shall be proceed before writting REG_RQPN.
     */
    if (_device.OutEpQueueSel & TxSele::TX_SELE_NQ) {
      numNQ = WMM_NORMAL_PAGE_NUM_NPQ_8812;
    }
  }

  uint32_t numPubQ = TX_TOTAL_PAGE_NUMBER_8812 - numHQ - numLQ - numNQ;

  value8 = (uint8_t)_NPQ(numNQ);
  _device.rtw_write8(REG_RQPN_NPQ, value8);

  /* TX DMA */
  value32 = _HPQ(numHQ) | _LPQ(numLQ) | _PUBQ(numPubQ) | LD_RQPN;
  _device.rtw_write32(REG_RQPN, value32);
}

void HalModule::_InitTxBufferBoundary_8812AUsb() {
  uint8_t txPageBoundary8812 = TX_PAGE_BOUNDARY_8812;

  _device.rtw_write8(REG_BCNQ_BDNY, txPageBoundary8812);
  _device.rtw_write8(REG_MGQ_BDNY, txPageBoundary8812);
  _device.rtw_write8(REG_WMAC_LBK_BF_HD, txPageBoundary8812);
  _device.rtw_write8(REG_TRXFF_BNDY, txPageBoundary8812);
  _device.rtw_write8(REG_TDECTRL + 1, txPageBoundary8812);
}

void HalModule::_InitQueuePriority_8812AUsb() {
  switch (_device.OutEpNumber) {
  case 2:
    _InitNormalChipTwoOutEpPriority_8812AUsb();
    break;
  case 3:
    _InitNormalChipThreeOutEpPriority_8812AUsb();
    break;
  case 4:
    _InitNormalChipFourOutEpPriority_8812AUsb();
    break;
  default:
    _logger->error("_InitQueuePriority_8812AUsb(): Shall not reach here!");
    break;
  }
}

void HalModule::_InitNormalChipTwoOutEpPriority_8812AUsb() {
  uint16_t valueHi;
  uint16_t valueLow;

  switch (_device.OutEpQueueSel) {
  case (TxSele::TX_SELE_HQ | TxSele::TX_SELE_LQ):
    valueHi = QUEUE_HIGH;
    valueLow = QUEUE_LOW;
    break;
  case (TxSele::TX_SELE_NQ | TxSele::TX_SELE_LQ):
    valueHi = QUEUE_NORMAL;
    valueLow = QUEUE_LOW;
    break;
  case (TxSele::TX_SELE_HQ | TxSele::TX_SELE_NQ):
    valueHi = QUEUE_HIGH;
    valueLow = QUEUE_NORMAL;
    break;
  default:
    valueHi = QUEUE_HIGH;
    valueLow = QUEUE_NORMAL;
    break;
  }

  uint16_t beQ, bkQ, viQ, voQ, mgtQ, hiQ;
  if (!registry_priv::wifi_spec) {
    beQ = valueLow;
    bkQ = valueLow;
    viQ = valueHi;
    voQ = valueHi;
    mgtQ = valueHi;
    hiQ = valueHi;
  } else {
    /* for WMM ,CONFIG_OUT_EP_WIFI_MODE */
    beQ = valueLow;
    bkQ = valueHi;
    viQ = valueHi;
    voQ = valueLow;
    mgtQ = valueHi;
    hiQ = valueHi;
  }

  _InitNormalChipRegPriority_8812AUsb(beQ, bkQ, viQ, voQ, mgtQ, hiQ);
}

void HalModule::_InitNormalChipRegPriority_8812AUsb(uint16_t beQ, uint16_t bkQ,
                                                    uint16_t viQ, uint16_t voQ,
                                                    uint16_t mgtQ,
                                                    uint16_t hiQ) {
  uint16_t value16 = (uint16_t)(_device.rtw_read16(REG_TRXDMA_CTRL) & 0x7);

  value16 = (uint16_t)(value16 | _TXDMA_BEQ_MAP(beQ) | _TXDMA_BKQ_MAP(bkQ) |
                       _TXDMA_VIQ_MAP(viQ) | _TXDMA_VOQ_MAP(voQ) |
                       _TXDMA_MGQ_MAP(mgtQ) | _TXDMA_HIQ_MAP(hiQ));

  _device.rtw_write16(REG_TRXDMA_CTRL, value16);
}

void HalModule::_InitNormalChipThreeOutEpPriority_8812AUsb() {
  uint16_t beQ, bkQ, viQ, voQ, mgtQ, hiQ;

  if (!registry_priv::wifi_spec) {
    /* typical setting */
    beQ = QUEUE_LOW;
    bkQ = QUEUE_LOW;
    viQ = QUEUE_NORMAL;
    voQ = QUEUE_HIGH;
    mgtQ = QUEUE_HIGH;
    hiQ = QUEUE_HIGH;
  } else {
    /* for WMM */
    beQ = QUEUE_LOW;
    bkQ = QUEUE_NORMAL;
    viQ = QUEUE_NORMAL;
    voQ = QUEUE_HIGH;
    mgtQ = QUEUE_HIGH;
    hiQ = QUEUE_HIGH;
  }

  _InitNormalChipRegPriority_8812AUsb(beQ, bkQ, viQ, voQ, mgtQ, hiQ);
}

void HalModule::_InitNormalChipFourOutEpPriority_8812AUsb() {
  uint16_t beQ, bkQ, viQ, voQ, mgtQ, hiQ;

  if (!registry_priv::wifi_spec) {
    /* typical setting */
    beQ = QUEUE_LOW;
    bkQ = QUEUE_LOW;
    viQ = QUEUE_NORMAL;
    voQ = QUEUE_NORMAL;
    mgtQ = QUEUE_EXTRA;
    hiQ = QUEUE_HIGH;
  } else {
    /* for WMM */
    beQ = QUEUE_LOW;
    bkQ = QUEUE_NORMAL;
    viQ = QUEUE_NORMAL;
    voQ = QUEUE_HIGH;
    mgtQ = QUEUE_HIGH;
    hiQ = QUEUE_HIGH;
  }

  _InitNormalChipRegPriority_8812AUsb(beQ, bkQ, viQ, voQ, mgtQ, hiQ);
  init_hi_queue_config_8812a_usb();
}

#define REG_HIQ_NO_LMT_EN 0x05A7

void HalModule::init_hi_queue_config_8812a_usb() {
  /* Packet in Hi Queue Tx immediately (No constraint for ATIM Period)*/
  _device.rtw_write8(REG_HIQ_NO_LMT_EN, 0xFF);
}

void HalModule::_InitPageBoundary_8812AUsb() {
  _device.rtw_write16((REG_TRXFF_BNDY + 2), RX_DMA_BOUNDARY_8812);
}

void HalModule::_InitTransferPageSize_8812AUsb() {
  uint8_t value8 = _PSTX(PBP_512);
  _device.rtw_write8(REG_PBP, value8);
}

void HalModule::_InitDriverInfoSize_8812A(uint8_t drvInfoSize) {
  _device.rtw_write8(REG_RX_DRVINFO_SZ, drvInfoSize);
}

void HalModule::_InitInterrupt_8812AU() {
  /* HIMR */
  _device.rtw_write32(REG_HIMR0_8812, _intrMask[0] & 0xFFFFFFFF);
  _device.rtw_write32(REG_HIMR1_8812, _intrMask[1] & 0xFFFFFFFF);
}

void HalModule::_InitNetworkType_8812A() {
  auto value32 = _device.rtw_read32(REG_CR);
  /* TODO: use the other function to set network type */
  value32 = (value32 & ~MASK_NETTYPE) | _NETTYPE(NT_LINK_AP);

  _device.rtw_write32(REG_CR, value32);
}

void HalModule::_InitWMACSetting_8812A() {
  /* rcr = AAP | APM | AM | AB | APP_ICV | ADF | AMF | APP_FCS | HTC_LOC_CTRL |
   * APP_MIC | APP_PHYSTS; */
  uint32_t rcr = RCR_APM | RCR_AM | RCR_AB | RCR_CBSSID_DATA | RCR_CBSSID_BCN |
                 RCR_APP_ICV | RCR_AMF | RCR_HTC_LOC_CTRL | RCR_APP_MIC |
                 RCR_APP_PHYST_RXFF | RCR_APPFCS | FORCEACK;

  _radioManagementModule->hw_var_rcr_config(rcr);

  /* Accept all multicast address */
  _device.rtw_write32(REG_MAR, 0xFFFFFFFF);
  _device.rtw_write32(REG_MAR + 4, 0xFFFFFFFF);

  uint32_t value16 = BIT10 | BIT5;
  _device.rtw_write16(REG_RXFLTMAP1, (uint16_t)value16);
}

void HalModule::_InitAdaptiveCtrl_8812AUsb() {
  /* Response Rate Set */
  uint32_t value32 = _device.rtw_read32(REG_RRSR);
  value32 &= ~RATE_BITMAP_ALL;

  value32 |= RATE_RRSR_WITHOUT_CCK;
  value32 |= RATE_RRSR_CCK_ONLY_1M;
  _device.rtw_write32(REG_RRSR, value32);

  /* CF-END Threshold */
  /* m_spIoBase.rtw_write8(REG_CFEND_TH, 0x1); */

  /* SIFS (used in NAV) */
  uint16_t value16 = (uint16_t)(_SPEC_SIFS_CCK(0x10) | _SPEC_SIFS_OFDM(0x10));
  _device.rtw_write16(REG_SPEC_SIFS, value16);

  /* Retry Limit */
  value16 = _LRL(RL_VAL_STA) | _SRL(RL_VAL_STA);
  _device.rtw_write16(REG_RL, value16);
}

void HalModule::_InitEDCA_8812AUsb() {
  /* Set Spec SIFS (used in NAV) */
  _device.rtw_write16(REG_SPEC_SIFS, 0x100a);
  _device.rtw_write16(REG_MAC_SPEC_SIFS, 0x100a);

  /* Set SIFS for CCK */
  _device.rtw_write16(REG_SIFS_CTX, 0x100a);

  /* Set SIFS for OFDM */
  _device.rtw_write16(REG_SIFS_TRX, 0x100a);

  /* TXOP */
  _device.rtw_write32(REG_EDCA_BE_PARAM, 0x005EA42B);
  _device.rtw_write32(REG_EDCA_BK_PARAM, 0x0000A44F);
  _device.rtw_write32(REG_EDCA_VI_PARAM, 0x005EA324);
  _device.rtw_write32(REG_EDCA_VO_PARAM, 0x002FA226);

  /* 0x50 for 80MHz clock */
  _device.rtw_write8(REG_USTIME_TSF, 0x50);
  _device.rtw_write8(REG_USTIME_EDCA, 0x50);
}

void HalModule::_InitRetryFunction_8812A() {
  uint32_t value8;

  value8 = _device.rtw_read8(REG_FWHW_TXQ_CTRL);
  value8 |= EN_AMPDU_RTY_NEW;
  _device.rtw_write8(REG_FWHW_TXQ_CTRL, (uint8_t)value8);

  /* Set ACK timeout */
  /* rtw_write8(adapterState, REG_ACKTO, 0x40);  */ /* masked by page for BCM
                                                       IOT issue temporally */
  _device.rtw_write8(REG_ACKTO, 0x80);
}

void HalModule::init_UsbAggregationSetting_8812A() {
  ///* Tx aggregation setting */
  usb_AggSettingTxUpdate_8812A();

  ///* Rx aggregation setting */
  usb_AggSettingRxUpdate_8812A();
}

void HalModule::usb_AggSettingTxUpdate_8812A() {
  if (_usbTxAggMode) {
    uint32_t value32 = _device.rtw_read32(REG_TDECTRL);
    value32 = value32 & ~(BLK_DESC_NUM_MASK << BLK_DESC_NUM_SHIFT);
    value32 |= ((_usbTxAggDescNum & BLK_DESC_NUM_MASK) << BLK_DESC_NUM_SHIFT);

    _device.rtw_write32(REG_DWBCN0_CTRL_8812, value32);
    // if (IS_HARDWARE_TYPE_8821U(adapterState))   /* page added for Jaguar */
    //     rtw_write8(adapterState, REG_DWBCN1_CTRL_8812,
    //     pHalData._usbTxAggDescNum << 1);
  }
}

void HalModule::usb_AggSettingRxUpdate_8812A() {
  uint32_t valueDMA = _device.rtw_read8(REG_TRXDMA_CTRL);
  switch (_rxAggMode) {
  case RX_AGG_DMA:
    valueDMA |= RXDMA_AGG_EN;
    /* 2012/10/26 MH For TX through start rate temp fix. */
    {
      uint16_t temp;

      /* Adjust DMA page and thresh. */
      temp = (uint16_t)(_rxAggDmaSize | (_rxAggDmaTimeout << 8));
      _device.rtw_write16(REG_RXDMA_AGG_PG_TH, temp);
      _device.rtw_write8(
          REG_RXDMA_AGG_PG_TH + 3,
          (uint8_t)
              BIT7); /* for dma agg , 0x280[31]GBIT_RXDMA_AGG_OLD_MOD, set 1 */
    }
    break;
  case RX_AGG_USB:
    valueDMA |= RXDMA_AGG_EN;
    {
      uint16_t temp;

      /* Adjust DMA page and thresh. */
      temp =
          (uint16_t)(_device.rxagg_usb_size | (_device.rxagg_usb_timeout << 8));
      _device.rtw_write16(REG_RXDMA_AGG_PG_TH, temp);
    }
    break;
  case RX_AGG_MIX:
  case RX_AGG_DISABLE:
  default:
    /* TODO: */
    break;
  }

  _device.rtw_write8(REG_TRXDMA_CTRL, (uint8_t)valueDMA);
}

#define TBTT_PROHIBIT_SETUP_TIME 0x04         /* 128us, unit is 32us */
#define TBTT_PROHIBIT_HOLD_TIME 0x80          /* 4ms, unit is 32us*/
#define TBTT_PROHIBIT_HOLD_TIME_STOP_BCN 0x64 /* 3.2ms unit is 32us*/

void HalModule::_InitBeaconParameters_8812A() {
  uint8_t val8 = DIS_TSF_UDT;
  uint16_t val16 = (uint16_t)(val8 | (val8 << 8)); /* port0 and port1 */

  _device.rtw_write16(REG_BCN_CTRL, val16);

  /* TBTT setup time */
  _device.rtw_write8(REG_TBTT_PROHIBIT, TBTT_PROHIBIT_SETUP_TIME);

  /* TBTT hold time: 0x540[19:8] */
  _device.rtw_write8(REG_TBTT_PROHIBIT + 1,
                     TBTT_PROHIBIT_HOLD_TIME_STOP_BCN & 0xFF);
  _device.rtw_write8(
      REG_TBTT_PROHIBIT + 2,
      (uint8_t)((_device.rtw_read8(REG_TBTT_PROHIBIT + 2) & 0xF0) |
                (TBTT_PROHIBIT_HOLD_TIME_STOP_BCN >> 8)));

  _device.rtw_write8(REG_DRVERLYINT, DRIVER_EARLY_INT_TIME_8812); /* 5ms */
  _device.rtw_write8(REG_BCNDMATIM, BCN_DMA_ATIME_INT_TIME_8812); /* 2ms */

  /* Suggested by designer timchen. Change beacon AIFS to the largest number */
  /* beacause test chip does not contension before sending beacon. by tynli.
   * 2009.11.03 */
  _device.rtw_write16(REG_BCNTCFG, 0x4413);
}

void HalModule::_InitBeaconMaxError_8812A() {
  _device.rtw_write8(REG_BCN_MAX_ERR, 0xFF);
}

void HalModule::_InitBurstPktLen() {
  uint8_t speedvalue, provalue, temp;

  _device.rtw_write8(0xf050, 0x01); /* usb3 rx interval */
  _device.rtw_write16(
      REG_RXDMA_STATUS,
      0x7400); /* burset lenght=4, set 0x3400 for burset length=2 */
  _device.rtw_write8(0x289, 0xf5); /* for rxdma control */

  /* 0x456 = 0x70, sugguested by Zhilin */
  _device.rtw_write8(REG_AMPDU_MAX_TIME_8812, 0x70);

  _device.rtw_write32(REG_AMPDU_MAX_LENGTH_8812, 0xffffffff);
  _device.rtw_write8(REG_USTIME_TSF, 0x50);
  _device.rtw_write8(REG_USTIME_EDCA, 0x50);

  speedvalue =
      _device.rtw_read8(0xff); /* check device operation speed: SS 0xff bit7 */

  if ((speedvalue & BIT7) != 0) {
    /* USB2/1.1 Mode */
    temp = _device.rtw_read8(0xfe17);
    if (((temp >> 4) & 0x03) == 0) {
      provalue = _device.rtw_read8(REG_RXDMA_PRO_8812);
      _device.rtw_write8(REG_RXDMA_PRO_8812,
                         (uint8_t)((provalue | BIT4 | BIT3 | BIT2 | BIT1) &
                                   (~BIT5))); /* set burst pkt len=512B */
    } else {
      provalue = _device.rtw_read8(REG_RXDMA_PRO_8812);
      _device.rtw_write8(REG_RXDMA_PRO_8812,
                         (uint8_t)((provalue | BIT5 | BIT3 | BIT2 | BIT1) &
                                   (~BIT4))); /* set burst pkt len=64B */
    }
  } else {
    /* USB3 Mode */
    provalue = _device.rtw_read8(REG_RXDMA_PRO_8812);
    _device.rtw_write8(REG_RXDMA_PRO_8812,
                       //((provalue | BIT3 | BIT2 | BIT1) & (~(BIT5 | BIT4))));
                       ///* set burst pkt len=1k */
                       (uint8_t)((provalue | BIT3 | BIT2 | BIT1) &
                                 (0b11001111))); /* set burst pkt len=1k */

    _device.rtw_write8(0xf008, (uint8_t)(_device.rtw_read8(0xf008) & 0xE7));
  }

  temp = _device.rtw_read8(REG_SYS_FUNC_EN);
  _device.rtw_write8(REG_SYS_FUNC_EN,
                     (uint8_t)(temp & (~BIT10))); /* reset 8051 */

  _device.rtw_write8(REG_HT_SINGLE_AMPDU_8812,
                     (uint8_t)(_device.rtw_read8(REG_HT_SINGLE_AMPDU_8812) |
                               BIT7));        /* enable single pkt ampdu */
  _device.rtw_write8(REG_RX_PKT_LIMIT, 0x18); /* for VHT packet length 11K */

  _device.rtw_write8(REG_PIFS, 0x00);

  _device.rtw_write16(REG_MAX_AGGR_NUM, 0x1f1f);
  _device.rtw_write8(REG_FWHW_TXQ_CTRL,
                     (uint8_t)(_device.rtw_read8(REG_FWHW_TXQ_CTRL) & (~BIT7)));

  // AMPDUBurstMode is always false
  // if (pHalData.AMPDUBurstMode)
  //{
  //    adapterState.Device.rtw_write8(REG_AMPDU_BURST_MODE_8812, 0x5F);
  //}

  _device.rtw_write8(0x1c, (uint8_t)(_device.rtw_read8(0x1c) | BIT5 |
                                     BIT6)); /* to prevent mac is reseted by
                                                bus. 20111208, by Page */

  /* ARFB table 9 for 11ac 5G 2SS */
  _device.rtw_write32(REG_ARFR0_8812, 0x00000010);
  _device.rtw_write32(REG_ARFR0_8812 + 4, 0xfffff000);

  /* ARFB table 10 for 11ac 5G 1SS */
  _device.rtw_write32(REG_ARFR1_8812, 0x00000010);
  _device.rtw_write32(REG_ARFR1_8812 + 4, 0x003ff000);

  /* ARFB table 11 for 11ac 24G 1SS */
  _device.rtw_write32(REG_ARFR2_8812, 0x00000015);
  _device.rtw_write32(REG_ARFR2_8812 + 4, 0x003ff000);
  /* ARFB table 12 for 11ac 24G 2SS */
  _device.rtw_write32(REG_ARFR3_8812, 0x00000015);
  _device.rtw_write32(REG_ARFR3_8812 + 4, 0xffcff000);
}

bool HalModule::PHY_BBConfig8812() {
  /* tangw check start 20120412 */
  /* . APLL_EN,,APLL_320_GATEB,APLL_320BIAS,  auto config by hw fsm after
   * pfsm_go (0x4 bit 8) set */
  uint32_t TmpU1B = _device.rtw_read8(REG_SYS_FUNC_EN);

  TmpU1B |= FEN_USBA;

  _device.rtw_write8(REG_SYS_FUNC_EN, (uint8_t)TmpU1B);

  _device.rtw_write8(
      REG_SYS_FUNC_EN,
      (uint8_t)(TmpU1B | FEN_BB_GLB_RSTn | FEN_BBRSTB)); /* same with 8812 */
  /* 6. 0x1f[7:0] = 0x07 PathA RF Power On */
  _device.rtw_write8(REG_RF_CTRL,
                     0x07); /* RF_SDMRSTB,RF_RSTB,RF_EN same with 8723a */
  /* 7.  PathB RF Power On */
  _device.rtw_write8(REG_OPT_CTRL_8812 + 2,
                     0x7); /* RF_SDMRSTB,RF_RSTB,RF_EN same with 8723a */
  /* tangw check end 20120412 */

  /*  */
  /* Config BB and AGC */
  /*  */
  auto rtStatus = phy_BB8812_Config_ParaFile();

  hal_set_crystal_cap(_eepromManager->crystal_cap);

  return rtStatus;
}

bool HalModule::phy_BB8812_Config_ParaFile() {
  bool rtStatus = odm_config_bb_with_header_file(CONFIG_BB_PHY_REG);

  /* Read PHY_REG.TXT BB INIT!! */

  if (rtStatus != true) {
    _logger->error("phy_BB8812_Config_ParaFile: CONFIG_BB_PHY_REG Fail!!");
    goto phy_BB_Config_ParaFile_Fail;
  }

  rtStatus = odm_config_bb_with_header_file(CONFIG_BB_AGC_TAB);

  if (rtStatus != true) {
    _logger->error("phy_BB8812_Config_ParaFile CONFIG_BB_AGC_TAB Fail!!");
  }

phy_BB_Config_ParaFile_Fail:

  return rtStatus;
}

bool HalModule::odm_config_bb_with_header_file(odm_bb_config_type config_type) {
  bool result = true;

  /* @1 AP doesn't use PHYDM initialization in these ICs */

  if (config_type == CONFIG_BB_PHY_REG) {
    // READ_AND_CONFIG_MP(8812a, _phy_reg);
    odm_read_and_config_mp_8812a_phy_reg();
  } else if (config_type == CONFIG_BB_AGC_TAB) {
    // READ_AND_CONFIG_MP(8812a, _agc_tab);
    odm_read_and_config_mp_8812a_agc_tab();
  } else if (config_type == CONFIG_BB_PHY_REG_PG) {
    throw std::logic_error(
        "odm_bb_config_type.CONFIG_BB_PHY_REG_PG not yet implemented");
    // READ_AND_CONFIG_MP(8812a, _phy_reg_pg);
  } else if (config_type == CONFIG_BB_PHY_REG_MP) {
    // READ_AND_CONFIG_MP(8812a, _phy_reg_mp);
    odm_read_and_config_mp_8812a_phy_reg_mp();
  } else if (config_type == CONFIG_BB_AGC_TAB_DIFF) {
    throw std::logic_error(
        "odm_bb_config_type.CONFIG_BB_AGC_TAB_DIFF not yet implemented");
    // dm.fw_offload_ability &= ~PHYDM_PHY_PARAM_OFFLOAD;
    ///*@AGC_TAB DIFF dont support FW offload*/
    // if ((dm.channel >= 36) && (dm.channel <= 64))
    //{
    //     AGC_DIFF_CONFIG_MP(8812a, lb);
    // }
    // else if (*dm.channel >= 100)
    //{
    //     AGC_DIFF_CONFIG_MP(8812a, hb);
    // }
  }

  // TODO:
  // if (config_type == odm_bb_config_type.CONFIG_BB_PHY_REG || config_type ==
  // odm_bb_config_type.CONFIG_BB_AGC_TAB)
  //{
  //    if (dm.fw_offload_ability & PHYDM_PHY_PARAM_OFFLOAD)
  //    {
  //        result = phydm_set_reg_by_fw(dm, PHYDM_HALMAC_CMD_END, 0, 0, 0,
  //        (RfPath)0,0); PHYDM_DBG(dm, ODM_COMP_INIT, "phy param offload
  //        end!result = %d", result);
  //    }
  //}

  return result;
}

void HalModule::hal_set_crystal_cap(uint8_t crystal_cap) {
  crystal_cap = (uint8_t)(crystal_cap & 0x3F);

  /* write 0x2C[30:25] = 0x2C[24:19] = CrystalCap */
  _device.phy_set_bb_reg(REG_MAC_PHY_CTRL, 0x7FF80000u,
                         (uint8_t)(crystal_cap | (crystal_cap << 6)));
}

static uint32_t array_mp_8812a_phy_reg_mp[] = {
    0x800,
    0x8020D410,
    0x830,
    0x2EAA8EB8,
};

#define MASKDWORD 0xffffffff

void HalModule::odm_read_and_config_mp_8812a_phy_reg_mp() {
  uint32_t i = 0;
  uint8_t c_cond;
  bool is_matched = true, is_skipped = false;
  uint32_t array_len = ARRAY_LENGTH(array_mp_8812a_phy_reg_mp);

  uint32_t pre_v1 = 0, pre_v2 = 0;

  // PHYDM_DBG(dm, ODM_COMP_INIT, "===> %s\n", __func__);

  while ((i + 1) < array_len) {
    auto v1 = array_mp_8812a_phy_reg_mp[i];
    auto v2 = array_mp_8812a_phy_reg_mp[i + 1];

    if ((v1 & (BIT31 | BIT30)) != 0) {
      /*positive & negative condition*/
      if ((v1 & BIT31) != 0) {
        /* positive condition*/
        c_cond = (uint8_t)((v1 & (BIT29 | BIT28)) >> 28);
        if (c_cond == COND_ENDIF) {
          /*end*/
          is_matched = true;
          is_skipped = false;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "ENDIF\n");
        } else if (c_cond == COND_ELSE) {
          /*else*/
          is_matched = is_skipped ? false : true;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "ELSE\n");
        } else {
          /*if , else if*/
          pre_v1 = v1;
          pre_v2 = v2;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "IF or ELSE IF\n");
        }
      } else if ((v1 & BIT30) != 0) {
        /*negative condition*/
        if (is_skipped == false) {
          if (check_positive(pre_v1, pre_v2, v2)) {
            is_matched = true;
            is_skipped = true;
          } else {
            is_matched = false;
            is_skipped = false;
          }
        } else
          is_matched = false;
      }
    } else {
      if (is_matched) {
        odm_config_bb_phy_8812a(v1, MASKDWORD, v2);
      }
    }

    i = i + 2;
  }
}

static void ODM_delay_ms(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void ODM_sleep_ms(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void ODM_delay_us(int us) {
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}

void HalModule::odm_config_bb_phy_8812a(uint32_t addr, uint32_t bitmask,
                                        uint32_t data) {
  if (addr == 0xfe) {
    ODM_sleep_ms(50);
  } else if (addr == 0xfd) {
    ODM_delay_ms(5);
  } else if (addr == 0xfc) {
    ODM_delay_ms(1);
  } else if (addr == 0xfb) {
    ODM_delay_us(50);
  } else if (addr == 0xfa) {
    ODM_delay_us(5);
  } else if (addr == 0xf9) {
    ODM_delay_us(1);
  } else {
    odm_set_bb_reg(addr, bitmask, data);
    /* Add 1us delay between BB/RF register setting. */
    ODM_delay_us(1);
  }
}

void HalModule::odm_set_bb_reg(uint32_t reg_addr, uint32_t bit_mask,
                               uint32_t data) {
  _device.phy_set_bb_reg((uint16_t)reg_addr, bit_mask, data);
}

static uint32_t array_mp_8812a_phy_reg[] = {
    0x800,      0x8020D010, 0x804,      0x080112E0, 0x808,      0x0E028233,
    0x80C,      0x12131113, 0x810,      0x20101263, 0x814,      0x020C3D10,
    0x818,      0x03A00385, 0x820,      0x00000000, 0x824,      0x00030FE0,
    0x828,      0x00000000, 0x82C,      0x002083DD, 0x830,      0x2EAAEEB8,
    0x834,      0x0037A706, 0x838,      0x06C89B44, 0x83C,      0x0000095B,
    0x840,      0xC0000001, 0x844,      0x40003CDE, 0x848,      0x6210FF8B,
    0x84C,      0x6CFDFFB8, 0x850,      0x28874706, 0x854,      0x0001520C,
    0x858,      0x8060E000, 0x85C,      0x74210168, 0x860,      0x6929C321,
    0x864,      0x79727432, 0x868,      0x8CA7A314, 0x86C,      0x338C2878,
    0x870,      0x03333333, 0x874,      0x31602C2E, 0x878,      0x00003152,
    0x87C,      0x000FC000, 0x8A0,      0x00000013, 0x8A4,      0x7F7F7F7F,
    0x8A8,      0xA202033E, 0x8AC,      0x0FF0FA0A, 0x8B0,      0x00000600,
    0x8B4,      0x000FC080, 0x8B8,      0x6C10D7FF, 0x8BC,      0x4CA520A3,
    0x8C0,      0x27F00020, 0x8C4,      0x00000000, 0x8C8,      0x00012D69,
    0x8CC,      0x08248492, 0x8D0,      0x0000B800, 0x8DC,      0x00000000,
    0x8D4,      0x940008A0, 0x8D8,      0x290B5612, 0x8F8,      0x400002C0,
    0x8FC,      0x00000000, 0x900,      0x00000701, 0x90C,      0x00000000,
    0x910,      0x0000FC00, 0x914,      0x00000404, 0x918,      0x1C1028C0,
    0x91C,      0x64B11A1C, 0x920,      0xE0767233, 0x924,      0x055AA500,
    0x928,      0x00000004, 0x92C,      0xFFFE0000, 0x930,      0xFFFFFFFE,
    0x934,      0x001FFFFF, 0x960,      0x00000000, 0x964,      0x00000000,
    0x968,      0x00000000, 0x96C,      0x00000000, 0x970,      0x801FFFFF,
    0x978,      0x00000000, 0x97C,      0x00000000, 0x980,      0x00000000,
    0x984,      0x00000000, 0x988,      0x00000000, 0x990,      0x27100000,
    0x994,      0xFFFF0100, 0x998,      0xFFFFFF5C, 0x99C,      0xFFFFFFFF,
    0x9A0,      0x000000FF, 0x9A4,      0x00080080, 0x9A8,      0x00000000,
    0x9AC,      0x00000000, 0x9B0,      0x81081008, 0x9B4,      0x00000000,
    0x9B8,      0x01081008, 0x9BC,      0x01081008, 0x9D0,      0x00000000,
    0x9D4,      0x00000000, 0x9D8,      0x00000000, 0x9DC,      0x00000000,
    0x9E4,      0x00000003, 0x9E8,      0x000002D5, 0xA00,      0x00D047C8,
    0xA04,      0x01FF000C, 0xA08,      0x8C838300, 0xA0C,      0x2E7F000F,
    0xA10,      0x9500BB78, 0xA14,      0x11144028, 0xA18,      0x00881117,
    0xA1C,      0x89140F00, 0xA20,      0x1A1B0000, 0xA24,      0x090E1217,
    0xA28,      0x00000305, 0xA2C,      0x00900000, 0xA70,      0x101FFF00,
    0xA74,      0x00000008, 0xA78,      0x00000900, 0xA7C,      0x225B0606,
    0xA80,      0x218075B2, 0xA84,      0x001F8C80, 0xB00,      0x03100000,
    0xB04,      0x0000B000, 0xB08,      0xAE0201EB, 0xB0C,      0x01003207,
    0xB10,      0x00009807, 0xB14,      0x01000000, 0xB18,      0x00000002,
    0xB1C,      0x00000002, 0xB20,      0x0000001F, 0xB24,      0x03020100,
    0xB28,      0x07060504, 0xB2C,      0x0B0A0908, 0xB30,      0x0F0E0D0C,
    0xB34,      0x13121110, 0xB38,      0x17161514, 0xB3C,      0x0000003A,
    0xB40,      0x00000000, 0xB44,      0x00000000, 0xB48,      0x13000032,
    0xB4C,      0x48080000, 0xB50,      0x00000000, 0xB54,      0x00000000,
    0xB58,      0x00000000, 0xB5C,      0x00000000, 0xC00,      0x00000007,
    0xC04,      0x00042020, 0xC08,      0x80410231, 0xC0C,      0x00000000,
    0xC10,      0x00000100, 0xC14,      0x01000000, 0xC1C,      0x40000003,
    0xC20,      0x12121212, 0xC24,      0x12121212, 0xC28,      0x12121212,
    0xC2C,      0x12121212, 0xC30,      0x12121212, 0xC34,      0x12121212,
    0xC38,      0x12121212, 0xC3C,      0x12121212, 0xC40,      0x12121212,
    0xC44,      0x12121212, 0xC48,      0x12121212, 0xC4C,      0x12121212,
    0xC50,      0x00000020, 0xC54,      0x0008121C, 0xC58,      0x30000C1C,
    0xC5C,      0x00000058, 0xC60,      0x34344443, 0xC64,      0x07003333,
    0x80000008, 0x00000000, 0x40000000, 0x00000000, 0xC68,      0x59791979,
    0x90000008, 0x05000000, 0x40000000, 0x00000000, 0xC68,      0x59791979,
    0x90000002, 0x00000000, 0x40000000, 0x00000000, 0xC68,      0x59791979,
    0x90000004, 0x00000000, 0x40000000, 0x00000000, 0xC68,      0x59791979,
    0x90000001, 0x00000000, 0x40000000, 0x00000000, 0xC68,      0x59791979,
    0x90000001, 0x00000005, 0x40000000, 0x00000000, 0xC68,      0x59791979,
    0xA0000000, 0x00000000, 0xC68,      0x59799979, 0xB0000000, 0x00000000,
    0xC6C,      0x59795979, 0xC70,      0x19795979, 0xC74,      0x19795979,
    0xC78,      0x19791979, 0xC7C,      0x19791979, 0xC80,      0x19791979,
    0xC84,      0x19791979, 0xC94,      0x0100005C, 0xC98,      0x00000000,
    0xC9C,      0x00000000, 0xCA0,      0x00000029, 0xCA4,      0x08040201,
    0xCA8,      0x80402010, 0xCB0,      0x77547777, 0xCB4,      0x00000077,
    0xCB8,      0x00508242, 0xE00,      0x00000007, 0xE04,      0x00042020,
    0xE08,      0x80410231, 0xE0C,      0x00000000, 0xE10,      0x00000100,
    0xE14,      0x01000000, 0xE1C,      0x40000003, 0xE20,      0x12121212,
    0xE24,      0x12121212, 0xE28,      0x12121212, 0xE2C,      0x12121212,
    0xE30,      0x12121212, 0xE34,      0x12121212, 0xE38,      0x12121212,
    0xE3C,      0x12121212, 0xE40,      0x12121212, 0xE44,      0x12121212,
    0xE48,      0x12121212, 0xE4C,      0x12121212, 0xE50,      0x00000020,
    0xE54,      0x0008121C, 0xE58,      0x30000C1C, 0xE5C,      0x00000058,
    0xE60,      0x34344443, 0xE64,      0x07003333, 0xE68,      0x59791979,
    0xE6C,      0x59795979, 0xE70,      0x19795979, 0xE74,      0x19795979,
    0xE78,      0x19791979, 0xE7C,      0x19791979, 0xE80,      0x19791979,
    0xE84,      0x19791979, 0xE94,      0x0100005C, 0xE98,      0x00000000,
    0xE9C,      0x00000000, 0xEA0,      0x00000029, 0xEA4,      0x08040201,
    0xEA8,      0x80402010, 0xEB0,      0x77547777, 0xEB4,      0x00000077,
    0xEB8,      0x00508242,
};

void HalModule::odm_read_and_config_mp_8812a_phy_reg() {
  uint32_t i = 0;
  uint8_t c_cond;
  bool is_matched = true, is_skipped = false;
  uint32_t array_len = ARRAY_LENGTH(array_mp_8812a_phy_reg);

  uint32_t pre_v1 = 0, pre_v2 = 0;

  // PHYDM_DBG(dm, ODM_COMP_INIT, "===> %s\n", __func__);

  while ((i + 1) < array_len) {
    auto v1 = array_mp_8812a_phy_reg[i];
    auto v2 = array_mp_8812a_phy_reg[i + 1];

    if ((v1 & (BIT31 | BIT30)) != 0) {
      /*positive & negative condition*/
      if ((v1 & BIT31) != 0) {
        /* positive condition*/
        c_cond = (uint8_t)((v1 & (BIT29 | BIT28)) >> 28);
        if (c_cond == COND_ENDIF) {
          /*end*/
          is_matched = true;
          is_skipped = false;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "ENDIF\n");
        } else if (c_cond == COND_ELSE) {
          /*else*/
          is_matched = is_skipped ? false : true;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "ELSE\n");
        } else {
          /*if , else if*/
          pre_v1 = v1;
          pre_v2 = v2;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "IF or ELSE IF\n");
        }
      } else if ((v1 & BIT30) != 0) {
        /*negative condition*/
        if (is_skipped == false) {
          if (check_positive(pre_v1, pre_v2, v2)) {
            is_matched = true;
            is_skipped = true;
          } else {
            is_matched = false;
            is_skipped = false;
          }
        } else
          is_matched = false;
      }
    } else {
      if (is_matched) {
        //_logger->debug("SEND_TO {:04X}", v1);
        odm_config_bb_phy_8812a(v1, MASKDWORD, v2);
      }
    }

    i = i + 2;
  }
}

static uint32_t array_mp_8812a_agc_tab[] = {
    0x80000001, 0x00000000, 0x40000000, 0x00000000, 0x81C,      0xFC000001,
    0x81C,      0xFB020001, 0x81C,      0xFA040001, 0x81C,      0xF9060001,
    0x81C,      0xF8080001, 0x81C,      0xF70A0001, 0x81C,      0xF60C0001,
    0x81C,      0xF50E0001, 0x81C,      0xF4100001, 0x81C,      0xF3120001,
    0x81C,      0xF2140001, 0x81C,      0xF1160001, 0x81C,      0xF0180001,
    0x81C,      0xEF1A0001, 0x81C,      0xEE1C0001, 0x81C,      0xED1E0001,
    0x81C,      0xEC200001, 0x81C,      0xEB220001, 0x81C,      0xEA240001,
    0x81C,      0xCD260001, 0x81C,      0xCC280001, 0x81C,      0xCB2A0001,
    0x81C,      0xCA2C0001, 0x81C,      0xC92E0001, 0x81C,      0xC8300001,
    0x81C,      0xA6320001, 0x81C,      0xA5340001, 0x81C,      0xA4360001,
    0x81C,      0xA3380001, 0x81C,      0xA23A0001, 0x81C,      0x883C0001,
    0x81C,      0x873E0001, 0x81C,      0x86400001, 0x81C,      0x85420001,
    0x81C,      0x84440001, 0x81C,      0x83460001, 0x81C,      0x82480001,
    0x81C,      0x814A0001, 0x81C,      0x484C0001, 0x81C,      0x474E0001,
    0x81C,      0x46500001, 0x81C,      0x45520001, 0x81C,      0x44540001,
    0x81C,      0x43560001, 0x81C,      0x42580001, 0x81C,      0x415A0001,
    0x81C,      0x255C0001, 0x81C,      0x245E0001, 0x81C,      0x23600001,
    0x81C,      0x22620001, 0x81C,      0x21640001, 0x81C,      0x21660001,
    0x81C,      0x21680001, 0x81C,      0x216A0001, 0x81C,      0x216C0001,
    0x81C,      0x216E0001, 0x81C,      0x21700001, 0x81C,      0x21720001,
    0x81C,      0x21740001, 0x81C,      0x21760001, 0x81C,      0x21780001,
    0x81C,      0x217A0001, 0x81C,      0x217C0001, 0x81C,      0x217E0001,
    0x90000001, 0x00000005, 0x40000000, 0x00000000, 0x81C,      0xF9000001,
    0x81C,      0xF8020001, 0x81C,      0xF7040001, 0x81C,      0xF6060001,
    0x81C,      0xF5080001, 0x81C,      0xF40A0001, 0x81C,      0xF30C0001,
    0x81C,      0xF20E0001, 0x81C,      0xF1100001, 0x81C,      0xF0120001,
    0x81C,      0xEF140001, 0x81C,      0xEE160001, 0x81C,      0xED180001,
    0x81C,      0xEC1A0001, 0x81C,      0xEB1C0001, 0x81C,      0xEA1E0001,
    0x81C,      0xCD200001, 0x81C,      0xCC220001, 0x81C,      0xCB240001,
    0x81C,      0xCA260001, 0x81C,      0xC9280001, 0x81C,      0xC82A0001,
    0x81C,      0xC72C0001, 0x81C,      0xC62E0001, 0x81C,      0xA5300001,
    0x81C,      0xA4320001, 0x81C,      0xA3340001, 0x81C,      0xA2360001,
    0x81C,      0x88380001, 0x81C,      0x873A0001, 0x81C,      0x863C0001,
    0x81C,      0x853E0001, 0x81C,      0x84400001, 0x81C,      0x83420001,
    0x81C,      0x82440001, 0x81C,      0x81460001, 0x81C,      0x48480001,
    0x81C,      0x474A0001, 0x81C,      0x464C0001, 0x81C,      0x454E0001,
    0x81C,      0x44500001, 0x81C,      0x43520001, 0x81C,      0x42540001,
    0x81C,      0x41560001, 0x81C,      0x25580001, 0x81C,      0x245A0001,
    0x81C,      0x235C0001, 0x81C,      0x225E0001, 0x81C,      0x21600001,
    0x81C,      0x21620001, 0x81C,      0x21640001, 0x81C,      0x21660001,
    0x81C,      0x21680001, 0x81C,      0x216A0001, 0x81C,      0x236C0001,
    0x81C,      0x226E0001, 0x81C,      0x21700001, 0x81C,      0x21720001,
    0x81C,      0x21740001, 0x81C,      0x21760001, 0x81C,      0x21780001,
    0x81C,      0x217A0001, 0x81C,      0x217C0001, 0x81C,      0x217E0001,
    0xA0000000, 0x00000000, 0x81C,      0xFF000001, 0x81C,      0xFF020001,
    0x81C,      0xFF040001, 0x81C,      0xFF060001, 0x81C,      0xFF080001,
    0x81C,      0xFE0A0001, 0x81C,      0xFD0C0001, 0x81C,      0xFC0E0001,
    0x81C,      0xFB100001, 0x81C,      0xFA120001, 0x81C,      0xF9140001,
    0x81C,      0xF8160001, 0x81C,      0xF7180001, 0x81C,      0xF61A0001,
    0x81C,      0xF51C0001, 0x81C,      0xF41E0001, 0x81C,      0xF3200001,
    0x81C,      0xF2220001, 0x81C,      0xF1240001, 0x81C,      0xF0260001,
    0x81C,      0xEF280001, 0x81C,      0xEE2A0001, 0x81C,      0xED2C0001,
    0x81C,      0xEC2E0001, 0x81C,      0xEB300001, 0x81C,      0xEA320001,
    0x81C,      0xE9340001, 0x81C,      0xE8360001, 0x81C,      0xE7380001,
    0x81C,      0xE63A0001, 0x81C,      0xE53C0001, 0x81C,      0xC73E0001,
    0x81C,      0xC6400001, 0x81C,      0xC5420001, 0x81C,      0xC4440001,
    0x81C,      0xC3460001, 0x81C,      0xC2480001, 0x81C,      0xC14A0001,
    0x81C,      0xA74C0001, 0x81C,      0xA64E0001, 0x81C,      0xA5500001,
    0x81C,      0xA4520001, 0x81C,      0xA3540001, 0x81C,      0xA2560001,
    0x81C,      0xA1580001, 0x81C,      0x675A0001, 0x81C,      0x665C0001,
    0x81C,      0x655E0001, 0x81C,      0x64600001, 0x81C,      0x63620001,
    0x81C,      0x48640001, 0x81C,      0x47660001, 0x81C,      0x46680001,
    0x81C,      0x456A0001, 0x81C,      0x446C0001, 0x81C,      0x436E0001,
    0x81C,      0x42700001, 0x81C,      0x41720001, 0x81C,      0x41740001,
    0x81C,      0x41760001, 0x81C,      0x41780001, 0x81C,      0x417A0001,
    0x81C,      0x417C0001, 0x81C,      0x417E0001, 0xB0000000, 0x00000000,
    0x80000004, 0x00000000, 0x40000000, 0x00000000, 0x81C,      0xFC800001,
    0x81C,      0xFB820001, 0x81C,      0xFA840001, 0x81C,      0xF9860001,
    0x81C,      0xF8880001, 0x81C,      0xF78A0001, 0x81C,      0xF68C0001,
    0x81C,      0xF58E0001, 0x81C,      0xF4900001, 0x81C,      0xF3920001,
    0x81C,      0xF2940001, 0x81C,      0xF1960001, 0x81C,      0xF0980001,
    0x81C,      0xEF9A0001, 0x81C,      0xEE9C0001, 0x81C,      0xED9E0001,
    0x81C,      0xECA00001, 0x81C,      0xEBA20001, 0x81C,      0xEAA40001,
    0x81C,      0xE9A60001, 0x81C,      0xE8A80001, 0x81C,      0xE7AA0001,
    0x81C,      0xE6AC0001, 0x81C,      0xE5AE0001, 0x81C,      0xE4B00001,
    0x81C,      0xE3B20001, 0x81C,      0xA8B40001, 0x81C,      0xA7B60001,
    0x81C,      0xA6B80001, 0x81C,      0xA5BA0001, 0x81C,      0xA4BC0001,
    0x81C,      0xA3BE0001, 0x81C,      0xA2C00001, 0x81C,      0xA1C20001,
    0x81C,      0x68C40001, 0x81C,      0x67C60001, 0x81C,      0x66C80001,
    0x81C,      0x65CA0001, 0x81C,      0x64CC0001, 0x81C,      0x47CE0001,
    0x81C,      0x46D00001, 0x81C,      0x45D20001, 0x81C,      0x44D40001,
    0x81C,      0x43D60001, 0x81C,      0x42D80001, 0x81C,      0x08DA0001,
    0x81C,      0x07DC0001, 0x81C,      0x06DE0001, 0x81C,      0x05E00001,
    0x81C,      0x04E20001, 0x81C,      0x03E40001, 0x81C,      0x02E60001,
    0x81C,      0x01E80001, 0x81C,      0x01EA0001, 0x81C,      0x01EC0001,
    0x81C,      0x01EE0001, 0x81C,      0x01F00001, 0x81C,      0x01F20001,
    0x81C,      0x01F40001, 0x81C,      0x01F60001, 0x81C,      0x01F80001,
    0x81C,      0x01FA0001, 0x81C,      0x01FC0001, 0x81C,      0x01FE0001,
    0xA0000000, 0x00000000, 0x81C,      0xFF800001, 0x81C,      0xFF820001,
    0x81C,      0xFF840001, 0x81C,      0xFE860001, 0x81C,      0xFD880001,
    0x81C,      0xFC8A0001, 0x81C,      0xFB8C0001, 0x81C,      0xFA8E0001,
    0x81C,      0xF9900001, 0x81C,      0xF8920001, 0x81C,      0xF7940001,
    0x81C,      0xF6960001, 0x81C,      0xF5980001, 0x81C,      0xF49A0001,
    0x81C,      0xF39C0001, 0x81C,      0xF29E0001, 0x81C,      0xF1A00001,
    0x81C,      0xF0A20001, 0x81C,      0xEFA40001, 0x81C,      0xEEA60001,
    0x81C,      0xEDA80001, 0x81C,      0xECAA0001, 0x81C,      0xEBAC0001,
    0x81C,      0xEAAE0001, 0x81C,      0xE9B00001, 0x81C,      0xE8B20001,
    0x81C,      0xE7B40001, 0x81C,      0xE6B60001, 0x81C,      0xE5B80001,
    0x81C,      0xE4BA0001, 0x81C,      0xE3BC0001, 0x81C,      0xA8BE0001,
    0x81C,      0xA7C00001, 0x81C,      0xA6C20001, 0x81C,      0xA5C40001,
    0x81C,      0xA4C60001, 0x81C,      0xA3C80001, 0x81C,      0xA2CA0001,
    0x81C,      0xA1CC0001, 0x81C,      0x68CE0001, 0x81C,      0x67D00001,
    0x81C,      0x66D20001, 0x81C,      0x65D40001, 0x81C,      0x64D60001,
    0x81C,      0x47D80001, 0x81C,      0x46DA0001, 0x81C,      0x45DC0001,
    0x81C,      0x44DE0001, 0x81C,      0x43E00001, 0x81C,      0x42E20001,
    0x81C,      0x08E40001, 0x81C,      0x07E60001, 0x81C,      0x06E80001,
    0x81C,      0x05EA0001, 0x81C,      0x04EC0001, 0x81C,      0x03EE0001,
    0x81C,      0x02F00001, 0x81C,      0x01F20001, 0x81C,      0x01F40001,
    0x81C,      0x01F60001, 0x81C,      0x01F80001, 0x81C,      0x01FA0001,
    0x81C,      0x01FC0001, 0x81C,      0x01FE0001, 0xB0000000, 0x00000000,
    0xC50,      0x00000022, 0xC50,      0x00000020, 0xE50,      0x00000022,
    0xE50,      0x00000020,
};

void HalModule::odm_read_and_config_mp_8812a_agc_tab() {
  uint32_t i = 0;
  uint8_t c_cond;
  bool is_matched = true, is_skipped = false;
  uint32_t array_len = ARRAY_LENGTH(array_mp_8812a_agc_tab);

  uint32_t pre_v1 = 0, pre_v2 = 0;

  // PHYDM_DBG(dm, ODM_COMP_INIT, "===> %s\n", __func__);

  while ((i + 1) < array_len) {
    auto v1 = array_mp_8812a_agc_tab[i];
    auto v2 = array_mp_8812a_agc_tab[i + 1];

    if ((v1 & (BIT31 | BIT30)) != 0) {
      /*positive & negative condition*/
      if ((v1 & BIT31) != 0) {
        /* positive condition*/
        c_cond = (uint8_t)((v1 & (BIT29 | BIT28)) >> 28);
        if (c_cond == COND_ENDIF) {
          /*end*/
          is_matched = true;
          is_skipped = false;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "ENDIF\n");
        } else if (c_cond == COND_ELSE) {
          /*else*/
          is_matched = is_skipped ? false : true;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "ELSE\n");
        } else {
          /*if , else if*/
          pre_v1 = v1;
          pre_v2 = v2;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "IF or ELSE IF\n");
        }
      } else if ((v1 & BIT30) != 0) {
        /*negative condition*/
        if (is_skipped == false) {
          if (check_positive(pre_v1, pre_v2, v2)) {
            is_matched = true;
            is_skipped = true;
          } else {
            is_matched = false;
            is_skipped = false;
          }
        } else
          is_matched = false;
      }
    } else {
      if (is_matched) {
        odm_config_bb_agc_8812a(v1, MASKDWORD, v2);
      }
    }

    i = i + 2;
  }
}

void HalModule::odm_config_bb_agc_8812a(uint32_t addr, uint32_t bitmask,
                                        uint32_t data) {
  odm_set_bb_reg(addr, bitmask, data);
  /* Add 1us delay between BB/RF register setting. */
  ODM_delay_us(1);
}

void HalModule::PHY_RF6052_Config_8812() {
  /*  */
  /* Config BB and RF */
  /*  */
  phy_RF6052_Config_ParaFile_8812();
}

void HalModule::phy_RF6052_Config_ParaFile_8812() {
  uint8_t eRFPath;

  for (eRFPath = 0; (uint8_t)eRFPath < _eepromManager->numTotalRfPath;
       eRFPath++) {
    /*----Initialize RF fom connfiguration file----*/
    switch (eRFPath) {
    case RfPath::RF_PATH_A:
      odm_config_rf_with_header_file(odm_rf_config_type::CONFIG_RF_RADIO,
                                     (RfPath)eRFPath);
      break;
    case RfPath::RF_PATH_B:
      odm_config_rf_with_header_file(odm_rf_config_type::CONFIG_RF_RADIO,
                                     (RfPath)eRFPath);
      break;
    default:
      break;
    }
  }
}

void HalModule::odm_config_rf_with_header_file(odm_rf_config_type config_type,
                                               RfPath e_rf_path) {
  if (config_type == odm_rf_config_type::CONFIG_RF_RADIO) {
    if (e_rf_path == RfPath::RF_PATH_A) {
      // READ_AND_CONFIG_MP(8812a, _radioa);
      odm_read_and_config_mp_8812a_radioa();
    } else if (e_rf_path == RfPath::RF_PATH_B) {
      // READ_AND_CONFIG_MP(8812a, _radiob);
      odm_read_and_config_mp_8812a_radiob();
    }
  }
}

static uint32_t array_mp_8812a_radioa[] = {
    0x000,      0x00010000, 0x018,      0x0001712A, 0x056,      0x00051CF2,
    0x066,      0x00040000, 0x01E,      0x00080000, 0x089,      0x00000080,
    0x80000001, 0x00000000, 0x40000000, 0x00000000, 0x086,      0x00014B3A,
    0x90000001, 0x00000005, 0x40000000, 0x00000000, 0x086,      0x00014B3A,
    0xA0000000, 0x00000000, 0x086,      0x00014B38, 0xB0000000, 0x00000000,
    0x80000004, 0x00000000, 0x40000000, 0x00000000, 0x08B,      0x00080180,
    0xA0000000, 0x00000000, 0x08B,      0x00087180, 0xB0000000, 0x00000000,
    0x0B1,      0x0001FC1A, 0x0B3,      0x000F0810, 0x0B4,      0x0001A78D,
    0x0BA,      0x00086180, 0x018,      0x00000006, 0x0EF,      0x00002000,
    0x80000001, 0x00000000, 0x40000000, 0x00000000, 0x03B,      0x0003F218,
    0x03B,      0x00030A58, 0x03B,      0x0002FA58, 0x03B,      0x00022590,
    0x03B,      0x0001FA50, 0x03B,      0x00010248, 0x03B,      0x00008240,
    0x90000001, 0x00000005, 0x40000000, 0x00000000, 0x03B,      0x0003F218,
    0x03B,      0x00030A58, 0x03B,      0x0002FA58, 0x03B,      0x00022590,
    0x03B,      0x0001FA50, 0x03B,      0x00010248, 0x03B,      0x00008240,
    0xA0000000, 0x00000000, 0x03B,      0x00038A58, 0x03B,      0x00037A58,
    0x03B,      0x0002A590, 0x03B,      0x00027A50, 0x03B,      0x00018248,
    0x03B,      0x00010240, 0x03B,      0x00008240, 0xB0000000, 0x00000000,
    0x0EF,      0x00000100, 0x80000002, 0x00000000, 0x40000000, 0x00000000,
    0x034,      0x0000A4EE, 0x034,      0x00009076, 0x034,      0x00008073,
    0x034,      0x00007070, 0x034,      0x0000606D, 0x034,      0x0000506A,
    0x034,      0x00004049, 0x034,      0x00003046, 0x034,      0x00002028,
    0x034,      0x00001025, 0x034,      0x00000022, 0xA0000000, 0x00000000,
    0x034,      0x0000ADF4, 0x034,      0x00009DF1, 0x034,      0x00008DEE,
    0x034,      0x00007DEB, 0x034,      0x00006DE8, 0x034,      0x00005DE5,
    0x034,      0x00004DE2, 0x034,      0x00003CE6, 0x034,      0x000024E7,
    0x034,      0x000014E4, 0x034,      0x000004E1, 0xB0000000, 0x00000000,
    0x0EF,      0x00000000, 0x0EF,      0x000020A2, 0x0DF,      0x00000080,
    0x035,      0x00000192, 0x035,      0x00008192, 0x035,      0x00010192,
    0x036,      0x00000024, 0x036,      0x00008024, 0x036,      0x00010024,
    0x036,      0x00018024, 0x0EF,      0x00000000, 0x051,      0x00000C21,
    0x052,      0x000006D9, 0x053,      0x000FC649, 0x054,      0x0000017E,
    0x0EF,      0x00000002, 0x008,      0x00008400, 0x018,      0x0001712A,
    0x0EF,      0x00001000, 0x03A,      0x00000080, 0x03B,      0x0003A02C,
    0x03C,      0x00004000, 0x03A,      0x00000400, 0x03B,      0x0003202C,
    0x03C,      0x00010000, 0x03A,      0x000000A0, 0x03B,      0x0002B064,
    0x03C,      0x00004000, 0x03A,      0x000000D8, 0x03B,      0x00023070,
    0x03C,      0x00004000, 0x03A,      0x00000468, 0x03B,      0x0001B870,
    0x03C,      0x00010000, 0x03A,      0x00000098, 0x03B,      0x00012085,
    0x03C,      0x000E4000, 0x03A,      0x00000418, 0x03B,      0x0000A080,
    0x03C,      0x000F0000, 0x03A,      0x00000418, 0x03B,      0x00002080,
    0x03C,      0x00010000, 0x03A,      0x00000080, 0x03B,      0x0007A02C,
    0x03C,      0x00004000, 0x03A,      0x00000400, 0x03B,      0x0007202C,
    0x03C,      0x00010000, 0x03A,      0x000000A0, 0x03B,      0x0006B064,
    0x03C,      0x00004000, 0x03A,      0x000000D8, 0x03B,      0x00063070,
    0x03C,      0x00004000, 0x03A,      0x00000468, 0x03B,      0x0005B870,
    0x03C,      0x00010000, 0x03A,      0x00000098, 0x03B,      0x00052085,
    0x03C,      0x000E4000, 0x03A,      0x00000418, 0x03B,      0x0004A080,
    0x03C,      0x000F0000, 0x03A,      0x00000418, 0x03B,      0x00042080,
    0x03C,      0x00010000, 0x03A,      0x00000080, 0x03B,      0x000BA02C,
    0x03C,      0x00004000, 0x03A,      0x00000400, 0x03B,      0x000B202C,
    0x03C,      0x00010000, 0x03A,      0x000000A0, 0x03B,      0x000AB064,
    0x03C,      0x00004000, 0x03A,      0x000000D8, 0x03B,      0x000A3070,
    0x03C,      0x00004000, 0x03A,      0x00000468, 0x03B,      0x0009B870,
    0x03C,      0x00010000, 0x03A,      0x00000098, 0x03B,      0x00092085,
    0x03C,      0x000E4000, 0x03A,      0x00000418, 0x03B,      0x0008A080,
    0x03C,      0x000F0000, 0x03A,      0x00000418, 0x03B,      0x00082080,
    0x03C,      0x00010000, 0x0EF,      0x00001100, 0x80000008, 0x00000000,
    0x40000000, 0x00000000, 0x034,      0x0004A0B2, 0x034,      0x000490AF,
    0x034,      0x00048070, 0x034,      0x0004706D, 0x034,      0x00046050,
    0x034,      0x0004504D, 0x034,      0x0004404A, 0x034,      0x00043047,
    0x034,      0x0004200A, 0x034,      0x00041007, 0x034,      0x00040004,
    0x90000008, 0x05000000, 0x40000000, 0x00000000, 0x034,      0x0004A0B2,
    0x034,      0x000490AF, 0x034,      0x00048070, 0x034,      0x0004706D,
    0x034,      0x0004604D, 0x034,      0x0004504A, 0x034,      0x00044047,
    0x034,      0x00043044, 0x034,      0x00042007, 0x034,      0x00041004,
    0x034,      0x00040001, 0xA0000000, 0x00000000, 0x034,      0x0004ADF5,
    0x034,      0x00049DF2, 0x034,      0x00048DEF, 0x034,      0x00047DEC,
    0x034,      0x00046DE9, 0x034,      0x00045DE6, 0x034,      0x00044DE3,
    0x034,      0x000438C8, 0x034,      0x000428C5, 0x034,      0x000418C2,
    0x034,      0x000408C0, 0xB0000000, 0x00000000, 0x80000008, 0x00000000,
    0x40000000, 0x00000000, 0x034,      0x0002A0B2, 0x034,      0x000290AF,
    0x034,      0x00028070, 0x034,      0x0002706D, 0x034,      0x00026050,
    0x034,      0x0002504D, 0x034,      0x0002404A, 0x034,      0x00023047,
    0x034,      0x0002200A, 0x034,      0x00021007, 0x034,      0x00020004,
    0x90000008, 0x05000000, 0x40000000, 0x00000000, 0x034,      0x0002A0B4,
    0x034,      0x000290B1, 0x034,      0x00028072, 0x034,      0x0002706F,
    0x034,      0x0002604F, 0x034,      0x0002504C, 0x034,      0x00024049,
    0x034,      0x00023046, 0x034,      0x00022009, 0x034,      0x00021006,
    0x034,      0x00020003, 0xA0000000, 0x00000000, 0x034,      0x0002ADF5,
    0x034,      0x00029DF2, 0x034,      0x00028DEF, 0x034,      0x00027DEC,
    0x034,      0x00026DE9, 0x034,      0x00025DE6, 0x034,      0x00024DE3,
    0x034,      0x000238C8, 0x034,      0x000228C5, 0x034,      0x000218C2,
    0x034,      0x000208C0, 0xB0000000, 0x00000000, 0x80000008, 0x00000000,
    0x40000000, 0x00000000, 0x034,      0x0000A0B2, 0x034,      0x000090AF,
    0x034,      0x00008070, 0x034,      0x0000706D, 0x034,      0x00006050,
    0x034,      0x0000504D, 0x034,      0x0000404A, 0x034,      0x00003047,
    0x034,      0x0000200A, 0x034,      0x00001007, 0x034,      0x00000004,
    0x90000008, 0x05000000, 0x40000000, 0x00000000, 0x034,      0x0000A0B2,
    0x034,      0x000090AF, 0x034,      0x00008070, 0x034,      0x0000706D,
    0x034,      0x0000604D, 0x034,      0x0000504A, 0x034,      0x00004047,
    0x034,      0x00003044, 0x034,      0x00002007, 0x034,      0x00001004,
    0x034,      0x00000001, 0xA0000000, 0x00000000, 0x034,      0x0000AFF7,
    0x034,      0x00009DF7, 0x034,      0x00008DF4, 0x034,      0x00007DF1,
    0x034,      0x00006DEE, 0x034,      0x00005DEB, 0x034,      0x00004DE8,
    0x034,      0x000038CC, 0x034,      0x000028C9, 0x034,      0x000018C6,
    0x034,      0x000008C3, 0xB0000000, 0x00000000, 0x0EF,      0x00000000,
    0x80000008, 0x00000000, 0x40000000, 0x00000000, 0x018,      0x0001712A,
    0x0EF,      0x00000040, 0x035,      0x000001D4, 0x035,      0x000081D4,
    0x035,      0x000101D4, 0x035,      0x000201B4, 0x035,      0x000281B4,
    0x035,      0x000301B4, 0x035,      0x000401B4, 0x035,      0x000481B4,
    0x035,      0x000501B4, 0x90000008, 0x05000000, 0x40000000, 0x00000000,
    0x018,      0x0001712A, 0x0EF,      0x00000040, 0x035,      0x000001D4,
    0x035,      0x000081D4, 0x035,      0x000101D4, 0x035,      0x000201B4,
    0x035,      0x000281B4, 0x035,      0x000301B4, 0x035,      0x000401B4,
    0x035,      0x000481B4, 0x035,      0x000501B4, 0xA0000000, 0x00000000,
    0x018,      0x0001712A, 0x0EF,      0x00000040, 0x035,      0x00000188,
    0x035,      0x00008147, 0x035,      0x00010147, 0x035,      0x000201D7,
    0x035,      0x000281D7, 0x035,      0x000301D7, 0x035,      0x000401D8,
    0x035,      0x000481D8, 0x035,      0x000501D8, 0xB0000000, 0x00000000,
    0x0EF,      0x00000000, 0x80000008, 0x00000000, 0x40000000, 0x00000000,
    0x018,      0x0001712A, 0x0EF,      0x00000010, 0x036,      0x00004BFB,
    0x036,      0x0000CBFB, 0x036,      0x00014BFB, 0x036,      0x0001CBFB,
    0x036,      0x00024F4B, 0x036,      0x0002CF4B, 0x036,      0x00034F4B,
    0x036,      0x0003CF4B, 0x036,      0x00044F4B, 0x036,      0x0004CF4B,
    0x036,      0x00054F4B, 0x036,      0x0005CF4B, 0x90000008, 0x05000000,
    0x40000000, 0x00000000, 0x018,      0x0001712A, 0x0EF,      0x00000010,
    0x036,      0x00004BFB, 0x036,      0x0000CBFB, 0x036,      0x00014BFB,
    0x036,      0x0001CBFB, 0x036,      0x00024F4B, 0x036,      0x0002CF4B,
    0x036,      0x00034F4B, 0x036,      0x0003CF4B, 0x036,      0x00044F4B,
    0x036,      0x0004CF4B, 0x036,      0x00054F4B, 0x036,      0x0005CF4B,
    0xA0000000, 0x00000000, 0x018,      0x0001712A, 0x0EF,      0x00000010,
    0x036,      0x00084EB4, 0x036,      0x0008CC35, 0x036,      0x00094C35,
    0x036,      0x0009CC35, 0x036,      0x000A4C35, 0x036,      0x000ACC35,
    0x036,      0x000B4C35, 0x036,      0x000BCC35, 0x036,      0x000C4C34,
    0x036,      0x000CCC35, 0x036,      0x000D4C35, 0x036,      0x000DCC35,
    0xB0000000, 0x00000000, 0x0EF,      0x00000000, 0x0EF,      0x00000008,
    0x80000008, 0x00000000, 0x40000000, 0x00000000, 0x03C,      0x000002CC,
    0x03C,      0x00000522, 0x03C,      0x00000902, 0x90000008, 0x05000000,
    0x40000000, 0x00000000, 0x03C,      0x000002CC, 0x03C,      0x00000522,
    0x03C,      0x00000902, 0xA0000000, 0x00000000, 0x03C,      0x000002A8,
    0x03C,      0x000005A2, 0x03C,      0x00000880, 0xB0000000, 0x00000000,
    0x0EF,      0x00000000, 0x018,      0x0001712A, 0x0EF,      0x00000002,
    0x0DF,      0x00000080, 0x01F,      0x00000064, 0x80000008, 0x00000000,
    0x40000000, 0x00000000, 0x061,      0x000FDD43, 0x062,      0x00038F4B,
    0x063,      0x00032117, 0x064,      0x000194AC, 0x065,      0x000931D1,
    0x90000008, 0x05000000, 0x40000000, 0x00000000, 0x061,      0x000FDD43,
    0x062,      0x00038F4B, 0x063,      0x00032117, 0x064,      0x000194AC,
    0x065,      0x000931D2, 0xA0000000, 0x00000000, 0x061,      0x000E5D53,
    0x062,      0x00038FCD, 0x063,      0x000114EB, 0x064,      0x000196AC,
    0x065,      0x000911D7, 0xB0000000, 0x00000000, 0x008,      0x00008400,
    0x01C,      0x000739D2, 0x0B4,      0x0001E78D, 0x018,      0x0001F12A,
    0xFFE,      0x00000000, 0xFFE,      0x00000000, 0xFFE,      0x00000000,
    0xFFE,      0x00000000, 0x0B4,      0x0001A78D, 0x018,      0x0001712A,

};

void HalModule::odm_read_and_config_mp_8812a_radioa() {
  uint32_t i = 0;
  uint8_t c_cond;
  bool is_matched = true, is_skipped = false;
  uint32_t array_len = ARRAY_LENGTH(array_mp_8812a_radioa);

  uint32_t pre_v1 = 0, pre_v2 = 0;

  // PHYDM_DBG(dm, ODM_COMP_INIT, "===> %s\n", __func__);

  while ((i + 1) < array_len) {
    auto v1 = array_mp_8812a_radioa[i];
    auto v2 = array_mp_8812a_radioa[i + 1];

    if ((v1 & (BIT31 | BIT30)) != 0) {
      /*positive & negative condition*/
      if ((v1 & BIT31) != 0) {
        /* positive condition*/
        c_cond = (uint8_t)((v1 & (BIT29 | BIT28)) >> 28);
        if (c_cond == COND_ENDIF) {
          /*end*/
          is_matched = true;
          is_skipped = false;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "ENDIF\n");
        } else if (c_cond == COND_ELSE) {
          /*else*/
          is_matched = is_skipped ? false : true;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "ELSE\n");
        } else {
          /*if , else if*/
          pre_v1 = v1;
          pre_v2 = v2;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "IF or ELSE IF\n");
        }
      } else if ((v1 & BIT30) != 0) {
        /*negative condition*/
        if (is_skipped == false) {
          if (check_positive(pre_v1, pre_v2, v2)) {
            is_matched = true;
            is_skipped = true;
          } else {
            is_matched = false;
            is_skipped = false;
          }
        } else
          is_matched = false;
      }
    } else {
      if (is_matched) {
        odm_config_rf_radio_a_8812a(v1, v2);
      }
    }

    i = i + 2;
  }
}

void HalModule::odm_config_rf_radio_a_8812a(uint32_t addr, uint32_t data) {
  uint32_t content = 0x1000; /* RF_Content: radioa_txt */
  uint32_t maskfor_phy_set = (uint32_t)(content & 0xE000);

  odm_config_rf_reg_8812a(addr, data, RfPath::RF_PATH_A,
                          (uint16_t)(addr | maskfor_phy_set));
}

#define RFREGOFFSETMASK 0xfffff

void HalModule::odm_config_rf_reg_8812a(uint32_t addr, uint32_t data,
                                        RfPath RF_PATH, uint16_t reg_addr) {
  if (addr == 0xfe || addr == 0xffe) {
    ODM_sleep_ms(50);
  } else {
    odm_set_rf_reg(RF_PATH, reg_addr, RFREGOFFSETMASK, data);
    /* Add 1us delay between BB/RF register setting. */
    ODM_delay_us(1);
  }
}

static uint32_t array_mp_8812a_radiob[] = {
    0x056,      0x00051CF2, 0x066,      0x00040000, 0x089,      0x00000080,
    0x80000001, 0x00000000, 0x40000000, 0x00000000, 0x086,      0x00014B3A,
    0x90000001, 0x00000005, 0x40000000, 0x00000000, 0x086,      0x00014B3A,
    0xA0000000, 0x00000000, 0x086,      0x00014B38, 0xB0000000, 0x00000000,
    0x80000004, 0x00000000, 0x40000000, 0x00000000, 0x08B,      0x00080180,
    0xA0000000, 0x00000000, 0x08B,      0x00087180, 0xB0000000, 0x00000000,
    0x018,      0x00000006, 0x0EF,      0x00002000, 0x80000001, 0x00000000,
    0x40000000, 0x00000000, 0x03B,      0x0003F218, 0x03B,      0x00030A58,
    0x03B,      0x0002FA58, 0x03B,      0x00022590, 0x03B,      0x0001FA50,
    0x03B,      0x00010248, 0x03B,      0x00008240, 0x90000001, 0x00000005,
    0x40000000, 0x00000000, 0x03B,      0x0003F218, 0x03B,      0x00030A58,
    0x03B,      0x0002FA58, 0x03B,      0x00022590, 0x03B,      0x0001FA50,
    0x03B,      0x00010248, 0x03B,      0x00008240, 0xA0000000, 0x00000000,
    0x03B,      0x00038A58, 0x03B,      0x00037A58, 0x03B,      0x0002A590,
    0x03B,      0x00027A50, 0x03B,      0x00018248, 0x03B,      0x00010240,
    0x03B,      0x00008240, 0xB0000000, 0x00000000, 0x0EF,      0x00000100,
    0x80000002, 0x00000000, 0x40000000, 0x00000000, 0x034,      0x0000A4EE,
    0x034,      0x00009076, 0x034,      0x00008073, 0x034,      0x00007070,
    0x034,      0x0000606D, 0x034,      0x0000506A, 0x034,      0x00004049,
    0x034,      0x00003046, 0x034,      0x00002028, 0x034,      0x00001025,
    0x034,      0x00000022, 0xA0000000, 0x00000000, 0x034,      0x0000ADF4,
    0x034,      0x00009DF1, 0x034,      0x00008DEE, 0x034,      0x00007DEB,
    0x034,      0x00006DE8, 0x034,      0x00005DE5, 0x034,      0x00004DE2,
    0x034,      0x00003CE6, 0x034,      0x000024E7, 0x034,      0x000014E4,
    0x034,      0x000004E1, 0xB0000000, 0x00000000, 0x0EF,      0x00000000,
    0x0EF,      0x000020A2, 0x0DF,      0x00000080, 0x035,      0x00000192,
    0x035,      0x00008192, 0x035,      0x00010192, 0x036,      0x00000024,
    0x036,      0x00008024, 0x036,      0x00010024, 0x036,      0x00018024,
    0x0EF,      0x00000000, 0x051,      0x00000C21, 0x052,      0x000006D9,
    0x053,      0x000FC649, 0x054,      0x0000017E, 0x0EF,      0x00000002,
    0x008,      0x00008400, 0x018,      0x0001712A, 0x0EF,      0x00001000,
    0x03A,      0x00000080, 0x03B,      0x0003A02C, 0x03C,      0x00004000,
    0x03A,      0x00000400, 0x03B,      0x0003202C, 0x03C,      0x00010000,
    0x03A,      0x000000A0, 0x03B,      0x0002B064, 0x03C,      0x00004000,
    0x03A,      0x000000D8, 0x03B,      0x00023070, 0x03C,      0x00004000,
    0x03A,      0x00000468, 0x03B,      0x0001B870, 0x03C,      0x00010000,
    0x03A,      0x00000098, 0x03B,      0x00012085, 0x03C,      0x000E4000,
    0x03A,      0x00000418, 0x03B,      0x0000A080, 0x03C,      0x000F0000,
    0x03A,      0x00000418, 0x03B,      0x00002080, 0x03C,      0x00010000,
    0x03A,      0x00000080, 0x03B,      0x0007A02C, 0x03C,      0x00004000,
    0x03A,      0x00000400, 0x03B,      0x0007202C, 0x03C,      0x00010000,
    0x03A,      0x000000A0, 0x03B,      0x0006B064, 0x03C,      0x00004000,
    0x03A,      0x000000D8, 0x03B,      0x00063070, 0x03C,      0x00004000,
    0x03A,      0x00000468, 0x03B,      0x0005B870, 0x03C,      0x00010000,
    0x03A,      0x00000098, 0x03B,      0x00052085, 0x03C,      0x000E4000,
    0x03A,      0x00000418, 0x03B,      0x0004A080, 0x03C,      0x000F0000,
    0x03A,      0x00000418, 0x03B,      0x00042080, 0x03C,      0x00010000,
    0x03A,      0x00000080, 0x03B,      0x000BA02C, 0x03C,      0x00004000,
    0x03A,      0x00000400, 0x03B,      0x000B202C, 0x03C,      0x00010000,
    0x03A,      0x000000A0, 0x03B,      0x000AB064, 0x03C,      0x00004000,
    0x03A,      0x000000D8, 0x03B,      0x000A3070, 0x03C,      0x00004000,
    0x03A,      0x00000468, 0x03B,      0x0009B870, 0x03C,      0x00010000,
    0x03A,      0x00000098, 0x03B,      0x00092085, 0x03C,      0x000E4000,
    0x03A,      0x00000418, 0x03B,      0x0008A080, 0x03C,      0x000F0000,
    0x03A,      0x00000418, 0x03B,      0x00082080, 0x03C,      0x00010000,
    0x0EF,      0x00001100, 0x80000008, 0x00000000, 0x40000000, 0x00000000,
    0x034,      0x0004A0B2, 0x034,      0x000490AF, 0x034,      0x00048070,
    0x034,      0x0004706D, 0x034,      0x00046050, 0x034,      0x0004504D,
    0x034,      0x0004404A, 0x034,      0x00043047, 0x034,      0x0004200A,
    0x034,      0x00041007, 0x034,      0x00040004, 0x90000008, 0x05000000,
    0x40000000, 0x00000000, 0x034,      0x0004A0B1, 0x034,      0x000490AE,
    0x034,      0x0004806F, 0x034,      0x0004706C, 0x034,      0x0004604C,
    0x034,      0x00045049, 0x034,      0x00044046, 0x034,      0x00043043,
    0x034,      0x00042006, 0x034,      0x00041003, 0x034,      0x00040000,
    0xA0000000, 0x00000000, 0x034,      0x0004ADF5, 0x034,      0x00049DF2,
    0x034,      0x00048DEF, 0x034,      0x00047DEC, 0x034,      0x00046DE9,
    0x034,      0x00045DE6, 0x034,      0x00044DE3, 0x034,      0x000438C8,
    0x034,      0x000428C5, 0x034,      0x000418C2, 0x034,      0x000408C0,
    0xB0000000, 0x00000000, 0x80000008, 0x00000000, 0x40000000, 0x00000000,
    0x034,      0x0002A0B2, 0x034,      0x000290AF, 0x034,      0x00028070,
    0x034,      0x0002706D, 0x034,      0x00026050, 0x034,      0x0002504D,
    0x034,      0x0002404A, 0x034,      0x00023047, 0x034,      0x0002200A,
    0x034,      0x00021007, 0x034,      0x00020004, 0x90000008, 0x05000000,
    0x40000000, 0x00000000, 0x034,      0x0002A0B3, 0x034,      0x000290B0,
    0x034,      0x00028071, 0x034,      0x0002706E, 0x034,      0x0002604E,
    0x034,      0x0002504B, 0x034,      0x00024048, 0x034,      0x00023045,
    0x034,      0x00022008, 0x034,      0x00021005, 0x034,      0x00020002,
    0xA0000000, 0x00000000, 0x034,      0x0002ADF5, 0x034,      0x00029DF2,
    0x034,      0x00028DEF, 0x034,      0x00027DEC, 0x034,      0x00026DE9,
    0x034,      0x00025DE6, 0x034,      0x00024DE3, 0x034,      0x000238C8,
    0x034,      0x000228C5, 0x034,      0x000218C2, 0x034,      0x000208C0,
    0xB0000000, 0x00000000, 0x80000008, 0x00000000, 0x40000000, 0x00000000,
    0x034,      0x0000A0B2, 0x034,      0x000090AF, 0x034,      0x00008070,
    0x034,      0x0000706D, 0x034,      0x00006050, 0x034,      0x0000504D,
    0x034,      0x0000404A, 0x034,      0x00003047, 0x034,      0x0000200A,
    0x034,      0x00001007, 0x034,      0x00000004, 0x90000008, 0x05000000,
    0x40000000, 0x00000000, 0x034,      0x0000A0B3, 0x034,      0x000090B0,
    0x034,      0x00008070, 0x034,      0x0000706D, 0x034,      0x0000604D,
    0x034,      0x0000504A, 0x034,      0x00004047, 0x034,      0x00003044,
    0x034,      0x00002007, 0x034,      0x00001004, 0x034,      0x00000001,
    0xA0000000, 0x00000000, 0x034,      0x0000AFF7, 0x034,      0x00009DF7,
    0x034,      0x00008DF4, 0x034,      0x00007DF1, 0x034,      0x00006DEE,
    0x034,      0x00005DEB, 0x034,      0x00004DE8, 0x034,      0x000038CC,
    0x034,      0x000028C9, 0x034,      0x000018C6, 0x034,      0x000008C3,
    0xB0000000, 0x00000000, 0x0EF,      0x00000000, 0x80000008, 0x00000000,
    0x40000000, 0x00000000, 0x018,      0x0001712A, 0x0EF,      0x00000040,
    0x035,      0x000001C5, 0x035,      0x000081C5, 0x035,      0x000101C5,
    0x035,      0x00020174, 0x035,      0x00028174, 0x035,      0x00030174,
    0x035,      0x00040185, 0x035,      0x00048185, 0x035,      0x00050185,
    0x0EF,      0x00000000, 0x90000008, 0x05000000, 0x40000000, 0x00000000,
    0x018,      0x0001712A, 0x0EF,      0x00000040, 0x035,      0x000001C5,
    0x035,      0x000081C5, 0x035,      0x000101C5, 0x035,      0x00020174,
    0x035,      0x00028174, 0x035,      0x00030174, 0x035,      0x00040185,
    0x035,      0x00048185, 0x035,      0x00050185, 0x0EF,      0x00000000,
    0xA0000000, 0x00000000, 0x018,      0x0001712A, 0x0EF,      0x00000040,
    0x035,      0x00000188, 0x035,      0x00008147, 0x035,      0x00010147,
    0x035,      0x000201D7, 0x035,      0x000281D7, 0x035,      0x000301D7,
    0x035,      0x000401D8, 0x035,      0x000481D8, 0x035,      0x000501D8,
    0x0EF,      0x00000000, 0xB0000000, 0x00000000, 0x80000008, 0x00000000,
    0x40000000, 0x00000000, 0x018,      0x0001712A, 0x0EF,      0x00000010,
    0x036,      0x00005B8B, 0x036,      0x0000DB8B, 0x036,      0x00015B8B,
    0x036,      0x0001DB8B, 0x036,      0x000262DB, 0x036,      0x0002E2DB,
    0x036,      0x000362DB, 0x036,      0x0003E2DB, 0x036,      0x0004553B,
    0x036,      0x0004D53B, 0x036,      0x0005553B, 0x036,      0x0005D53B,
    0x90000008, 0x05000000, 0x40000000, 0x00000000, 0x018,      0x0001712A,
    0x0EF,      0x00000010, 0x036,      0x00005B8B, 0x036,      0x0000DB8B,
    0x036,      0x00015B8B, 0x036,      0x0001DB8B, 0x036,      0x000262DB,
    0x036,      0x0002E2DB, 0x036,      0x000362DB, 0x036,      0x0003E2DB,
    0x036,      0x0004553B, 0x036,      0x0004D53B, 0x036,      0x0005553B,
    0x036,      0x0005D53B, 0xA0000000, 0x00000000, 0x018,      0x0001712A,
    0x0EF,      0x00000010, 0x036,      0x00084EB4, 0x036,      0x0008CC35,
    0x036,      0x00094C35, 0x036,      0x0009CC35, 0x036,      0x000A4C35,
    0x036,      0x000ACC35, 0x036,      0x000B4C35, 0x036,      0x000BCC35,
    0x036,      0x000C4C34, 0x036,      0x000CCC35, 0x036,      0x000D4C35,
    0x036,      0x000DCC35, 0xB0000000, 0x00000000, 0x0EF,      0x00000000,
    0x0EF,      0x00000008, 0x80000008, 0x00000000, 0x40000000, 0x00000000,
    0x03C,      0x000002DC, 0x03C,      0x00000524, 0x03C,      0x00000902,
    0x90000008, 0x05000000, 0x40000000, 0x00000000, 0x03C,      0x000002DC,
    0x03C,      0x00000524, 0x03C,      0x00000902, 0xA0000000, 0x00000000,
    0x03C,      0x000002A8, 0x03C,      0x000005A2, 0x03C,      0x00000880,
    0xB0000000, 0x00000000, 0x0EF,      0x00000000, 0x018,      0x0001712A,
    0x0EF,      0x00000002, 0x0DF,      0x00000080, 0x80000008, 0x00000000,
    0x40000000, 0x00000000, 0x061,      0x000EAC43, 0x062,      0x00038F47,
    0x063,      0x00031157, 0x064,      0x0001C4AC, 0x065,      0x000931D1,
    0x90000008, 0x05000000, 0x40000000, 0x00000000, 0x061,      0x000EAC43,
    0x062,      0x00038F47, 0x063,      0x00031157, 0x064,      0x0001C4AC,
    0x065,      0x000931D2, 0x90000002, 0x00000000, 0x40000000, 0x00000000,
    0x061,      0x000EAC43, 0x062,      0x00038F47, 0x063,      0x00031157,
    0x064,      0x0001C4AC, 0x065,      0x000931D1, 0xA0000000, 0x00000000,
    0x061,      0x000E5D53, 0x062,      0x00038FCD, 0x063,      0x000114EB,
    0x064,      0x000196AC, 0x065,      0x000911D7, 0xB0000000, 0x00000000,
    0x008,      0x00008400,

};

void HalModule::odm_set_rf_reg(RfPath e_rf_path, uint16_t reg_addr,
                               uint32_t bit_mask, uint32_t data) {
  _radioManagementModule->phy_set_rf_reg(e_rf_path, reg_addr, bit_mask, data);
}

void HalModule::odm_read_and_config_mp_8812a_radiob() {
  uint32_t i = 0;
  uint8_t c_cond;
  bool is_matched = true, is_skipped = false;
  uint32_t array_len = ARRAY_LENGTH(array_mp_8812a_radiob);

  uint32_t pre_v1 = 0, pre_v2 = 0;

  // PHYDM_DBG(dm, ODM_COMP_INIT, "===> %s\n", __func__);

  while ((i + 1) < array_len) {
    auto v1 = array_mp_8812a_radiob[i];
    auto v2 = array_mp_8812a_radiob[i + 1];

    if ((v1 & (BIT31 | BIT30)) != 0) {
      /*positive & negative condition*/
      if ((v1 & BIT31) != 0) {
        /* positive condition*/
        c_cond = (uint8_t)((v1 & (BIT29 | BIT28)) >> 28);
        if (c_cond == COND_ENDIF) {
          /*end*/
          is_matched = true;
          is_skipped = false;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "ENDIF\n");
        } else if (c_cond == COND_ELSE) {
          /*else*/
          is_matched = is_skipped ? false : true;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "ELSE\n");
        } else {
          /*if , else if*/
          pre_v1 = v1;
          pre_v2 = v2;
          // PHYDM_DBG(dm, ODM_COMP_INIT, "IF or ELSE IF\n");
        }
      } else if ((v1 & BIT30) != 0) {
        /*negative condition*/
        if (is_skipped == false) {
          if (check_positive(pre_v1, pre_v2, v2)) {
            is_matched = true;
            is_skipped = true;
          } else {
            is_matched = false;
            is_skipped = false;
          }
        } else
          is_matched = false;
      }
    } else {
      if (is_matched) {
        odm_config_rf_radio_b_8812a(v1, v2);
      }
    }

    i = i + 2;
  }
}

void HalModule::odm_config_rf_radio_b_8812a(uint32_t addr, uint32_t data) {
  uint32_t content = 0x1001; /* RF_Content: radiob_txt */
  uint32_t maskfor_phy_set = (uint32_t)(content & 0xE000);

  odm_config_rf_reg_8812a(addr, data, RfPath::RF_PATH_B,
                          (uint16_t)(addr | maskfor_phy_set));
}

void HalModule::PHY_BB8812_Config_1T() {
  /* BB OFDM RX Path_A */
  _device.phy_set_bb_reg(rRxPath_Jaguar, bRxPath_Jaguar, 0x11);
  /* BB OFDM TX Path_A */
  _device.phy_set_bb_reg(rTxPath_Jaguar, bMaskLWord, 0x1111);
  /* BB CCK R/Rx Path_A */
  _device.phy_set_bb_reg(rCCK_RX_Jaguar, bCCK_RX_Jaguar, 0x0);
  /* MCS support */
  _device.phy_set_bb_reg(0x8bc, 0xc0000060, 0x4);
  /* RF Path_B HSSI OFF */
  _device.phy_set_bb_reg(0xe00, 0xf, 0x4);
  /* RF Path_B Power Down */
  _device.phy_set_bb_reg(0xe90, bMaskDWord, 0);
  /* ADDA Path_B OFF */
  _device.phy_set_bb_reg(0xe60, bMaskDWord, 0);
  _device.phy_set_bb_reg(0xe64, bMaskDWord, 0);
}
