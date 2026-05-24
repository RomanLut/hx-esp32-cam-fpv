#include "RadioManagementModule.h"
#include "Hal8812PhyReg.h"
#include "registry_priv.h"

extern "C" {
#include "ieee80211_radiotap.h"
}

#include <chrono>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

int get_40mhz_center_channel(int channel) {
    static const std::unordered_map<int, int> channel_map = {
        // 2.4GHz 40MHz configuration: only one valid center channel (6)
        {4, 6},  // primary below center (extension above)
        {8, 6},  // primary above center (extension below)

        // 5GHz UNII-1
        {36, 38}, {40, 38},
        // 5GHz UNII-1 / UNII-2
        {44, 46}, {48, 46},
        // 5GHz UNII-2A
        {52, 54}, {56, 54}, {60, 62}, {64, 62},
        // 5GHz UNII-2C
        {100, 102}, {104, 102},
        {108, 110}, {112, 110},
        {116, 118}, {120, 118},
        {124, 126}, {128, 126},
        // 5GHz UNII-2E (if applicable)
        {132, 134}, {136, 134},
        {140, 142}, {144, 142},
        // 5GHz UNII-3
        {149, 151}, {153, 155},
        {157, 159}, {161, 163}
    };

    auto it = channel_map.find(channel);
    return (it != channel_map.end()) ? it->second : channel;
}

}

RadioManagementModule::RadioManagementModule(
    RtlUsbAdapter device, std::shared_ptr<EepromManager> eepromManager,
    Logger_t logger)
    : _device{device}, _eepromManager{eepromManager}, _logger{logger} {}

void RadioManagementModule::hw_var_rcr_config(uint32_t rcr) {
  _device.rtw_write32(REG_RCR, rcr);
}

#define _HW_STATE_NOLINK_ 0x00

void RadioManagementModule::SetMonitorMode() {
  rtw_hal_set_msr(_HW_STATE_NOLINK_);
  hw_var_set_monitor();
}

void RadioManagementModule::rtw_hal_set_msr(uint8_t net_type) {
  switch (_hwPort) {
  case HwPort::HW_PORT0: {
    /*REG_CR - BIT[17:16]-Network Type for port 0*/
    auto val8 = _device.rtw_read8(MSR) & 0x0C;
    val8 |= net_type;
    _device.rtw_write8(MSR, val8);
    break;
  }
  case HwPort::HW_PORT1: {
    /*REG_CR - BIT[19:18]-Network Type for port 1*/
    auto val8 = _device.rtw_read8(MSR) & 0x03;
    val8 |= net_type << 2;
    _device.rtw_write8(MSR, val8);
    break;
  }

  default:
    throw std::logic_error("not yet implemented");
    break;
  }
}

void RadioManagementModule::hw_var_set_monitor() {
  /* Receive all type */
  uint32_t rcr_bits = RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_APWRMGT |
                      RCR_ADF | RCR_ACF | RCR_AMF | RCR_APP_PHYST_RXFF;

  /* Append FCS */
  rcr_bits |= RCR_APPFCS;

  // rtw_hal_get_hwreg(adapterState, HW_VAR_RCR, pHalData.rcr_backup);
  hw_var_rcr_config(rcr_bits);

  /* Receive all data frames */
  uint16_t value_rxfltmap2 = 0xFFFF;
  _device.rtw_write16(REG_RXFLTMAP2, value_rxfltmap2);
}

static uint8_t rtw_get_center_ch(uint8_t channel, ChannelWidth_t chnl_bw,
                                 uint8_t chnl_offset) {
  uint8_t center_ch = channel;

  if (chnl_bw == ChannelWidth_t::CHANNEL_WIDTH_80) {
    if (channel == 36 || channel == 40 || channel == 44 || channel == 48)
      center_ch = 42;
    else if (channel == 52 || channel == 56 || channel == 60 || channel == 64)
      center_ch = 58;
    else if (channel == 100 || channel == 104 || channel == 108 ||
             channel == 112)
      center_ch = 106;
    else if (channel == 116 || channel == 120 || channel == 124 ||
             channel == 128)
      center_ch = 122;
    else if (channel == 132 || channel == 136 || channel == 140 ||
             channel == 144)
      center_ch = 138;
    else if (channel == 149 || channel == 153 || channel == 157 ||
             channel == 161)
      center_ch = 155;
    else if (channel == 165 || channel == 169 || channel == 173 ||
             channel == 177)
      center_ch = 171;
    else if (channel <= 14)
      center_ch = 7;
  } else if (chnl_bw == ChannelWidth_t::CHANNEL_WIDTH_40) {
      center_ch = get_40mhz_center_channel(center_ch);
  } else if (chnl_bw == ChannelWidth_t::CHANNEL_WIDTH_20) {
    center_ch = channel;
  } else {
    throw std::logic_error("not yet implemented");
  }

  return center_ch;
}

void RadioManagementModule::set_channel_bwmode(uint8_t channel,
                                               uint8_t channel_offset,
                                               ChannelWidth_t bwmode) {
  uint8_t center_ch, chnl_offset80 = HAL_PRIME_CHNL_OFFSET_DONT_CARE;

  _logger->info("[{}] ch = {}, offset = {}, bwmode = {}", __func__, unsigned(channel),
                unsigned(channel_offset), (int)bwmode);

  center_ch = rtw_get_center_ch(channel, bwmode, channel_offset);
  if (bwmode == ChannelWidth_t::CHANNEL_WIDTH_80) {
    if (center_ch > channel) {
      chnl_offset80 = HAL_PRIME_CHNL_OFFSET_LOWER;
    } else if (center_ch < channel) {
      chnl_offset80 = HAL_PRIME_CHNL_OFFSET_UPPER;
    } else {
      chnl_offset80 = HAL_PRIME_CHNL_OFFSET_DONT_CARE;
    }
  }

  rtw_hal_set_chnl_bw(center_ch, bwmode, channel_offset,
                      chnl_offset80); /* set center channel */
}

void RadioManagementModule::rtw_hal_set_chnl_bw(uint8_t channel,
                                                ChannelWidth_t Bandwidth,
                                                uint8_t Offset40,
                                                uint8_t Offset80) {
  PHY_SetSwChnlBWMode8812(channel, Bandwidth, Offset40, Offset80);
}

void RadioManagementModule::PHY_SetSwChnlBWMode8812(uint8_t channel,
                                                    ChannelWidth_t Bandwidth,
                                                    uint8_t Offset40,
                                                    uint8_t Offset80) {
  PHY_HandleSwChnlAndSetBW8812(true, true, channel, Bandwidth, Offset40,
                               Offset80, channel);
}

void RadioManagementModule::PHY_HandleSwChnlAndSetBW8812(
    bool bSwitchChannel, bool bSetBandWidth, uint8_t ChannelNum,
    ChannelWidth_t ChnlWidth, uint8_t ChnlOffsetOf40MHz,
    uint8_t ChnlOffsetOf80MHz, uint8_t CenterFrequencyIndex1) {
  _logger->info(
      "[{}] bSwitchChannel {}, bSetBandWidth {}", __func__,
      bSwitchChannel, bSetBandWidth);

  /* check is swchnl or setbw */
  if (!bSwitchChannel && !bSetBandWidth) {
    _logger->error("[{}]: not switch channel and not set bandwidth", __func__);
    return;
  }

  /* skip change for channel or bandwidth is the same */
  if (bSwitchChannel) {
    if (_currentChannel != ChannelNum) {
      _swChannel = true;
    }
  }

  if (bSetBandWidth) {
    if (_channelBwInitialized == false) {
      _channelBwInitialized = true;
      _setChannelBw = true;
    } else if ((_currentChannelBw != ChnlWidth) ||
               (_cur40MhzPrimeSc != ChnlOffsetOf40MHz) ||
               (_cur80MhzPrimeSc != ChnlOffsetOf80MHz) ||
               (_currentCenterFrequencyIndex != CenterFrequencyIndex1)) {
      _setChannelBw = true;
    }
  }

  if (!_setChannelBw && !_swChannel && _needIQK != true) {
    _logger->error("[{}]: _swChannel {}, _setChannelBw {}", __func__,
                   _swChannel, _setChannelBw);
    return;
  }

  if (_swChannel) {
    _currentChannel = ChannelNum;
    _currentCenterFrequencyIndex = ChannelNum;
  }

  if (_setChannelBw) {
    _currentChannelBw = ChnlWidth;
    _cur40MhzPrimeSc = ChnlOffsetOf40MHz;
    _cur80MhzPrimeSc = ChnlOffsetOf80MHz;
    _currentCenterFrequencyIndex = CenterFrequencyIndex1;
  }

  /* Switch workitem or set timer to do switch channel or setbandwidth operation
   */
  phy_SwChnlAndSetBwMode8812();
}

void RadioManagementModule::phy_SwChnlAndSetBwMode8812() {
  if (_swChannel) {
    phy_SwChnl8812();
    _swChannel = false;
  }

  if (_setChannelBw) {
    phy_PostSetBwMode8812();
    _setChannelBw = false;
  }
  /* 8814A uses a packed single-DWord write to BB 0x1998 per (path,rate)
   * instead of the 8812's per-rate per-byte register fanout (see the
   * CHIP_8814A branch at the top of PHY_SetTxPowerIndex_8812A). The
   * earlier "skip TX power on 8814" workaround was a symptom of the
   * 8812 register layout being wrong for 8814 — every write hit the
   * wrong bits and the chip's BB stalled on each one. Setting
   * DEVOURER_SKIP_TXPWR=1 keeps the old skip behaviour as an escape
   * hatch (e.g. for RX-only experiments). */
  if (std::getenv("DEVOURER_SKIP_TXPWR")) {
    _logger->info("DEVOURER_SKIP_TXPWR=1 — skipping TX power setup");
  } else {
    PHY_SetTxPowerLevel8812(_currentChannel);
  }

  _needIQK = false;
}

void RadioManagementModule::phy_set_rf_reg(RfPath eRFPath, uint16_t RegAddr,
                                           uint32_t BitMask, uint32_t Data) {
  uint32_t data = Data;
  //_logger->debug("RFREG;{};{:X};{:X};{:X}", (uint8_t)eRFPath, (uint)RegAddr,
  //               BitMask, data);
  if (BitMask == 0) {
    return;
  }

  /* RF data is 20 bits only */
  if (BitMask != bLSSIWrite_data_Jaguar) {
    uint32_t Original_Value, BitShift;
    Original_Value = phy_RFSerialRead(eRFPath, RegAddr);
    BitShift = PHY_CalculateBitShift(BitMask);
    data = ((Original_Value) & (~BitMask)) | (data << (int)BitShift);
  }

  phy_RFSerialWrite(eRFPath, RegAddr, data);
}

struct BbRegisterDefinition {
  /// LSSI data
  uint32_t Rf3WireOffset;

  /// wire parameter control2
  uint32_t RfHSSIPara2;

  /// LSSI RF readback data SI mode
  uint16_t RfLSSIReadBack;

  /// LSSI RF readback data PI mode 0x8b8-8bc for Path A and B
  uint16_t RfLSSIReadBackPi;
};

/* RTL8814AU path C / D BB register offsets — from hal/Hal8814PhyReg.h.
 * Inlined here to avoid pulling the full 8814 PHY register header (which has
 * extensive #define overlap with Hal8812PhyReg.h on shared Jaguar symbols). */
constexpr uint32_t rC_LSSIWrite_8814A = 0x1890;
constexpr uint32_t rD_LSSIWrite_8814A = 0x1A90;
constexpr uint16_t rC_SIRead_8814A = 0xd88;
constexpr uint16_t rD_SIRead_8814A = 0xdC8;
constexpr uint16_t rC_PIRead_8814A = 0xd84;
constexpr uint16_t rD_PIRead_8814A = 0xdC4;

std::map<RfPath, BbRegisterDefinition> PhyRegDef = {
    {RfPath::RF_PATH_A,
     {
         .Rf3WireOffset = rA_LSSIWrite_Jaguar,
         .RfHSSIPara2 = rHSSIRead_Jaguar,
         .RfLSSIReadBack = rA_SIRead_Jaguar,
         .RfLSSIReadBackPi = rA_PIRead_Jaguar,
     }},
    {RfPath::RF_PATH_B,
     {
         .Rf3WireOffset = rB_LSSIWrite_Jaguar,
         .RfHSSIPara2 = rHSSIRead_Jaguar,
         .RfLSSIReadBack = rB_SIRead_Jaguar,
         .RfLSSIReadBackPi = rB_PIRead_Jaguar,
     }},
    {RfPath::RF_PATH_C,
     {
         .Rf3WireOffset = rC_LSSIWrite_8814A,
         .RfHSSIPara2 = rHSSIRead_Jaguar,
         .RfLSSIReadBack = rC_SIRead_8814A,
         .RfLSSIReadBackPi = rC_PIRead_8814A,
     }},
    {RfPath::RF_PATH_D,
     {
         .Rf3WireOffset = rD_LSSIWrite_8814A,
         .RfHSSIPara2 = rHSSIRead_Jaguar,
         .RfLSSIReadBack = rD_SIRead_8814A,
         .RfLSSIReadBackPi = rD_PIRead_8814A,
     }}};

uint32_t RadioManagementModule::phy_query_bb_reg(uint16_t regAddr,
                                                 uint32_t bitMask) {
  return PHY_QueryBBReg8812(regAddr, bitMask);
}

uint32_t RadioManagementModule::PHY_QueryBBReg8812(uint16_t regAddr,
                                                   uint32_t bitMask) {
  uint32_t ReturnValue, OriginalValue, BitShift;

  /* RTW_INFO("--.PHY_QueryBBReg8812(): RegAddr(%#x), BitMask(%#x)\n", RegAddr,
   * BitMask); */

  OriginalValue = _device.rtw_read32(regAddr);
  BitShift = PHY_CalculateBitShift(bitMask);
  ReturnValue = (OriginalValue & bitMask) >> (int)BitShift;

  /* RTW_INFO("BBR MASK=0x%x Addr[0x%x]=0x%x\n", BitMask, RegAddr,
   * OriginalValue); */
  return ReturnValue;
}

uint32_t RadioManagementModule::phy_query_rf_reg(RfPath eRFPath,
                                                 uint32_t RegAddr,
                                                 uint32_t BitMask) {
  uint32_t val = phy_RFSerialRead(eRFPath, RegAddr);
  if (BitMask != 0 && BitMask != 0xFFFFFFFFu) {
    val = (val & BitMask) >> PHY_CalculateBitShift(BitMask);
  }
  return val;
}

uint32_t RadioManagementModule::phy_RFSerialRead(RfPath eRFPath,
                                                 uint32_t Offset) {
  uint32_t retValue;

  BbRegisterDefinition pPhyReg = PhyRegDef[eRFPath];
  bool bIsPIMode = false;

  /* <20120809, Kordan> CCA OFF(when entering), asked by James to avoid reading
   * the wrong value. */
  /* <20120828, Kordan> Toggling CCA would affect RF 0x0, skip it! */
  if (Offset != 0x0 && !(IS_C_CUT(_eepromManager->version_id))) {
    _device.phy_set_bb_reg(rCCAonSec_Jaguar, 0x8, 1);
  }

  Offset &= 0xff;

  if (eRFPath == RfPath::RF_PATH_A) {
    bIsPIMode = phy_query_bb_reg(0xC00, 0x4) != 0;
  } else if (eRFPath == RfPath::RF_PATH_B) {
    bIsPIMode = phy_query_bb_reg(0xE00, 0x4) != 0;
  }

  _device.phy_set_bb_reg((ushort)pPhyReg.RfHSSIPara2, bHSSIRead_addr_Jaguar,
                         Offset);

  if (IS_C_CUT(_eepromManager->version_id)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  if (bIsPIMode) {
    retValue = phy_query_bb_reg(pPhyReg.RfLSSIReadBackPi, rRead_data_Jaguar);
    /* RTW_INFO("[PI mode] RFR-%d Addr[0x%x]=0x%x\n", eRFPath,
     * pPhyReg.rfLSSIReadBackPi, retValue); */
  } else {
    retValue = phy_query_bb_reg(pPhyReg.RfLSSIReadBack, rRead_data_Jaguar);
    /* RTW_INFO("[SI mode] RFR-%d Addr[0x%x]=0x%x\n", eRFPath,
     * pPhyReg.RfLSSIReadBack, retValue); */
  }

  /* <20120809, Kordan> CCA ON(when exiting), asked by James to avoid reading
   * the wrong value. */
  /* <20120828, Kordan> Toggling CCA would affect RF 0x0, skip it! */
  if (Offset != 0x0 && !(IS_C_CUT(_eepromManager->version_id))) {
    _device.phy_set_bb_reg(rCCAonSec_Jaguar, 0x8, 0);
  }

  return retValue;
}

void RadioManagementModule::phy_RFSerialWrite(RfPath eRFPath, uint32_t Offset,
                                              uint32_t Data) {
  BbRegisterDefinition pPhyReg = PhyRegDef[eRFPath];

  Offset &= 0xff;
  /* Shadow Update */
  /* PHY_RFShadowWrite(adapterState, eRFPath, Offset, Data); */
  /* Put write addr in [27:20]  and write data in [19:00] */
  auto dataAndAddr = ((Offset << 20) | (Data & 0x000fffff)) & 0x0fffffff;

  /* Write Operation */
  /* TODO: Dynamically determine whether using PI or SI to write RF registers.
   */
  _device.phy_set_bb_reg((ushort)pPhyReg.Rf3WireOffset, bMaskDWord,
                         dataAndAddr);
  /* RTW_INFO("RFW-%d Addr[0x%x]=0x%x\n", eRFPath, pPhyReg.Rf3WireOffset,
   * DataAndAddr); */
}

void RadioManagementModule::PHY_SwitchWirelessBand8812(BandType Band) {
  ChannelWidth_t current_bw = _currentChannelBw;
  bool eLNA_2g = _eepromManager->ExternalLNA_2G;
  const bool is_8821 =
      _eepromManager->version_id.ICType == CHIP_8821;

  _logger->info("[{}] {}", __func__, Band == BandType::BAND_ON_2_4G ? "2.4G" : "5G");

  current_band_type = Band;

  if (Band == BandType::BAND_ON_2_4G) {
    /* 2.4G band */

    _device.phy_set_bb_reg(rOFDMCCKEN_Jaguar, bOFDMEN_Jaguar | bCCKEN_Jaguar,
                           0x03);

    if (is_8821) {
      phy_SetRFEReg8821(Band);
    } else {
      /* <20131128, VincentL> Remove 0x830[3:1] setting when switching 2G/5G,
       * requested by Yn. */
      _device.phy_set_bb_reg(rBWIndication_Jaguar, 0x3,
                             0x1); /* 0x834[1:0] = 0x1 */
      /* set PD_TH_20M for BB Yn user guide R27 */
      _device.phy_set_bb_reg(rPwed_TH_Jaguar,
                             BIT13 | BIT14 | BIT15 | BIT16 | BIT17,
                             0x17); /* 0x830[17:13]=5'b10111 */

      /* set PWED_TH for BB Yn user guide R29 */
      if (current_bw == ChannelWidth_t::CHANNEL_WIDTH_20 &&
          _eepromManager->version_id.RFType == RF_TYPE_1T1R &&
          eLNA_2g == false) {
        /* 0x830[3:1]=3'b010 */
        _device.phy_set_bb_reg(rPwed_TH_Jaguar, BIT1 | BIT2 | BIT3, 0x02);
      } else {
        /* 0x830[3:1]=3'b100 */
        _device.phy_set_bb_reg(rPwed_TH_Jaguar, BIT1 | BIT2 | BIT3, 0x04);
      }
    }

    /* AGC table select. 8821AU uses the path-A TX scale knob at 0xC1C[11:8];
     * 8812/8814 use the AGC table select bit at 0x82C[1:0]. */
    if (is_8821 && IS_NORMAL_CHIP(_eepromManager->version_id)) {
      _device.phy_set_bb_reg(rA_TxScale_Jaguar, 0xF00, 0);
    } else {
      _device.phy_set_bb_reg(rAGC_table_Jaguar, 0x3, 0);
    }

    if (!is_8821) {
      phy_SetRFEReg8812(Band);
    }

    /* <20131106, Kordan> Workaround to fix CCK FA for scan issue. */
    /* if( pHalData.bMPMode == FALSE) */

    _device.phy_set_bb_reg(rTxPath_Jaguar, 0xf0, 0x1);
    _device.phy_set_bb_reg(rCCK_RX_Jaguar, 0x0f000000, 0x1);

    /* CCK_CHECK_en */
    _device.rtw_write8(
        REG_CCK_CHECK_8812,
        (uint8_t)(_device.rtw_read8(REG_CCK_CHECK_8812) & (~BIT7)));
  } else {
    /* 5G band */

    /* CCK_CHECK_en */
    _device.rtw_write8(REG_CCK_CHECK_8812,
                       (uint8_t)(_device.rtw_read8(REG_CCK_CHECK_8812) | BIT7));

    uint16_t count = 0;
    uint16_t reg41A = _device.rtw_read16(REG_TXPKT_EMPTY);
    reg41A &= 0x30;
    while ((reg41A != 0x30) && (count < 50)) {
      using namespace std::chrono_literals;
      std::this_thread::sleep_for(50ms);
      reg41A = _device.rtw_read16(REG_TXPKT_EMPTY);
      reg41A &= 0x30;
      count++;
    }

    if (count != 0) {
      _logger->info("PHY_SwitchWirelessBand8812(): Switch to 5G Band. Count = "
                    "{:4X} reg41A={:4X}",
                    count, reg41A);
    }

    /* 2012/02/01, Sinda add registry to switch workaround without long-run
     * verification for scan issue. */
    _device.phy_set_bb_reg(rOFDMCCKEN_Jaguar, bOFDMEN_Jaguar | bCCKEN_Jaguar,
                           0x03);

    if (is_8821) {
      phy_SetRFEReg8821(Band);
    } else {
      /* <20131128, VincentL> Remove 0x830[3:1] setting when switching 2G/5G,
       * requested by Yn. */
      _device.phy_set_bb_reg(rBWIndication_Jaguar, 0x3,
                             0x2); /* 0x834[1:0] = 0x2 */
      /* set PD_TH_20M for BB Yn user guide R27 */
      _device.phy_set_bb_reg(rPwed_TH_Jaguar,
                             BIT13 | BIT14 | BIT15 | BIT16 | BIT17,
                             0x15); /* 0x830[17:13]=5'b10101 */

      /* set PWED_TH for BB Yn user guide R29 */
      /* 0x830[3:1]=3'b100 */
      _device.phy_set_bb_reg(rPwed_TH_Jaguar, BIT1 | BIT2 | BIT3, 0x04);
    }

    /* AGC table select (same family-split as 2.4G branch). */
    if (is_8821 && IS_NORMAL_CHIP(_eepromManager->version_id)) {
      _device.phy_set_bb_reg(rA_TxScale_Jaguar, 0xF00, 1);
    } else {
      _device.phy_set_bb_reg(rAGC_table_Jaguar, 0x3, 1);
    }

    if (!is_8821) {
      phy_SetRFEReg8812(Band);
    }

    /* <20131106, Kordan> Workaround to fix CCK FA for scan issue. */
    /* if( pHalData.bMPMode == FALSE) */
    _device.phy_set_bb_reg(rTxPath_Jaguar, 0xf0, 0x0);
    _device.phy_set_bb_reg(rCCK_RX_Jaguar, 0x0f000000, 0xF);
  }

  phy_SetBBSwingByBand_8812A(Band);
}

/* 8821AU has a single RFE pinmux register set (path A only). The 2.4G/5G
 * paths split on EFUSE ExternalLNA_2G — when an external LNA is present the
 * pinmux is configured for the EXT_LNA cell of the RFE control plane (mux
 * values 0x2 with INV[20]=1). Ported from svpcom/rtl8812au v5.2.20 via
 * PR#22. */
void RadioManagementModule::phy_SetRFEReg8821(BandType Band) {
  if (Band == BandType::BAND_ON_2_4G) {
    _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, 0xF000, 0x7);
    _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, 0xF0, 0x7);

    if (_eepromManager->ExternalLNA_2G) {
      _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, BIT20, 1);
      _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, BIT22, 0);
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, BIT2 | BIT1 | BIT0, 0x2);
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, BIT10 | BIT9 | BIT8, 0x2);
    } else {
      _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, BIT20, 0);
      _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, BIT22, 0);
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, BIT2 | BIT1 | BIT0, 0x7);
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, BIT10 | BIT9 | BIT8, 0x7);
    }
  } else {
    _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, 0xF000, 0x5);
    _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, 0xF0, 0x4);
    _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, BIT20, 0);
    _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, BIT22, 0);
    _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, BIT2 | BIT1 | BIT0, 0x7);
    _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, BIT10 | BIT9 | BIT8, 0x7);
  }
}

void RadioManagementModule::phy_SetRFEReg8812(BandType Band) {
  uint32_t u1tmp = 0;

  if (Band == BandType::BAND_ON_2_4G) {
    switch (_eepromManager->rfe_type) {
    case 0:
    case 2:
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x77777777);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x77777777);
      _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x000);
      _device.phy_set_bb_reg(rB_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x000);
      break;
    case 1: {
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x77777777);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x77777777);
      _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x000);
      _device.phy_set_bb_reg(rB_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x000);
    } break;
    case 3:
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x54337770);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x54337770);
      _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x010);
      _device.phy_set_bb_reg(rB_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x010);
      _device.phy_set_bb_reg(r_ANTSEL_SW_Jaguar, 0x00000303, 0x1);
      break;
    case 4:
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x77777777);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x77777777);
      _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x001);
      _device.phy_set_bb_reg(rB_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x001);
      break;
    case 5:
      _device.rtw_write8(rA_RFE_Pinmux_Jaguar + 2, 0x77);

      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x77777777);
      u1tmp = _device.rtw_read8(rA_RFE_Inv_Jaguar + 3);
      u1tmp &= ~BIT0;
      _device.rtw_write8(rA_RFE_Inv_Jaguar + 3, (uint8_t)(u1tmp));
      _device.phy_set_bb_reg(rB_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x000);
      break;
    case 6:
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x07772770);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x07772770);
      _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, bMaskDWord, 0x00000077);
      _device.phy_set_bb_reg(rB_RFE_Inv_Jaguar, bMaskDWord, 0x00000077);
      break;
    default:
      break;
    }
  } else {
    switch (_eepromManager->rfe_type) {
    case 0:
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x77337717);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x77337717);
      _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x010);
      _device.phy_set_bb_reg(rB_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x010);
      break;
    case 1: {
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x77337717);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x77337717);
      _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x000);
      _device.phy_set_bb_reg(rB_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x000);
    } break;
    case 2:
    case 4:
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x77337777);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x77337777);
      _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x010);
      _device.phy_set_bb_reg(rB_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x010);
      break;
    case 3:
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x54337717);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x54337717);
      _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x010);
      _device.phy_set_bb_reg(rB_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x010);
      _device.phy_set_bb_reg(r_ANTSEL_SW_Jaguar, 0x00000303, 0x1);
      break;
    case 5:
      _device.rtw_write8(rA_RFE_Pinmux_Jaguar + 2, 0x33);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x77337777);
      u1tmp = _device.rtw_read8(rA_RFE_Inv_Jaguar + 3);
      _device.rtw_write8(rA_RFE_Inv_Jaguar + 3, (uint8_t)(u1tmp |= BIT0));
      _device.phy_set_bb_reg(rB_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x010);
      break;
    case 6:
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x07737717);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x07737717);
      _device.phy_set_bb_reg(rA_RFE_Inv_Jaguar, bMaskDWord, 0x00000077);
      _device.phy_set_bb_reg(rB_RFE_Inv_Jaguar, bMaskDWord, 0x00000077);
      break;
    default:
      break;
    }
  }
}

void RadioManagementModule::phy_SetBBSwingByBand_8812A(BandType Band) {
  _device.phy_set_bb_reg(
      rA_TxScale_Jaguar, 0xFFE00000,
      phy_get_tx_bb_swing_8812a(Band, RfPath::RF_PATH_A)); /* 0xC1C[31:21] */
  _device.phy_set_bb_reg(
      rB_TxScale_Jaguar, 0xFFE00000,
      phy_get_tx_bb_swing_8812a(Band, RfPath::RF_PATH_B)); /* 0xE1C[31:21] */
}

#define EEPROM_TX_BBSWING_2G_8812 0xC6
#define EEPROM_TX_BBSWING_5G_8812 0xC7

uint32_t RadioManagementModule::phy_get_tx_bb_swing_8812a(BandType Band,
                                                          RfPath RFPath) {
  int8_t bbSwing_2G = (int8_t)(-1 * registry_priv::TxBBSwing_2G);
  int8_t bbSwing_5G = (int8_t)(-1 * registry_priv::TxBBSwing_5G);
  uint32_t _out = 0x200;
  const int8_t AUTO = -1;

  if (_device.AutoloadFailFlag) {
    if (Band == BandType::BAND_ON_2_4G) {
      if (bbSwing_2G == 0)
        _out = 0x200; /* 0 dB */
      else if (bbSwing_2G == -3)
        _out = 0x16A; /* -3 dB */
      else if (bbSwing_2G == -6)
        _out = 0x101; /* -6 dB */
      else if (bbSwing_2G == -9)
        _out = 0x0B6; /* -9 dB */
      else {
        if (_eepromManager->ExternalPA_2G) {
          _out = 0x16A;
        } else {
          _out = 0x200;
        }
      }
    } else if (Band == BandType::BAND_ON_5G) {
      if (bbSwing_5G == 0)
        _out = 0x200; /* 0 dB */

      else if (bbSwing_5G == -3)
        _out = 0x16A; /* -3 dB */

      else if (bbSwing_5G == -6)
        _out = 0x101; /* -6 dB */

      else if (bbSwing_5G == -9)
        _out = 0x0B6; /* -9 dB */

      else {
        _out = 0x200;
      }
    } else {
      _out = 0x16A; /* -3 dB */
    }
  } else {
    uint8_t swing = 0;
    int8_t onePathSwing = 0;

    if (Band == BandType::BAND_ON_2_4G) {
      if (registry_priv::TxBBSwing_2G == AUTO) {
        _eepromManager->efuse_ShadowRead1Byte(EEPROM_TX_BBSWING_2G_8812,
                                              &swing);
        swing = (swing == 0xFF) ? (uint8_t)0x00 : swing;
      } else if (bbSwing_2G == 0)
        swing = 0x00; /* 0 dB */
      else if (bbSwing_2G == -3)
        swing = 0x05; /* -3 dB */
      else if (bbSwing_2G == -6)
        swing = 0x0A; /* -6 dB */
      else if (bbSwing_2G == -9)
        swing = 0xFF; /* -9 dB */
      else
        swing = 0x00;
    } else {
      if (registry_priv::TxBBSwing_5G == AUTO) {
        _eepromManager->efuse_ShadowRead1Byte(EEPROM_TX_BBSWING_5G_8812,
                                              &swing);
        swing = (swing == 0xFF) ? (uint8_t)0x00 : swing;
      } else if (bbSwing_5G == 0)
        swing = 0x00; /* 0 dB */
      else if (bbSwing_5G == -3)
        swing = 0x05; /* -3 dB */
      else if (bbSwing_5G == -6)
        swing = 0x0A; /* -6 dB */
      else if (bbSwing_5G == -9)
        swing = 0xFF; /* -9 dB */
      else
        swing = 0x00;
    }

    if (RFPath == RfPath::RF_PATH_A) {
      onePathSwing = (uint8_t)((swing & 0x3) >> 0); /* 0xC6/C7[1:0] */
    } else if (RFPath == RfPath::RF_PATH_B) {
      onePathSwing = (uint8_t)((swing & 0xC) >> 2); /* 0xC6/C7[3:2] */
    }

    if (onePathSwing == 0x0) {
      _out = 0x200; /* 0 dB */
    } else if (onePathSwing == 0x1) {
      _out = 0x16A; /* -3 dB */
    } else if (onePathSwing == 0x2) {
      _out = 0x101; /* -6 dB */
    } else if (onePathSwing == 0x3) {
      _out = 0x0B6; /* -9 dB */
    }
  }

  /* RTW_INFO("<=== phy_get_tx_bb_swing_8812a, out = 0x%X\n", out); */

  return _out;
}

void RadioManagementModule::init_hw_mlme_ext(SelectedChannel pmlmeext) {
  /* Modify to make sure first time change channel(band) would be done properly
   */
  _currentChannel = 0;
  _currentChannelBw = ChannelWidth_t::CHANNEL_WIDTH_MAX;
  current_band_type = BandType::BAND_MAX;

  /* set_opmode_cmd(padapter, infra_client_with_mlme); */ /* removed */
  Set_HW_VAR_ENABLE_RX_BAR(true);
  set_channel_bwmode(pmlmeext.Channel, pmlmeext.ChannelOffset,
                     pmlmeext.ChannelWidth);
}

void RadioManagementModule::Set_HW_VAR_ENABLE_RX_BAR(bool val) {
  if (val) {
    /* enable RX BAR */
    uint32_t val16 = _device.rtw_read16(REG_RXFLTMAP1);

    val16 |= BIT8;
    _device.rtw_write16(REG_RXFLTMAP1, (uint16_t)val16);
  } else {
    /* disable RX BAR */
    uint32_t val16 = _device.rtw_read16(REG_RXFLTMAP1);

    val16 &= ~BIT8;
    _device.rtw_write16(REG_RXFLTMAP1, (uint16_t)val16);
  }

  _logger->info("[HW_VAR_ENABLE_RX_BAR] 0x{:4X}=0x{:4X}", REG_RXFLTMAP1,
                _device.rtw_read16(REG_RXFLTMAP1));
}

void RadioManagementModule::phy_SwChnl8812() {
  u8 channelToSW = _currentChannel;

  if (phy_SwBand8812(channelToSW) == false) {
    _logger->error("error Chnl {} !", channelToSW);
  }

  /* RTW_INFO("[BW:CHNL], phy_SwChnl8812(), switch to channel %d !!\n",
   * channelToSW); */

  /* fc_area		 */
  if (36 <= channelToSW && channelToSW <= 48) {
    _device.phy_set_bb_reg(rFc_area_Jaguar, 0x1ffe0000, 0x494);
  } else if (15 <= channelToSW && channelToSW <= 35) {
    _device.phy_set_bb_reg(rFc_area_Jaguar, 0x1ffe0000, 0x494);
  } else if (50 <= channelToSW && channelToSW <= 80) {
    _device.phy_set_bb_reg(rFc_area_Jaguar, 0x1ffe0000, 0x453);
  } else if (82 <= channelToSW && channelToSW <= 116) {
    _device.phy_set_bb_reg(rFc_area_Jaguar, 0x1ffe0000, 0x452);
  } else if (118 <= channelToSW) {
    _device.phy_set_bb_reg(rFc_area_Jaguar, 0x1ffe0000, 0x412);
  } else {
    _device.phy_set_bb_reg(rFc_area_Jaguar, 0x1ffe0000, 0x96a);
  }

  for (uint8_t eRFPath = 0; (uint8_t)eRFPath < _eepromManager->numTotalRfPath;
       eRFPath++) {
    /* RF_MOD_AG */
    if (36 <= channelToSW && channelToSW <= 80) {
      phy_set_rf_reg((RfPath)eRFPath, RF_CHNLBW_Jaguar,
                     BIT18 | BIT17 | BIT16 | BIT9 | BIT8,
                     0x101); /* 5'b00101); */
    } else if (15 <= channelToSW && channelToSW <= 35) {
      phy_set_rf_reg((RfPath)eRFPath, RF_CHNLBW_Jaguar,
                     BIT18 | BIT17 | BIT16 | BIT9 | BIT8,
                     0x101); /* 5'b00101); */
    } else if (82 <= channelToSW && channelToSW <= 140) {
      phy_set_rf_reg((RfPath)eRFPath, RF_CHNLBW_Jaguar,
                     BIT18 | BIT17 | BIT16 | BIT9 | BIT8,
                     0x301); /* 5'b01101); */
    } else if (140 < channelToSW) {
      phy_set_rf_reg((RfPath)eRFPath, RF_CHNLBW_Jaguar,
                     BIT18 | BIT17 | BIT16 | BIT9 | BIT8,
                     0x501); /* 5'b10101); */
    } else {
      phy_set_rf_reg((RfPath)eRFPath, RF_CHNLBW_Jaguar,
                     BIT18 | BIT17 | BIT16 | BIT9 | BIT8,
                     0x000); /* 5'b00000); */
    }

    /* <20121109, Kordan> A workaround for 8812A only. */
    phy_FixSpur_8812A(_currentChannelBw, channelToSW);
    phy_set_rf_reg((RfPath)eRFPath, RF_CHNLBW_Jaguar, bMaskByte0, channelToSW);
  }
}

bool RadioManagementModule::phy_SwBand8812(uint8_t channelToSW) {
  uint8_t u1Btmp;
  bool ret_value = true;
  BandType Band;
  BandType BandToSW;

  u1Btmp = _device.rtw_read8(REG_CCK_CHECK_8812);
  if ((u1Btmp & BIT7) != 0) {
    Band = BandType::BAND_ON_5G;
  } else {
    Band = BandType::BAND_ON_2_4G;
  }

  /* Use current channel to judge Band Type and switch Band if need. */
  if (channelToSW > 14) {
    BandToSW = BandType::BAND_ON_5G;
  } else {
    BandToSW = BandType::BAND_ON_2_4G;
  }

  if (BandToSW != Band) {
    PHY_SwitchWirelessBand8812(BandToSW);
  }

  return ret_value;
}

void RadioManagementModule::phy_FixSpur_8812A(ChannelWidth_t Bandwidth,
                                              uint8_t Channel) {
  /* C cut Item12 ADC FIFO CLOCK */
  if (IS_C_CUT(_eepromManager->version_id)) {
    if (Bandwidth == CHANNEL_WIDTH_40 && Channel == 11) {
      _device.phy_set_bb_reg(rRFMOD_Jaguar, 0xC00,
                             0x3); /* 0x8AC[11:10] = 2'b11 */
    } else {
      _device.phy_set_bb_reg(rRFMOD_Jaguar, 0xC00,
                             0x2); /* 0x8AC[11:10] = 2'b10 */
    }

    /* <20120914, Kordan> A workarould to resolve 2480Mhz spur by setting ADC
     * clock as 160M. (Asked by Binson) */
    if (Bandwidth == CHANNEL_WIDTH_20 && (Channel == 13 || Channel == 14)) {

      _device.phy_set_bb_reg(rRFMOD_Jaguar, 0x300,
                             0x3); /* 0x8AC[9:8] = 2'b11 */
      _device.phy_set_bb_reg(rADC_Buf_Clk_Jaguar, BIT30, 1); /* 0x8C4[30] = 1 */

    } else if (Bandwidth == CHANNEL_WIDTH_40 && Channel == 11) {
      _device.phy_set_bb_reg(rADC_Buf_Clk_Jaguar, BIT30, 1); /* 0x8C4[30] = 1 */
    } else if (Bandwidth != CHANNEL_WIDTH_80) {
      _device.phy_set_bb_reg(rRFMOD_Jaguar, 0x300,
                             0x2); /* 0x8AC[9:8] = 2'b10	 */
      _device.phy_set_bb_reg(rADC_Buf_Clk_Jaguar, BIT30, 0); /* 0x8C4[30] = 0 */
    }
  } else {
    /* <20120914, Kordan> A workarould to resolve 2480Mhz spur by setting ADC
     * clock as 160M. (Asked by Binson) */
    if (Bandwidth == CHANNEL_WIDTH_20 && (Channel == 13 || Channel == 14)) {
      _device.phy_set_bb_reg(rRFMOD_Jaguar, 0x300, 0x3); /* 0x8AC[9:8] = 11 */
    } else if (Channel <= 14)                            /* 2.4G only */
    {
      _device.phy_set_bb_reg(rRFMOD_Jaguar, 0x300, 0x2); /* 0x8AC[9:8] = 10 */
    }
  }
}

enum VHT_DATA_SC : uint8_t {
  UNDEFINED = 0,
  VHT_DATA_SC_20_UPPER_OF_80MHZ = 1,
  VHT_DATA_SC_20_LOWER_OF_80MHZ = 2,
  VHT_DATA_SC_20_UPPERST_OF_80MHZ = 3,
  VHT_DATA_SC_20_LOWEST_OF_80MHZ = 4,
  VHT_DATA_SC_40_UPPER_OF_80MHZ = 9,
  VHT_DATA_SC_40_LOWER_OF_80MHZ = 10,
};

void RadioManagementModule::phy_PostSetBwMode8812() {
  uint8_t L1pkVal = 0, reg_837 = 0;

  /* 3 Set Reg668 BW */
  phy_SetRegBW_8812(_currentChannelBw);

  /* 3 Set Reg483 */
  auto SubChnlNum = phy_GetSecondaryChnl_8812();
  _device.rtw_write8(REG_DATA_SC_8812, SubChnlNum);

  reg_837 = _device.rtw_read8(rBWIndication_Jaguar + 3);
  /* 3 Set Reg848 Reg864 Reg8AC Reg8C4 RegA00 */
  switch (_currentChannelBw) {
  case CHANNEL_WIDTH_20:
    _device.phy_set_bb_reg(rRFMOD_Jaguar, 0x003003C3,
                           0x00300200); /* 0x8ac[21,20,9:6,1,0]=8'b11100000 */
    _device.phy_set_bb_reg(rADC_Buf_Clk_Jaguar, BIT30,
                           0); /* 0x8c4[30] = 1'b0 */

    if (_eepromManager->numTotalRfPath >= 2) {
      _device.phy_set_bb_reg(rL1PeakTH_Jaguar, 0x03C00000,
                             7); /* multi-path 0x848[25:22] = 0x7 */
    } else {
      _device.phy_set_bb_reg(rL1PeakTH_Jaguar, 0x03C00000,
                             8); /* 1R 0x848[25:22] = 0x8 */
    }

    break;

  case CHANNEL_WIDTH_40:
    _device.phy_set_bb_reg(
        rRFMOD_Jaguar, 0x003003C3,
        0x00300201); /* 0x8ac[21,20,9:6,1,0]=8'b11100000		 */
    _device.phy_set_bb_reg(rADC_Buf_Clk_Jaguar, BIT30,
                           0); /* 0x8c4[30] = 1'b0 */
    _device.phy_set_bb_reg(rRFMOD_Jaguar, 0x3C, SubChnlNum);
    _device.phy_set_bb_reg(rCCAonSec_Jaguar, 0xf0000000, SubChnlNum);

    if ((reg_837 & BIT2) != 0)
      L1pkVal = 6;
    else {
      if (_eepromManager->numTotalRfPath >= 2) {
        L1pkVal = 7;
      } else {
        L1pkVal = 8;
      }
    }

    _device.phy_set_bb_reg(rL1PeakTH_Jaguar, 0x03C00000,
                           L1pkVal); /* 0x848[25:22] = 0x6 */

    if (SubChnlNum == VHT_DATA_SC::VHT_DATA_SC_20_UPPER_OF_80MHZ) {
      _device.phy_set_bb_reg(rCCK_System_Jaguar, bCCK_System_Jaguar, 1);
    } else {
      _device.phy_set_bb_reg(rCCK_System_Jaguar, bCCK_System_Jaguar, 0);
    }

    break;

  case CHANNEL_WIDTH_80:
    _device.phy_set_bb_reg(rRFMOD_Jaguar, 0x003003C3,
                           0x00300202); /* 0x8ac[21,20,9:6,1,0]=8'b11100010 */
    _device.phy_set_bb_reg(rADC_Buf_Clk_Jaguar, BIT30, 1); /* 0x8c4[30] = 1 */
    _device.phy_set_bb_reg(rRFMOD_Jaguar, 0x3C, SubChnlNum);
    _device.phy_set_bb_reg(rCCAonSec_Jaguar, 0xf0000000, SubChnlNum);

    if ((reg_837 & BIT2) != 0)
      L1pkVal = 5;
    else {
      if (_eepromManager->numTotalRfPath >= 2) {
        L1pkVal = 6;
      } else {
        L1pkVal = 7;
      }
    }

    _device.phy_set_bb_reg(rL1PeakTH_Jaguar, 0x03C00000,
                           L1pkVal); /* 0x848[25:22] = 0x5 */

    break;

  default:
    _logger->error("phy_PostSetBWMode8812():	unknown Bandwidth: {}",
                   (int)_currentChannelBw);
    break;
  }

  /* <20121109, Kordan> A workaround for 8812A only. */
  phy_FixSpur_8812A(_currentChannelBw, _currentChannel);

  /* RTW_INFO("phy_PostSetBwMode8812(): Reg483: %x\n", rtw_read8(adapterState,
   * 0x483)); */
  /* RTW_INFO("phy_PostSetBwMode8812(): Reg668: %x\n", rtw_read32(adapterState,
   * 0x668)); */
  /* RTW_INFO("phy_PostSetBwMode8812(): Reg8AC: %x\n",
   * phy_query_bb_reg(adapterState, rRFMOD_Jaguar, 0xffffffff)); */

  /* 3 Set RF related register */
  PHY_RF6052SetBandwidth8812(_currentChannelBw);
}

void RadioManagementModule::phy_SetRegBW_8812(ChannelWidth_t CurrentBW) {
  uint16_t RegRfMod_BW, u2tmp;
  RegRfMod_BW = _device.rtw_read16(REG_WMAC_TRXPTCL_CTL);

  switch (CurrentBW) {
  case CHANNEL_WIDTH_20:
    _device.rtw_write16(
        REG_WMAC_TRXPTCL_CTL,
        (ushort)(RegRfMod_BW & 0xFE7F)); /* BIT 7 = 0, BIT 8 = 0 */
    break;

  case CHANNEL_WIDTH_40:
    u2tmp = (ushort)(RegRfMod_BW | BIT7);
    _device.rtw_write16(REG_WMAC_TRXPTCL_CTL,
                        (ushort)(u2tmp & 0xFEFF)); /* BIT 7 = 1, BIT 8 = 0 */
    break;

  case CHANNEL_WIDTH_80:
    u2tmp = (ushort)(RegRfMod_BW | BIT8);
    _device.rtw_write16(REG_WMAC_TRXPTCL_CTL,
                        (ushort)(u2tmp & 0xFF7F)); /* BIT 7 = 0, BIT 8 = 1 */
    break;

  default:
    _logger->error("phy_PostSetBWMode8812():	unknown Bandwidth: {}",
                   (int)CurrentBW);
    break;
  }
}

void RadioManagementModule::PHY_RF6052SetBandwidth8812(
    ChannelWidth_t Bandwidth) /* 20M or 40M */
{
  /* RF_CHNLBW_Jaguar[11:10] encodes the per-path channel bandwidth:
   *   0b11 = 20 MHz, 0b01 = 40 MHz, 0b00 = 80 MHz.
   * Apply to every populated RF path (4 paths on 8814AU, 2 on 8812AU). */
  uint32_t bw_bits;
  switch (Bandwidth) {
  case CHANNEL_WIDTH_20:
    bw_bits = 3;
    break;
  case CHANNEL_WIDTH_40:
    bw_bits = 1;
    break;
  case CHANNEL_WIDTH_80:
    bw_bits = 0;
    break;
  default:
    _logger->error("PHY_RF6052SetBandwidth8812(): unknown Bandwidth: {}",
                   (int)Bandwidth);
    return;
  }

  for (uint8_t p = 0; p < _eepromManager->numTotalRfPath; ++p) {
    phy_set_rf_reg(static_cast<RfPath>(p), RF_CHNLBW_Jaguar, BIT11 | BIT10,
                   bw_bits);
  }
}

uint8_t RadioManagementModule::phy_GetSecondaryChnl_8812() {
  VHT_DATA_SC SCSettingOf40 = UNDEFINED, SCSettingOf20 = UNDEFINED;

  /* RTW_INFO("SCMapping: Case: pHalData._currentChannelBw %d,
   * pHalData._cur80MhzPrimeSc %d, pHalData._cur40MhzPrimeSc
   * %d\n",pHalData._currentChannelBw,pHalData._cur80MhzPrimeSc,pHalData._cur40MhzPrimeSc);
   */
  if (_currentChannelBw == CHANNEL_WIDTH_80) {
    if (_cur80MhzPrimeSc == HAL_PRIME_CHNL_OFFSET_LOWER) {
      SCSettingOf40 = VHT_DATA_SC::VHT_DATA_SC_40_LOWER_OF_80MHZ;
    } else if (_cur80MhzPrimeSc == HAL_PRIME_CHNL_OFFSET_UPPER) {
      SCSettingOf40 = VHT_DATA_SC::VHT_DATA_SC_40_UPPER_OF_80MHZ;
    } else {
      _logger->error("SCMapping: DONOT CARE Mode Setting");
    }

    if ((_cur40MhzPrimeSc == HAL_PRIME_CHNL_OFFSET_LOWER) &&
        (_cur80MhzPrimeSc == HAL_PRIME_CHNL_OFFSET_LOWER)) {
      SCSettingOf20 = VHT_DATA_SC::VHT_DATA_SC_20_LOWEST_OF_80MHZ;
    } else if ((_cur40MhzPrimeSc == HAL_PRIME_CHNL_OFFSET_UPPER) &&
               (_cur80MhzPrimeSc == HAL_PRIME_CHNL_OFFSET_LOWER)) {
      SCSettingOf20 = VHT_DATA_SC::VHT_DATA_SC_20_LOWER_OF_80MHZ;
    } else if ((_cur40MhzPrimeSc == HAL_PRIME_CHNL_OFFSET_LOWER) &&
               (_cur80MhzPrimeSc == HAL_PRIME_CHNL_OFFSET_UPPER)) {
      SCSettingOf20 = VHT_DATA_SC::VHT_DATA_SC_20_UPPER_OF_80MHZ;
    } else if ((_cur40MhzPrimeSc == HAL_PRIME_CHNL_OFFSET_UPPER) &&
               (_cur80MhzPrimeSc == HAL_PRIME_CHNL_OFFSET_UPPER)) {
      SCSettingOf20 = VHT_DATA_SC::VHT_DATA_SC_20_UPPERST_OF_80MHZ;
    } else {
      _logger->error("SCMapping: DONOT CARE Mode Setting");
    }
  } else if (_currentChannelBw == CHANNEL_WIDTH_40) {
    /* RTW_INFO("SCMapping: Case: pHalData._currentChannelBw %d,
     * pHalData._cur40MhzPrimeSc
     * %d\n",pHalData._currentChannelBw,pHalData._cur40MhzPrimeSc); */

    if (_cur40MhzPrimeSc == HAL_PRIME_CHNL_OFFSET_UPPER) {
      SCSettingOf20 = VHT_DATA_SC::VHT_DATA_SC_20_UPPER_OF_80MHZ;
    } else if (_cur40MhzPrimeSc == HAL_PRIME_CHNL_OFFSET_LOWER) {
      SCSettingOf20 = VHT_DATA_SC::VHT_DATA_SC_20_LOWER_OF_80MHZ;
    } else {
      _logger->error("SCMapping: DONOT CARE Mode Setting");
    }
  }

  /*RTW_INFO("SCMapping: SC Value %x\n", ((SCSettingOf40 << 4) |
   * SCSettingOf20));*/
  return (uint8_t)(((uint8_t)SCSettingOf40 << 4) | (uint8_t)SCSettingOf20);
}

void RadioManagementModule::PHY_SetTxPowerLevel8812(uint8_t Channel) {
  for (uint8_t path = 0; (uint8_t)path < _eepromManager->numTotalRfPath;
       path++) {
    phy_set_tx_power_level_by_path(Channel, (RfPath)path);
    PHY_TxPowerTrainingByPath_8812((RfPath)path);
  }
}

void RadioManagementModule::phy_set_tx_power_level_by_path(uint8_t channel,
                                                           RfPath path) {
  bool bIsIn24G = (current_band_type == BandType::BAND_ON_2_4G);

  if (bIsIn24G) {
    phy_set_tx_power_index_by_rate_section(path, channel, RATE_SECTION::CCK);
  }

  phy_set_tx_power_index_by_rate_section(path, channel, RATE_SECTION::OFDM);

  phy_set_tx_power_index_by_rate_section(path, channel,
                                         RATE_SECTION::HT_MCS0_MCS7);
  phy_set_tx_power_index_by_rate_section(path, channel,
                                         RATE_SECTION::VHT_1SSMCS0_1SSMCS9);

  if (_eepromManager->numTotalRfPath >= 2) {
    phy_set_tx_power_index_by_rate_section(path, channel,
                                           RATE_SECTION::HT_MCS8_MCS15);
    phy_set_tx_power_index_by_rate_section(path, channel,
                                           RATE_SECTION::VHT_2SSMCS0_2SSMCS9);
  }
  /* 8814A 3-stream rate sections — must be programmed so the chip's TXAGC
   * table is fully populated even though the USB-2 link can't sustain 3-SS
   * data rates. Upstream PHY_SetTxPowerLevel8814 iterates all sections. */
  if (_eepromManager->version_id.ICType == CHIP_8814A) {
    phy_set_tx_power_index_by_rate_section(path, channel,
                                           RATE_SECTION::HT_MCS16_MCS23);
    phy_set_tx_power_index_by_rate_section(path, channel,
                                           RATE_SECTION::VHT_3SSMCS0_3SSMCS9);
  }
}

const static std::vector<MGN_RATE> mgn_rates_cck = {
    MGN_RATE::MGN_1M, MGN_RATE::MGN_2M, MGN_RATE::MGN_5_5M, MGN_RATE::MGN_11M};

const static std::vector<MGN_RATE> mgn_rates_ofdm = {
    MGN_RATE::MGN_6M,  MGN_RATE::MGN_9M,  MGN_RATE::MGN_12M, MGN_RATE::MGN_18M,
    MGN_RATE::MGN_24M, MGN_RATE::MGN_36M, MGN_RATE::MGN_48M, MGN_RATE::MGN_54M};

const static std::vector<MGN_RATE> mgn_rates_mcs0_7 = {
    MGN_RATE::MGN_MCS0, MGN_RATE::MGN_MCS1, MGN_RATE::MGN_MCS2,
    MGN_RATE::MGN_MCS3, MGN_RATE::MGN_MCS4, MGN_RATE::MGN_MCS5,
    MGN_RATE::MGN_MCS6, MGN_RATE::MGN_MCS7};

const static std::vector<MGN_RATE> mgn_rates_mcs8_15 = {
    MGN_RATE::MGN_MCS8,  MGN_RATE::MGN_MCS9,  MGN_RATE::MGN_MCS10,
    MGN_RATE::MGN_MCS11, MGN_RATE::MGN_MCS12, MGN_RATE::MGN_MCS13,
    MGN_RATE::MGN_MCS14, MGN_RATE::MGN_MCS15};

const static std::vector<MGN_RATE> mgn_rates_mcs16_23 = {
    MGN_RATE::MGN_MCS16, MGN_RATE::MGN_MCS17, MGN_RATE::MGN_MCS18,
    MGN_RATE::MGN_MCS19, MGN_RATE::MGN_MCS20, MGN_RATE::MGN_MCS21,
    MGN_RATE::MGN_MCS22, MGN_RATE::MGN_MCS23};

const static std::vector<MGN_RATE> mgn_rates_mcs24_31 = {
    MGN_RATE::MGN_MCS24, MGN_RATE::MGN_MCS25, MGN_RATE::MGN_MCS26,
    MGN_RATE::MGN_MCS27, MGN_RATE::MGN_MCS28, MGN_RATE::MGN_MCS29,
    MGN_RATE::MGN_MCS30, MGN_RATE::MGN_MCS31};

const static std::vector<MGN_RATE> mgn_rates_vht1ss = {
    MGN_RATE::MGN_VHT1SS_MCS0, MGN_RATE::MGN_VHT1SS_MCS1,
    MGN_RATE::MGN_VHT1SS_MCS2, MGN_RATE::MGN_VHT1SS_MCS3,
    MGN_RATE::MGN_VHT1SS_MCS4, MGN_RATE::MGN_VHT1SS_MCS5,
    MGN_RATE::MGN_VHT1SS_MCS6, MGN_RATE::MGN_VHT1SS_MCS7,
    MGN_RATE::MGN_VHT1SS_MCS8, MGN_RATE::MGN_VHT1SS_MCS9};

const static std::vector<MGN_RATE> mgn_rates_vht2ss = {
    MGN_RATE::MGN_VHT2SS_MCS0, MGN_RATE::MGN_VHT2SS_MCS1,
    MGN_RATE::MGN_VHT2SS_MCS2, MGN_RATE::MGN_VHT2SS_MCS3,
    MGN_RATE::MGN_VHT2SS_MCS4, MGN_RATE::MGN_VHT2SS_MCS5,
    MGN_RATE::MGN_VHT2SS_MCS6, MGN_RATE::MGN_VHT2SS_MCS7,
    MGN_RATE::MGN_VHT2SS_MCS8, MGN_RATE::MGN_VHT2SS_MCS9};

const static std::vector<MGN_RATE> mgn_rates_vht3ss = {
    MGN_RATE::MGN_VHT3SS_MCS0, MGN_RATE::MGN_VHT3SS_MCS1,
    MGN_RATE::MGN_VHT3SS_MCS2, MGN_RATE::MGN_VHT3SS_MCS3,
    MGN_RATE::MGN_VHT3SS_MCS4, MGN_RATE::MGN_VHT3SS_MCS5,
    MGN_RATE::MGN_VHT3SS_MCS6, MGN_RATE::MGN_VHT3SS_MCS7,
    MGN_RATE::MGN_VHT3SS_MCS8, MGN_RATE::MGN_VHT3SS_MCS9};

const static std::vector<MGN_RATE> mgn_rates_vht4ss = {
    MGN_RATE::MGN_VHT4SS_MCS0, MGN_RATE::MGN_VHT4SS_MCS1,
    MGN_RATE::MGN_VHT4SS_MCS2, MGN_RATE::MGN_VHT4SS_MCS3,
    MGN_RATE::MGN_VHT4SS_MCS4, MGN_RATE::MGN_VHT4SS_MCS5,
    MGN_RATE::MGN_VHT4SS_MCS6, MGN_RATE::MGN_VHT4SS_MCS7,
    MGN_RATE::MGN_VHT4SS_MCS8, MGN_RATE::MGN_VHT4SS_MCS9};

const static std::vector<MGN_RATE> rates_by_sections[] = {
    mgn_rates_cck,      mgn_rates_ofdm,     mgn_rates_mcs0_7, mgn_rates_mcs8_15,
    mgn_rates_mcs16_23, mgn_rates_mcs24_31, mgn_rates_vht1ss, mgn_rates_vht2ss,
    mgn_rates_vht3ss,   mgn_rates_vht4ss,
};

void RadioManagementModule::SetTxPower(uint8_t p) {
  power = p;
  _logger->info("iwconfig wlan0 txpower {}", (int)p);
}

static uint8_t phy_get_tx_power_index() { return 16; }

void RadioManagementModule::PHY_SetTxPowerIndexByRateArray(
    RfPath rfPath, const std::vector<MGN_RATE> &rates) {
  for (int i = 0; i < rates.size(); ++i) {
    auto powerIndex = power;
    MGN_RATE rate = rates[i];
    PHY_SetTxPowerIndex_8812A(powerIndex, rfPath, rate);
  }
}

void RadioManagementModule::PHY_SetTxPowerIndex_8812A(uint32_t powerIndex,
                                                      RfPath rfPath,
                                                      MGN_RATE rate) {

  _logger->debug("PHY_SetTxPowerIndex {} {} {}", powerIndex, (int)rfPath, rate);

  /* 8814A: per-rate per-path power index is programmed via a single packed
   * BB-register write at 0x1998. Port of PHY_SetTxPowerIndex_8814A from
   * upstream hal/rtl8814a/rtl8814a_phycfg.c:743.
   *
   *   txagc_table_wd[31:24] = PowerIndex
   *   txagc_table_wd[15:8]  = RFPath
   *   txagc_table_wd[7:0]   = MRateToHwRate(Rate)
   *   txagc_table_wd        |= 0x00801000  (TXAGC table-write enable + addr)
   *
   * The 8812 per-rate fanout below uses register addresses (rTxAGC_A_CCK_*
   * etc.) that don't exist on 8814 — using it on 8814 scribbles random bits
   * and stalls the BB; that's what the earlier "skip TX power for monitor
   * mode" workaround was masking. */
  if (_eepromManager->version_id.ICType == CHIP_8814A) {
    uint32_t txagc_table_wd =
        0x00801000u |
        (static_cast<uint32_t>(rfPath) << 8) |
        static_cast<uint32_t>(MRateToHwRate(static_cast<uint8_t>(rate))) |
        (powerIndex << 24);
    _device.phy_set_bb_reg(0x1998, bMaskDWord, txagc_table_wd);
    if (rate == MGN_1M) {
      /* Upstream comment: "first time to turn on the txagc table". */
      _device.phy_set_bb_reg(0x1998, bMaskDWord, txagc_table_wd);
    }
    return;
  }

  /* The per-rate register table below only encodes paths A/B (8812-family).
   * 8814AU paths C/D use a different per-path register layout (the rTxAGC_C_
   * and rTxAGC_D_ symbol family in Hal8814PhyReg.h) — that's handled by the
   * CHIP_8814A branch above. */
  if (static_cast<uint8_t>(rfPath) >= RF_PATH_C) {
    return;
  }
  if (powerIndex % 2 == 1)
    powerIndex -= 1;
  if (rfPath == RF_PATH_A) {
    switch (rate) {
    case MGN_1M:
      _device.phy_set_bb_reg(rTxAGC_A_CCK11_CCK1_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_2M:
      _device.phy_set_bb_reg(rTxAGC_A_CCK11_CCK1_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_5_5M:
      _device.phy_set_bb_reg(rTxAGC_A_CCK11_CCK1_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_11M:
      _device.phy_set_bb_reg(rTxAGC_A_CCK11_CCK1_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_6M:
      _device.phy_set_bb_reg(rTxAGC_A_Ofdm18_Ofdm6_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_9M:
      _device.phy_set_bb_reg(rTxAGC_A_Ofdm18_Ofdm6_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_12M:
      _device.phy_set_bb_reg(rTxAGC_A_Ofdm18_Ofdm6_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_18M:
      _device.phy_set_bb_reg(rTxAGC_A_Ofdm18_Ofdm6_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_24M:
      _device.phy_set_bb_reg(rTxAGC_A_Ofdm54_Ofdm24_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_36M:
      _device.phy_set_bb_reg(rTxAGC_A_Ofdm54_Ofdm24_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_48M:
      _device.phy_set_bb_reg(rTxAGC_A_Ofdm54_Ofdm24_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_54M:
      _device.phy_set_bb_reg(rTxAGC_A_Ofdm54_Ofdm24_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_MCS0:
      _device.phy_set_bb_reg(rTxAGC_A_MCS3_MCS0_JAguar, bMaskByte0, powerIndex);
      break;
    case MGN_MCS1:
      _device.phy_set_bb_reg(rTxAGC_A_MCS3_MCS0_JAguar, bMaskByte1, powerIndex);
      break;
    case MGN_MCS2:
      _device.phy_set_bb_reg(rTxAGC_A_MCS3_MCS0_JAguar, bMaskByte2, powerIndex);
      break;
    case MGN_MCS3:
      _device.phy_set_bb_reg(rTxAGC_A_MCS3_MCS0_JAguar, bMaskByte3, powerIndex);
      break;

    case MGN_MCS4:
      _device.phy_set_bb_reg(rTxAGC_A_MCS7_MCS4_JAguar, bMaskByte0, powerIndex);
      break;
    case MGN_MCS5:
      _device.phy_set_bb_reg(rTxAGC_A_MCS7_MCS4_JAguar, bMaskByte1, powerIndex);
      break;
    case MGN_MCS6:
      _device.phy_set_bb_reg(rTxAGC_A_MCS7_MCS4_JAguar, bMaskByte2, powerIndex);
      break;
    case MGN_MCS7:
      _device.phy_set_bb_reg(rTxAGC_A_MCS7_MCS4_JAguar, bMaskByte3, powerIndex);
      break;

    case MGN_MCS8:
      _device.phy_set_bb_reg(rTxAGC_A_MCS11_MCS8_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_MCS9:
      _device.phy_set_bb_reg(rTxAGC_A_MCS11_MCS8_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_MCS10:
      _device.phy_set_bb_reg(rTxAGC_A_MCS11_MCS8_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_MCS11:
      _device.phy_set_bb_reg(rTxAGC_A_MCS11_MCS8_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_MCS12:
      _device.phy_set_bb_reg(rTxAGC_A_MCS15_MCS12_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_MCS13:
      _device.phy_set_bb_reg(rTxAGC_A_MCS15_MCS12_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_MCS14:
      _device.phy_set_bb_reg(rTxAGC_A_MCS15_MCS12_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_MCS15:
      _device.phy_set_bb_reg(rTxAGC_A_MCS15_MCS12_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_VHT1SS_MCS0:
      _device.phy_set_bb_reg(rTxAGC_A_Nss1Index3_Nss1Index0_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_VHT1SS_MCS1:
      _device.phy_set_bb_reg(rTxAGC_A_Nss1Index3_Nss1Index0_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_VHT1SS_MCS2:
      _device.phy_set_bb_reg(rTxAGC_A_Nss1Index3_Nss1Index0_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_VHT1SS_MCS3:
      _device.phy_set_bb_reg(rTxAGC_A_Nss1Index3_Nss1Index0_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_VHT1SS_MCS4:
      _device.phy_set_bb_reg(rTxAGC_A_Nss1Index7_Nss1Index4_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_VHT1SS_MCS5:
      _device.phy_set_bb_reg(rTxAGC_A_Nss1Index7_Nss1Index4_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_VHT1SS_MCS6:
      _device.phy_set_bb_reg(rTxAGC_A_Nss1Index7_Nss1Index4_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_VHT1SS_MCS7:
      _device.phy_set_bb_reg(rTxAGC_A_Nss1Index7_Nss1Index4_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_VHT1SS_MCS8:
      _device.phy_set_bb_reg(rTxAGC_A_Nss2Index1_Nss1Index8_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_VHT1SS_MCS9:
      _device.phy_set_bb_reg(rTxAGC_A_Nss2Index1_Nss1Index8_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS0:
      _device.phy_set_bb_reg(rTxAGC_A_Nss2Index1_Nss1Index8_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS1:
      _device.phy_set_bb_reg(rTxAGC_A_Nss2Index1_Nss1Index8_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_VHT2SS_MCS2:
      _device.phy_set_bb_reg(rTxAGC_A_Nss2Index5_Nss2Index2_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS3:
      _device.phy_set_bb_reg(rTxAGC_A_Nss2Index5_Nss2Index2_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS4:
      _device.phy_set_bb_reg(rTxAGC_A_Nss2Index5_Nss2Index2_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS5:
      _device.phy_set_bb_reg(rTxAGC_A_Nss2Index5_Nss2Index2_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_VHT2SS_MCS6:
      _device.phy_set_bb_reg(rTxAGC_A_Nss2Index9_Nss2Index6_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS7:
      _device.phy_set_bb_reg(rTxAGC_A_Nss2Index9_Nss2Index6_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS8:
      _device.phy_set_bb_reg(rTxAGC_A_Nss2Index9_Nss2Index6_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS9:
      _device.phy_set_bb_reg(rTxAGC_A_Nss2Index9_Nss2Index6_JAguar, bMaskByte3,
                             powerIndex);
      break;

    default:
      _logger->error("Invalid Rate!!\n");
      break;
    }
  } else if (rfPath == RF_PATH_B) {
    switch (rate) {
    case MGN_1M:
      _device.phy_set_bb_reg(rTxAGC_B_CCK11_CCK1_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_2M:
      _device.phy_set_bb_reg(rTxAGC_B_CCK11_CCK1_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_5_5M:
      _device.phy_set_bb_reg(rTxAGC_B_CCK11_CCK1_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_11M:
      _device.phy_set_bb_reg(rTxAGC_B_CCK11_CCK1_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_6M:
      _device.phy_set_bb_reg(rTxAGC_B_Ofdm18_Ofdm6_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_9M:
      _device.phy_set_bb_reg(rTxAGC_B_Ofdm18_Ofdm6_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_12M:
      _device.phy_set_bb_reg(rTxAGC_B_Ofdm18_Ofdm6_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_18M:
      _device.phy_set_bb_reg(rTxAGC_B_Ofdm18_Ofdm6_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_24M:
      _device.phy_set_bb_reg(rTxAGC_B_Ofdm54_Ofdm24_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_36M:
      _device.phy_set_bb_reg(rTxAGC_B_Ofdm54_Ofdm24_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_48M:
      _device.phy_set_bb_reg(rTxAGC_B_Ofdm54_Ofdm24_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_54M:
      _device.phy_set_bb_reg(rTxAGC_B_Ofdm54_Ofdm24_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_MCS0:
      _device.phy_set_bb_reg(rTxAGC_B_MCS3_MCS0_JAguar, bMaskByte0, powerIndex);
      break;
    case MGN_MCS1:
      _device.phy_set_bb_reg(rTxAGC_B_MCS3_MCS0_JAguar, bMaskByte1, powerIndex);
      break;
    case MGN_MCS2:
      _device.phy_set_bb_reg(rTxAGC_B_MCS3_MCS0_JAguar, bMaskByte2, powerIndex);
      break;
    case MGN_MCS3:
      _device.phy_set_bb_reg(rTxAGC_B_MCS3_MCS0_JAguar, bMaskByte3, powerIndex);
      break;

    case MGN_MCS4:
      _device.phy_set_bb_reg(rTxAGC_B_MCS7_MCS4_JAguar, bMaskByte0, powerIndex);
      break;
    case MGN_MCS5:
      _device.phy_set_bb_reg(rTxAGC_B_MCS7_MCS4_JAguar, bMaskByte1, powerIndex);
      break;
    case MGN_MCS6:
      _device.phy_set_bb_reg(rTxAGC_B_MCS7_MCS4_JAguar, bMaskByte2, powerIndex);
      break;
    case MGN_MCS7:
      _device.phy_set_bb_reg(rTxAGC_B_MCS7_MCS4_JAguar, bMaskByte3, powerIndex);
      break;

    case MGN_MCS8:
      _device.phy_set_bb_reg(rTxAGC_B_MCS11_MCS8_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_MCS9:
      _device.phy_set_bb_reg(rTxAGC_B_MCS11_MCS8_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_MCS10:
      _device.phy_set_bb_reg(rTxAGC_B_MCS11_MCS8_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_MCS11:
      _device.phy_set_bb_reg(rTxAGC_B_MCS11_MCS8_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_MCS12:
      _device.phy_set_bb_reg(rTxAGC_B_MCS15_MCS12_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_MCS13:
      _device.phy_set_bb_reg(rTxAGC_B_MCS15_MCS12_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_MCS14:
      _device.phy_set_bb_reg(rTxAGC_B_MCS15_MCS12_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_MCS15:
      _device.phy_set_bb_reg(rTxAGC_B_MCS15_MCS12_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_VHT1SS_MCS0:
      _device.phy_set_bb_reg(rTxAGC_B_Nss1Index3_Nss1Index0_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_VHT1SS_MCS1:
      _device.phy_set_bb_reg(rTxAGC_B_Nss1Index3_Nss1Index0_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_VHT1SS_MCS2:
      _device.phy_set_bb_reg(rTxAGC_B_Nss1Index3_Nss1Index0_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_VHT1SS_MCS3:
      _device.phy_set_bb_reg(rTxAGC_B_Nss1Index3_Nss1Index0_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_VHT1SS_MCS4:
      _device.phy_set_bb_reg(rTxAGC_B_Nss1Index7_Nss1Index4_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_VHT1SS_MCS5:
      _device.phy_set_bb_reg(rTxAGC_B_Nss1Index7_Nss1Index4_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_VHT1SS_MCS6:
      _device.phy_set_bb_reg(rTxAGC_B_Nss1Index7_Nss1Index4_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_VHT1SS_MCS7:
      _device.phy_set_bb_reg(rTxAGC_B_Nss1Index7_Nss1Index4_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_VHT1SS_MCS8:
      _device.phy_set_bb_reg(rTxAGC_B_Nss2Index1_Nss1Index8_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_VHT1SS_MCS9:
      _device.phy_set_bb_reg(rTxAGC_B_Nss2Index1_Nss1Index8_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS0:
      _device.phy_set_bb_reg(rTxAGC_B_Nss2Index1_Nss1Index8_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS1:
      _device.phy_set_bb_reg(rTxAGC_B_Nss2Index1_Nss1Index8_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_VHT2SS_MCS2:
      _device.phy_set_bb_reg(rTxAGC_B_Nss2Index5_Nss2Index2_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS3:
      _device.phy_set_bb_reg(rTxAGC_B_Nss2Index5_Nss2Index2_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS4:
      _device.phy_set_bb_reg(rTxAGC_B_Nss2Index5_Nss2Index2_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS5:
      _device.phy_set_bb_reg(rTxAGC_B_Nss2Index5_Nss2Index2_JAguar, bMaskByte3,
                             powerIndex);
      break;

    case MGN_VHT2SS_MCS6:
      _device.phy_set_bb_reg(rTxAGC_B_Nss2Index9_Nss2Index6_JAguar, bMaskByte0,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS7:
      _device.phy_set_bb_reg(rTxAGC_B_Nss2Index9_Nss2Index6_JAguar, bMaskByte1,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS8:
      _device.phy_set_bb_reg(rTxAGC_B_Nss2Index9_Nss2Index6_JAguar, bMaskByte2,
                             powerIndex);
      break;
    case MGN_VHT2SS_MCS9:
      _device.phy_set_bb_reg(rTxAGC_B_Nss2Index9_Nss2Index6_JAguar, bMaskByte3,
                             powerIndex);
      break;

    default:
      _logger->error("Invalid Rate!!\n");
      break;
    }
  } else
    _logger->error("Invalid RFPath!!\n");
#if 0
        if (PowerIndexDescription.SetTable.TryGetValue(rfPath, out var rfTable))
        {
            if (rfTable.TryGetValue(rate, out var values))
            {
                _device.phy_set_bb_reg(values.RegAddress, values.BitMask, powerIndex);
            }
            else
            {
                _logger.LogError("Invalid rate! RfPath: {RfPath} Rate:{Rate}", rfPath, rate);
            }
        }
        else
        {
            _logger.LogError("Invalid RfPath! RfPath: {RfPath} Rate:{Rate}", rfPath, rate);
        }
#endif
}

void RadioManagementModule::phy_set_tx_power_index_by_rate_section(
    RfPath rfPath, uint8_t channel, RATE_SECTION rateSection) {
  _logger->debug("SET_TX_POWER {} - {} - {}", (int)rfPath, (int)channel,
                 (int)rateSection);

  if (rateSection >= RATE_SECTION::RATE_SECTION_NUM) {
    throw std::logic_error("RateSection >= RATE_SECTION.RATE_SECTION_NUM");
  }

  // TODO: WTF is going on?
  if (rateSection == RATE_SECTION::CCK &&
      current_band_type != BandType::BAND_ON_2_4G) {
    return;
  }

  PHY_SetTxPowerIndexByRateArray(rfPath, rates_by_sections[(int)rateSection]);
}

void RadioManagementModule::PHY_TxPowerTrainingByPath_8812(RfPath rfPath) {
  if ((uint8_t)rfPath >= _eepromManager->numTotalRfPath) {
    return;
  }

  uint16_t writeOffset;
  uint32_t powerLevel;
  powerLevel = power;

  if (rfPath == RfPath::RF_PATH_A) {
    writeOffset = rA_TxPwrTraing_Jaguar;
  } else {
    writeOffset = rB_TxPwrTraing_Jaguar;
  }

  uint32_t writeData = 0;
  for (uint8_t i = 0; i < 3; i++) {
    if (i == 0) {
      powerLevel = powerLevel - 10;
    } else if (i == 1) {
      powerLevel = powerLevel - 8;
    } else {
      powerLevel = powerLevel - 6;
    }
    writeData |= (((powerLevel > 2) ? (powerLevel) : 2) << (i * 8));
  }

  _device.phy_set_bb_reg(writeOffset, 0xffffff, writeData);
}
