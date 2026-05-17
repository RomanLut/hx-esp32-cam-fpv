#ifndef RADIOMANAGEMENTMODULE_H
#define RADIOMANAGEMENTMODULE_H

#include <cstdint>
#include <vector>

#include "EepromManager.h"
#include "RfPath.h"
#include "RtlUsbAdapter.h"
#include "SelectedChannel.h"
#include "logger.h"

#define HAL_PRIME_CHNL_OFFSET_DONT_CARE 0
#define HAL_PRIME_CHNL_OFFSET_LOWER 1
#define HAL_PRIME_CHNL_OFFSET_UPPER 2

enum class HwPort { HW_PORT0, HW_PORT1 };

enum class BandType {
  BAND_ON_2_4G = 0,
  BAND_ON_5G = 1,
  BAND_ON_BOTH = 2,
  BAND_MAX = 3,
};

enum RATE_SECTION {
  CCK = 0,
  OFDM = 1,
  HT_MCS0_MCS7 = 2,
  HT_MCS8_MCS15 = 3,
  HT_MCS16_MCS23 = 4,
  HT_MCS24_MCS31 = 5,
  HT_1SS = HT_MCS0_MCS7,
  HT_2SS = HT_MCS8_MCS15,
  HT_3SS = HT_MCS16_MCS23,
  HT_4SS = HT_MCS24_MCS31,
  VHT_1SSMCS0_1SSMCS9 = 6,
  VHT_2SSMCS0_2SSMCS9 = 7,
  VHT_3SSMCS0_3SSMCS9 = 8,
  VHT_4SSMCS0_4SSMCS9 = 9,
  VHT_1SS = VHT_1SSMCS0_1SSMCS9,
  VHT_2SS = VHT_2SSMCS0_2SSMCS9,
  VHT_3SS = VHT_3SSMCS0_3SSMCS9,
  VHT_4SS = VHT_4SSMCS0_4SSMCS9,
  RATE_SECTION_NUM,
};

enum MGN_RATE {
  MGN_1M = 0x02,
  MGN_2M = 0x04,
  MGN_5_5M = 0x0B,
  MGN_6M = 0x0C,
  MGN_9M = 0x12,
  MGN_11M = 0x16,
  MGN_12M = 0x18,
  MGN_18M = 0x24,
  MGN_24M = 0x30,
  MGN_36M = 0x48,
  MGN_48M = 0x60,
  MGN_54M = 0x6C,
  MGN_MCS32 = 0x7F,
  MGN_MCS0,
  MGN_MCS1,
  MGN_MCS2,
  MGN_MCS3,
  MGN_MCS4,
  MGN_MCS5,
  MGN_MCS6,
  MGN_MCS7,
  MGN_MCS8,
  MGN_MCS9,
  MGN_MCS10,
  MGN_MCS11,
  MGN_MCS12,
  MGN_MCS13,
  MGN_MCS14,
  MGN_MCS15,
  MGN_MCS16,
  MGN_MCS17,
  MGN_MCS18,
  MGN_MCS19,
  MGN_MCS20,
  MGN_MCS21,
  MGN_MCS22,
  MGN_MCS23,
  MGN_MCS24,
  MGN_MCS25,
  MGN_MCS26,
  MGN_MCS27,
  MGN_MCS28,
  MGN_MCS29,
  MGN_MCS30,
  MGN_MCS31,
  MGN_VHT1SS_MCS0,
  MGN_VHT1SS_MCS1,
  MGN_VHT1SS_MCS2,
  MGN_VHT1SS_MCS3,
  MGN_VHT1SS_MCS4,
  MGN_VHT1SS_MCS5,
  MGN_VHT1SS_MCS6,
  MGN_VHT1SS_MCS7,
  MGN_VHT1SS_MCS8,
  MGN_VHT1SS_MCS9,
  MGN_VHT2SS_MCS0,
  MGN_VHT2SS_MCS1,
  MGN_VHT2SS_MCS2,
  MGN_VHT2SS_MCS3,
  MGN_VHT2SS_MCS4,
  MGN_VHT2SS_MCS5,
  MGN_VHT2SS_MCS6,
  MGN_VHT2SS_MCS7,
  MGN_VHT2SS_MCS8,
  MGN_VHT2SS_MCS9,
  MGN_VHT3SS_MCS0,
  MGN_VHT3SS_MCS1,
  MGN_VHT3SS_MCS2,
  MGN_VHT3SS_MCS3,
  MGN_VHT3SS_MCS4,
  MGN_VHT3SS_MCS5,
  MGN_VHT3SS_MCS6,
  MGN_VHT3SS_MCS7,
  MGN_VHT3SS_MCS8,
  MGN_VHT3SS_MCS9,
  MGN_VHT4SS_MCS0,
  MGN_VHT4SS_MCS1,
  MGN_VHT4SS_MCS2,
  MGN_VHT4SS_MCS3,
  MGN_VHT4SS_MCS4,
  MGN_VHT4SS_MCS5,
  MGN_VHT4SS_MCS6,
  MGN_VHT4SS_MCS7,
  MGN_VHT4SS_MCS8,
  MGN_VHT4SS_MCS9,
  MGN_UNKNOWN
};

class RadioManagementModule {
  RtlUsbAdapter _device;
  std::shared_ptr<EepromManager> _eepromManager;
  Logger_t _logger;
  HwPort _hwPort = HwPort::HW_PORT0;
  bool _needIQK = false;
  ChannelWidth_t _currentChannelBw;
  uint8_t _currentChannel;
  BandType current_band_type;
  bool _swChannel = false;
  bool _channelBwInitialized = false;
  bool _setChannelBw = false;
  uint8_t _cur40MhzPrimeSc;
  uint8_t _cur80MhzPrimeSc;
  uint8_t _currentCenterFrequencyIndex;
  uint8_t power = 16;

public:
  RadioManagementModule(RtlUsbAdapter device,
                        std::shared_ptr<EepromManager> eepromManager,
                        Logger_t logger);
  void hw_var_rcr_config(uint32_t rcr);
  void SetMonitorMode();
  void set_channel_bwmode(uint8_t channel, uint8_t channel_offset,
                          ChannelWidth_t bwmode);
  void phy_set_rf_reg(RfPath eRFPath, uint16_t RegAddr, uint32_t BitMask,
                      uint32_t Data);
  void init_hw_mlme_ext(SelectedChannel pmlmeext);
  void rtw_hal_set_chnl_bw(uint8_t channel, ChannelWidth_t Bandwidth,
                           uint8_t Offset40, uint8_t Offset80);
  void PHY_SwitchWirelessBand8812(BandType Band);
  void SetTxPower(uint8_t p);

private:
  void rtw_hal_set_msr(uint8_t net_type);
  void hw_var_set_monitor();
  void PHY_SetSwChnlBWMode8812(uint8_t channel, ChannelWidth_t Bandwidth,
                               uint8_t Offset40, uint8_t Offset80);
  void PHY_HandleSwChnlAndSetBW8812(bool bSwitchChannel, bool bSetBandWidth,
                                    uint8_t ChannelNum,
                                    ChannelWidth_t ChnlWidth,
                                    uint8_t ChnlOffsetOf40MHz,
                                    uint8_t ChnlOffsetOf80MHz,
                                    uint8_t CenterFrequencyIndex1);
  void phy_SwChnlAndSetBwMode8812();
  uint32_t phy_RFSerialRead(RfPath eRFPath, uint32_t Offset);
  void phy_RFSerialWrite(RfPath eRFPath, uint32_t Offset, uint32_t Data);
  void phy_SetRFEReg8812(BandType Band);
  void phy_SetBBSwingByBand_8812A(BandType Band);
  uint32_t phy_get_tx_bb_swing_8812a(BandType Band, RfPath RFPath);
  void Set_HW_VAR_ENABLE_RX_BAR(bool val);
  void phy_SwChnl8812();
  bool phy_SwBand8812(uint8_t channelToSW);
  void phy_FixSpur_8812A(ChannelWidth_t Bandwidth, uint8_t Channel);
  void phy_PostSetBwMode8812();
  void phy_SetRegBW_8812(ChannelWidth_t CurrentBW);
  void PHY_RF6052SetBandwidth8812(ChannelWidth_t Bandwidth);
  uint8_t phy_GetSecondaryChnl_8812();
  void PHY_SetTxPowerLevel8812(uint8_t Channel);
  void phy_set_tx_power_level_by_path(uint8_t channel, RfPath path);
  void phy_set_tx_power_index_by_rate_section(RfPath rfPath, uint8_t channel,
                                              RATE_SECTION rateSection);
  void PHY_TxPowerTrainingByPath_8812(RfPath rfPath);
  void PHY_SetTxPowerIndexByRateArray(RfPath rfPath,
                                      const std::vector<MGN_RATE> &rates);
  void PHY_SetTxPowerIndex_8812A(uint32_t powerIndex, RfPath rfPath,
                                 MGN_RATE rate);
  uint32_t phy_query_bb_reg(uint16_t regAddr, uint32_t bitMask);
  uint32_t PHY_QueryBBReg8812(uint16_t regAddr, uint32_t bitMask);
};

#endif /* RADIOMANAGEMENTMODULE_H */
