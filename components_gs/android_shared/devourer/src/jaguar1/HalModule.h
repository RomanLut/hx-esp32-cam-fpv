#ifndef HALMODULE_H
#define HALMODULE_H

#include "AdapterHealth.h"
#include "EepromManager.h"
extern "C"{
#include "Hal8812PwrSeq.h"
#include "Hal8814PwrSeq.h"
#include "Hal8821APwrSeq.h"
}
#include "PhydmWatchdog.h"
#include "RadioManagementModule.h"
#include "RtlAdapter.h"
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
  RtlAdapter _device;
  devourer::DeviceConfig _cfg; /* phydm_watchdog gate + FirmwareManager forward */
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
  /* Phydm DM watchdog. Lazily constructed in `rtw_hal_init` after
   * `RadioManagementModule` is fully wired up. */
  std::unique_ptr<PhydmWatchdog> _phydmWatchdog;
  /* Outcome of the fw download run by the last rtw_hal_init (the
   * FirmwareManager is a hal-init local; its status is copied out here). */
  devourer::FwBootStatus _fwBoot;

public:
  HalModule(RtlAdapter device, std::shared_ptr<EepromManager> eepromManager,
            std::shared_ptr<RadioManagementModule> _radioManagementModule,
            Logger_t logger, const devourer::DeviceConfig &cfg = {});
  bool rtw_hal_init(SelectedChannel selectedChannel);
  const devourer::FwBootStatus &GetFwBootStatus() const { return _fwBoot; }

private:
  bool rtl8812au_hal_init(uint8_t init_channel);
  bool InitPowerOn();
  bool InitLLTTable8812A(uint8_t txpktbuf_bndy);
  bool _LLTWrite_8812A(uint32_t address, uint32_t data);
  void _InitHardwareDropIncorrectBulkOut_8812A();
  bool HalPwrSeqCmdParsing(WLAN_PWR_CFG *PwrSeqCmd);
  void PHY_MACConfig8812();
  void odm_read_and_config_mp_8812a_mac_reg();
  void odm_read_and_config_mp_8814a_mac_reg();
  void odm_read_and_config_mp_8814a_phy_reg();
  void odm_read_and_config_mp_8814a_agc_tab();
  void odm_read_and_config_mp_8821a_mac_reg();
  void odm_read_and_config_mp_8821a_phy_reg();
  void odm_read_and_config_mp_8821a_agc_tab();
  void odm_read_and_config_mp_8821a_radioa();
  bool phy_BB8814_Config_ParaFile();
  bool phy_BB8821_Config_ParaFile();
  void phy_RF6052_Config_ParaFile_8821();
  void odm_write_1byte(uint16_t reg_addr, uint8_t data);
  bool check_positive(int32_t condition1, int32_t condition2,
                      int32_t condition4);
  void _InitQueueReservedPage_8812AUsb();
  void _InitQueueReservedPage_8821AUsb();
  void _InitTxBufferBoundary_8812AUsb();
  void _InitTxBufferBoundary_8821AUsb();
  void _InitQueuePriority_8812AUsb();
  void _InitNormalChipTwoOutEpPriority_8812AUsb();
  void _InitNormalChipRegPriority_8812AUsb(uint16_t beQ, uint16_t bkQ,
                                           uint16_t viQ, uint16_t voQ,
                                           uint16_t mgtQ, uint16_t hiQ);
  void _InitNormalChipThreeOutEpPriority_8812AUsb();
  void _InitNormalChipFourOutEpPriority_8812AUsb();
  void _InitNormalChipTwoOutEpPriority_8814AUsb();
  void _InitNormalChipThreeOutEpPriority_8814AUsb();
  void _InitNormalChipRegPriority_8814AUsb(uint16_t beQ, uint16_t bkQ,
                                           uint16_t viQ, uint16_t voQ,
                                           uint16_t mgtQ, uint16_t hiQ);
  void _InitQueueReservedPage_8814AUsb();
  void _InitPageBoundary_8814AUsb();
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
  void _InitBurstPktLen_8814A();

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
  void phy_RF6052_Config_ParaFile_8814();
  void odm_read_and_config_mp_8814a_radioa();
  void odm_read_and_config_mp_8814a_radiob();
  void odm_read_and_config_mp_8814a_radioc();
  void odm_read_and_config_mp_8814a_radiod();
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

  /* Pre-converge the path-A/B Initial Gain Index to phydm's documented
   * DIG floor (0x1c) for all Jaguar chips. Upstream phydm runs DIG
   * (Dynamic Initial Gain) from its watchdog and converges 0xc50/0xe50
   * to 0x1c when no false-alarm pressure is present (phydm_dig.c:
   * `dig_t->dm_dig_min = 0x1c` for ODM_RTL8812 | RTL8814A | RTL8821).
   * Devourer has no phydm watchdog, so devourer's static 0x20 (from the
   * BB-init table) never moves and the chip runs slightly less sensitive
   * than the kernel driver. Writing 0x1c once at init matches kernel
   * post-DIG-converged state — last remaining T1 canary divergence on
   * 0xc50/0xe50 that wasn't a real init bug. Safe for monitor RX:
   * higher sensitivity, lower CCA threshold, no TX-side impact. */
  void phydm_SetIgiFloor_Jaguar();
};

#endif /* HALMODULE_H */
