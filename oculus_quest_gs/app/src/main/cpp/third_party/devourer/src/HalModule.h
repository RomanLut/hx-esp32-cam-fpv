#ifndef HALMODULE_H
#define HALMODULE_H

#include "EepromManager.h"
extern "C"{
#include "Hal8812PwrSeq.h"
}
#include "RadioManagementModule.h"
#include "RtlUsbAdapter.h"
#include "SelectedChannel.h"
#include "logger.h"

typedef enum _RX_AGG_MODE {
  RX_AGG_DISABLE,
  RX_AGG_DMA,
  RX_AGG_USB,
  RX_AGG_MIX
} RX_AGG_MODE;

enum odm_bb_config_type {
  CONFIG_BB_PHY_REG,
  CONFIG_BB_AGC_TAB,
  CONFIG_BB_AGC_TAB_2G,
  CONFIG_BB_AGC_TAB_5G,
  CONFIG_BB_PHY_REG_PG,
  CONFIG_BB_PHY_REG_MP,
  CONFIG_BB_AGC_TAB_DIFF,
  CONFIG_BB_RF_CAL_INIT,
};

enum odm_rf_config_type {
  CONFIG_RF_RADIO,
  CONFIG_RF_TXPWR_LMT,
  CONFIG_RF_SYN_RADIO,
};

class HalModule {
  RtlUsbAdapter _device;
  std::shared_ptr<EepromManager> _eepromManager;
  std::shared_ptr<RadioManagementModule> _radioManagementModule;
  Logger_t _logger;
  bool _macPwrCtrlOn = false;
  uint32_t _intrMask[3] = {0};
  bool _usbTxAggMode = false;
  uint8_t _usbTxAggDescNum =
      0x01; // adjust value for OQT Overflow issue 0x3; only 4 bits
  RX_AGG_MODE _rxAggMode = RX_AGG_USB;
  uint8_t _rxAggDmaTimeout = 0x6; /* 6, absolute time = 34ms/(2^6) */
  uint8_t _rxAggDmaSize =
      16; /* uint: 128b, 0x0A = 10 =
             MAX_RX_DMA_BUFFER_SIZE/2/pHalData.UsbBulkOutSize */

public:
  HalModule(RtlUsbAdapter device, std::shared_ptr<EepromManager> eepromManager,
            std::shared_ptr<RadioManagementModule> _radioManagementModule,
            Logger_t logger);
  bool rtw_hal_init(SelectedChannel selectedChannel);

private:
  bool rtl8812au_hal_init();
  bool InitPowerOn();
  bool InitLLTTable8812A(uint8_t txpktbuf_bndy);
  bool _LLTWrite_8812A(uint32_t address, uint32_t data);
  void _InitHardwareDropIncorrectBulkOut_8812A();
  bool HalPwrSeqCmdParsing(WLAN_PWR_CFG *PwrSeqCmd);
  void PHY_MACConfig8812();
  void odm_read_and_config_mp_8812a_mac_reg();
  void odm_write_1byte(uint16_t reg_addr, uint8_t data);
  bool check_positive(int32_t condition1, int32_t condition2,
                      int32_t condition4);
  void _InitQueueReservedPage_8812AUsb();
  void _InitTxBufferBoundary_8812AUsb();
  void _InitQueuePriority_8812AUsb();
  void _InitNormalChipTwoOutEpPriority_8812AUsb();
  void _InitNormalChipRegPriority_8812AUsb(uint16_t beQ, uint16_t bkQ,
                                           uint16_t viQ, uint16_t voQ,
                                           uint16_t mgtQ, uint16_t hiQ);
  void _InitNormalChipThreeOutEpPriority_8812AUsb();
  void _InitNormalChipFourOutEpPriority_8812AUsb();
  void init_hi_queue_config_8812a_usb();
  void _InitPageBoundary_8812AUsb();
  void _InitTransferPageSize_8812AUsb();
  void _InitDriverInfoSize_8812A(uint8_t drvInfoSize);
  void _InitInterrupt_8812AU();
  void _InitNetworkType_8812A();
  void _InitWMACSetting_8812A();
  void _InitAdaptiveCtrl_8812AUsb();
  void _InitEDCA_8812AUsb();
  void _InitRetryFunction_8812A();

  void init_UsbAggregationSetting_8812A();
  void usb_AggSettingRxUpdate_8812A();
  void usb_AggSettingTxUpdate_8812A();
  void _InitBeaconParameters_8812A();
  void _InitBeaconMaxError_8812A();
  void _InitBurstPktLen();

  bool PHY_BBConfig8812();
  bool phy_BB8812_Config_ParaFile();
  void hal_set_crystal_cap(uint8_t crystal_cap);
  bool odm_config_bb_with_header_file(odm_bb_config_type config_type);
  void odm_read_and_config_mp_8812a_phy_reg();
  void odm_read_and_config_mp_8812a_phy_reg_mp();
  void odm_read_and_config_mp_8812a_agc_tab();

  void odm_config_bb_phy_8812a(uint32_t addr, uint32_t bitmask, uint32_t data);
  void odm_set_bb_reg(uint32_t reg_addr, uint32_t bit_mask, uint32_t data);
  void odm_config_bb_agc_8812a(uint32_t addr, uint32_t bitmask, uint32_t data);

  void PHY_RF6052_Config_8812();
  void phy_RF6052_Config_ParaFile_8812();
  void odm_config_rf_with_header_file(odm_rf_config_type config_type,
                                      RfPath e_rf_path);
  void odm_read_and_config_mp_8812a_radioa();
  void odm_read_and_config_mp_8812a_radiob();
  void odm_config_rf_radio_a_8812a(uint32_t addr, uint32_t data);
  void odm_config_rf_reg_8812a(uint32_t addr, uint32_t data, RfPath RF_PATH,
                               uint16_t reg_addr);
  void odm_set_rf_reg(RfPath e_rf_path, uint16_t reg_addr, uint32_t bit_mask,
                      uint32_t data);
  void odm_config_rf_radio_b_8812a(uint32_t addr, uint32_t data);
       void PHY_BB8812_Config_1T();

};

#endif /* HALMODULE_H */
