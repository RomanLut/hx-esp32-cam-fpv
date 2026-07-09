#include "RadioManagementModule.h"
#include "Hal8812PhyReg.h"
#include "InitTimer.h"
#include "registry_priv.h"

extern "C" {
#include "ieee80211_radiotap.h"
}

#include <chrono>
#include <cstdlib>
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
    RtlAdapter device, std::shared_ptr<EepromManager> eepromManager,
    Logger_t logger, const devourer::DeviceConfig &cfg)
    : _device{device}, _tuning{cfg.tuning},
      _keep_corrupted{cfg.rx.keep_corrupted},
      _dump_canary{cfg.debug.dump_canary}, _eepromManager{eepromManager},
      _logger{logger},
      _pwrTrk{device, eepromManager, this, logger},
      _iqk{device, eepromManager, this, logger}
#if defined(DEVOURER_HAVE_8814)
      ,
      _iqk8814{device, eepromManager, this, logger}
#endif
{}

void RadioManagementModule::RunIQK() {
  _iqk.Calibrate(_currentChannel, current_band_type, /*is_recovery=*/false);
}

uint32_t RadioManagementModule::phy_query_bb_reg_public(uint16_t regAddr,
                                                       uint32_t bitMask) {
  return phy_query_bb_reg(regAddr, bitMask);
}

void RadioManagementModule::InitPwrTrack() { _pwrTrk.Init(); }

void RadioManagementModule::TickPwrTrack() {
  _pwrTrk.TickThermalMeter(current_band_type, _currentChannel);
}

ThermalStatus RadioManagementModule::ReadThermalStatus() {
  /* Live thermal meter: RF path A, register 0x42, bits [15:10]. phy_query_rf_reg
   * already masks + shifts the bits down, so the result is the raw 6-bit reading.
   * Same register the 8812A power-track loop samples; here we read it standalone
   * (no chip-type gate, no BB-swing write) so the probe works on every Jaguar. */
  ThermalStatus s;
  uint32_t rf = phy_query_rf_reg(RfPath::RF_PATH_A, 0x42, 0xfc00u);
  s.raw = static_cast<uint8_t>(rf & 0x3F);
  s.baseline = _eepromManager->GetEepromThermalMeter();
  if (s.baseline == 0xFF) {
    s.valid = false;
    s.delta = 0;
  } else {
    s.valid = true;
    s.delta = static_cast<int>(s.raw) - static_cast<int>(s.baseline);
  }
  return s;
}

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

  /* DEVOURER_RX_KEEP_CORRUPTED: also pass frames whose 802.11 FCS (CRC32) or
   * decryption-ICV check failed. By default the chip drops them at the WMAC
   * filter — fine for clean-or-missing IP traffic, but it also hides any
   * partial-bit-error information that a FEC layer could otherwise use. With
   * the bits below set the frames reach the host with `crc_err` / `icv_err`
   * set on the RX descriptor; FrameParser surfaces them so a consumer like
   * tools/precoder/corruption_analysis.py can characterise the corruption.
   * Guarded by the same env var as the demo's filter — keep them in lockstep
   * so a noisy RX never surprises an IP-stack consumer that didn't ask for
   * it. */
  if (_keep_corrupted) {
    rcr_bits |= RCR_ACRC32 | RCR_AICV;
  }

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
  } else if (chnl_bw == ChannelWidth_t::CHANNEL_WIDTH_20 ||
             chnl_bw == ChannelWidth_t::CHANNEL_WIDTH_5 ||
             chnl_bw == ChannelWidth_t::CHANNEL_WIDTH_10) {
    /* 5/10 MHz narrowband (SPIKE, 8812A): a 20 MHz-based baseband re-clock —
     * the RF stays a 20 MHz tune, center == primary. */
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

  /* The full path may rewrite RF_CHNLBW (0x18) bandwidth bits + fc_area +
   * spur regs, so the fast_retune caches are stale after this. */
  _rf_chnlbw_cached = false;
  _last_fc_area = 0xffffffff;
  _last_spur_class = -1;
  _last_subchnl = -1;
  _bw_cache_valid = false; /* full path rewrites 0x8ac (phy_FixSpur etc.) */

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

bool RadioManagementModule::fast_retune(uint8_t channel, bool cache_rf) {
  /* Decline (without touching the chip) a band change — the caller falls back
   * to the full set_channel_bwmode, which does the RFE/AGC reconfig + IQK.
   * (_currentChannel is the center channel; its band == the primary's band.) */
  const bool cur_5g = _currentChannel > 14;
  const bool new_5g = channel > 14;
  if (cur_5g != new_5g)
    return false;

  if (_currentChannelBw != ChannelWidth_t::CHANNEL_WIDTH_20) {
    /* Wider-bandwidth hop. Across a same-BW, same-offset hop the ONLY thing
     * that changes is the RF center channel: the BW bits in RF 0x18, the
     * BW-only BB registers, and the secondary-channel registers (which depend
     * on the prime offset, not the channel number — see phy_GetSecondaryChnl)
     * are all constant. So the lean cached path writes the center channel from
     * the RF cache (no read-modify-write -> no 20 ms C-cut sleeps) and writes
     * the SubChnl registers only when they change. The BW-only registers were
     * set by the last full set at this BW (InitWrite) and are left untouched.
     * Register parity against the full path is checked by
     * tests/hop_parity_check.sh. */
    if (cache_rf && _eepromManager->version_id.ICType != CHIP_8814A) {
      InitTimer timer(_logger, "fast_retune");
      const uint8_t center =
          rtw_get_center_ch(channel, _currentChannelBw, _cur40MhzPrimeSc);
      _currentChannel = center;
      _currentCenterFrequencyIndex = center;
      phy_SwChnl8812_fast(center); /* cached RF center + fc_area + spur */

      const uint8_t sc = phy_GetSecondaryChnl_8812();
      if (static_cast<int>(sc) != _last_subchnl) {
        _device.rtw_write8(REG_DATA_SC_8812, sc);
        _device.phy_set_bb_reg(rRFMOD_Jaguar, 0x3C, sc);
        if (_currentChannelBw == ChannelWidth_t::CHANNEL_WIDTH_40) {
          _device.phy_set_bb_reg(rCCAonSec_Jaguar, 0xf0000000, sc);
          /* 1 == VHT_DATA_SC_20_UPPER_OF_80MHZ (enum defined later in this
           * file); matches the comparison in phy_PostSetBwMode8812. */
          _device.phy_set_bb_reg(rCCK_System_Jaguar, bCCK_System_Jaguar,
                                 sc == 1 ? 1 : 0);
        }
        _last_subchnl = sc;
      }
      timer.stage("sw_chnl_bw");
      timer.total();
      if (_dump_canary)
        DumpCanary();
      return true;
    }

    /* Non-cached fallback: reuse the full set_channel_bwmode (guaranteed
     * channel+BW parity) but skip the per-rate TX-power loop + pwrtrk tick via
     * _fast_skip_heavy. */
    InitTimer timer(_logger, "fast_retune");
    const uint8_t offset = _cur40MhzPrimeSc;
    const ChannelWidth_t bw = _currentChannelBw;
    _fast_skip_heavy = true;
    set_channel_bwmode(channel, offset, bw);
    _fast_skip_heavy = false;
    timer.stage("sw_chnl_bw");
    timer.total();
    return true;
  }

  if (channel == _currentChannel)
    return true;   /* already tuned here */

  InitTimer timer(_logger, "fast_retune");
  _currentChannel = channel;
  _currentCenterFrequencyIndex = channel;
  /* 20 MHz fast path: RF channel switch ONLY (primary == center) — no TX-power
   * loop, no bandwidth post-set, no thermal pwrtrk tick, no IQK. */
  if (cache_rf && _eepromManager->version_id.ICType != CHIP_8814A)
    phy_SwChnl8812_fast(channel);
  else
    phy_SwChnl8812();
  timer.stage("sw_chnl");
  timer.total();
  if (_dump_canary)
    DumpCanary();
  return true;
}

bool RadioManagementModule::fast_set_bandwidth(ChannelWidth_t new_bw) {
  /* 8812/8811 and 8814A only (8821 has no narrowband). Only toggles WITHIN
   * {20, 5, 10}: for all three the RF stays in 20 MHz mode (RF18[11:10]=3) and
   * the MAC stays 20 MHz (0x668 BW bits 0), so PHY_RF6052SetBandwidth8812 (the
   * per-path RF read-modify-write that owns the ~40 ms cut-C sleep),
   * phy_SetRegBW_8812, DATA_SC, and (8814) phy_SetBwRegAdc/Agc all write
   * identical values — the ONLY register that changes is 0x8ac. So the switch
   * is a single write-only dword. A 40/80 MHz endpoint DOES move
   * RF18/0x668/DATA_SC -> decline to the full path.
   *
   * The two dies pack 0x8ac differently: the 8812 divides via [9:8]/[21:20]
   * under mask 0x003003C3; the 8814 rides the 8822B layout ([9:8]+[16] /
   * [21:20]+[28]) under mask 0x103103C3 with the 8821C/8822B divide codes. */
  const auto ic = _eepromManager->version_id.ICType;
  uint32_t mask;
  uint32_t adc_nb5, adc_nb10, dac_nb5, dac_nb10;
  if (ic == CHIP_8812) {
    mask = 0x003003C3u;
    adc_nb5 = 0; adc_nb10 = 1; dac_nb5 = 1; dac_nb10 = 2;
#if defined(DEVOURER_HAVE_8814)
  } else if (ic == CHIP_8814A) {
    mask = 0x103103C3u;
    adc_nb5 = 2; adc_nb10 = 3; dac_nb5 = 2; dac_nb10 = 3;
#endif
  } else {
    return false;
  }
  auto in_set = [](ChannelWidth_t b) {
    return b == CHANNEL_WIDTH_20 || b == CHANNEL_WIDTH_5 ||
           b == CHANNEL_WIDTH_10;
  };
  const ChannelWidth_t cur = _currentChannelBw;
  if (!in_set(cur) || !in_set(new_bw))
    return false;
  if (new_bw == cur)
    return true;

  InitTimer timer(_logger, "fast_bw");

  /* Prime the compose-cache from a 20 MHz state ONLY — it must capture the
   * undivided 0x8ac (incl. phy_FixSpur's per-channel ADC-clock choice on the
   * 8812) so the 20 MHz restore is bit-exact. If we can't get a clean 20 MHz
   * snapshot, decline to the full path rather than cache a divided clock. */
  if (!_bw_cache_valid) {
    if (cur != CHANNEL_WIDTH_20)
      return false;
    _bw_cached_8ac = _device.rtw_read32(rRFMOD_Jaguar);
    _bw_cache_valid = true;
  }

  if (new_bw == CHANNEL_WIDTH_20) {
    /* Restore the cached 20 MHz dword verbatim — write-only, no read sleep. */
    _device.phy_set_bb_reg(rRFMOD_Jaguar, bMaskDWord, _bw_cached_8ac);
  } else {
    /* Compose the per-die narrowband divide codes onto the cached 20 MHz
     * 0x8ac — same codes as phy_PostSetBwMode8812/8814A's 5/10 branch. */
    const bool is5 = (new_bw == CHANNEL_WIDTH_5);
    uint32_t adc = is5 ? adc_nb5 : adc_nb10;
    uint32_t dac = is5 ? dac_nb5 : dac_nb10;
    const uint32_t smallbw = is5 ? 1u : 2u;
    if (_tuning.nb_adc)
      adc = *_tuning.nb_adc & 0x3;
    if (_tuning.nb_dac)
      dac = *_tuning.nb_dac & 0x3;
    const uint32_t fields =
        ((dac << 20) | (adc << 8) | (smallbw << 6)) & mask;
    const uint32_t v = (_bw_cached_8ac & ~mask) | fields;
    _device.phy_set_bb_reg(rRFMOD_Jaguar, bMaskDWord, v);
  }
  _currentChannelBw = new_bw;
  _logger->info("fast_bw: {} MHz (single 0x8ac write)",
                new_bw == CHANNEL_WIDTH_5 ? 5 : new_bw == CHANNEL_WIDTH_10 ? 10
                                                                          : 20);
  timer.stage("bb_reclock");
  timer.total();
  if (_dump_canary)
    DumpCanary();
  return true;
}

/* Cached-RF variant of phy_SwChnl8812. Same register effect, but the two
 * RF_CHNLBW (0x18) fields (RF_MOD_AG + channel byte) are merged into a cached
 * full-register value and written with the full LSSI-write mask — so no
 * read-modify-write, and thus none of the 20 ms C-cut RF-read sleeps that
 * dominate the masked-write path. 8812/8821 only; 8814 has its own channel-set. */
void RadioManagementModule::phy_SwChnl8812_fast(uint8_t channelToSW) {
  if (phy_SwBand8812(channelToSW) == false)
    _logger->error("error Chnl {} !", channelToSW);

  /* fc_area — identical boundaries to phy_SwChnl8812 (one BB write, no read). */
  uint32_t fc;
  if (36 <= channelToSW && channelToSW <= 48)
    fc = 0x494;
  else if (15 <= channelToSW && channelToSW <= 35)
    fc = 0x494;
  else if (50 <= channelToSW && channelToSW <= 80)
    fc = 0x453;
  else if (82 <= channelToSW && channelToSW <= 116)
    fc = 0x452;
  else if (118 <= channelToSW)
    fc = 0x412;
  else
    fc = 0x96a;
  /* fc_area is constant within a band — write only when it actually changes. */
  if (fc != _last_fc_area) {
    _device.phy_set_bb_reg(rFc_area_Jaguar, 0x1ffe0000, fc);
    _last_fc_area = fc;
  }

  /* RF_MOD_AG value for this channel range (same table as phy_SwChnl8812). */
  uint32_t mod_ag;
  if (36 <= channelToSW && channelToSW <= 80)
    mod_ag = 0x101;
  else if (15 <= channelToSW && channelToSW <= 35)
    mod_ag = 0x101;
  else if (82 <= channelToSW && channelToSW <= 140)
    mod_ag = 0x301;
  else if (140 < channelToSW)
    mod_ag = 0x501;
  else
    mod_ag = 0x000;
  const uint32_t mod_ag_mask = BIT18 | BIT17 | BIT16 | BIT9 | BIT8;
  const uint32_t mod_ag_shift = PHY_CalculateBitShift(mod_ag_mask);

  /* Prime the cache once (pays the 20 ms reads a single time); subsequent hops
   * write the full register straight from the cache. */
  if (!_rf_chnlbw_cached) {
    for (uint8_t p = 0; p < _eepromManager->numTotalRfPath && p < 4; p++)
      _rf_chnlbw_cache[p] =
          phy_query_rf_reg((RfPath)p, RF_CHNLBW_Jaguar, bLSSIWrite_data_Jaguar);
    _rf_chnlbw_cached = true;
  }

  /* phy_FixSpur_8812A writes GLOBAL (not per-path) BB ADC-clock regs, and its
   * output depends only on (bandwidth, whether the channel is 11/13/14). The
   * original per-path loop called it redundantly N times; here it runs once,
   * and only when its config class actually changes across the hop set. */
  const int spur_class =
      (_currentChannelBw == ChannelWidth_t::CHANNEL_WIDTH_40 && channelToSW == 11)
          ? 1
      : (_currentChannelBw == ChannelWidth_t::CHANNEL_WIDTH_20 &&
         (channelToSW == 13 || channelToSW == 14))
          ? 2
          : 0;
  if (spur_class != _last_spur_class) {
    phy_FixSpur_8812A(_currentChannelBw, channelToSW);
    _last_spur_class = spur_class;
  }

  for (uint8_t p = 0; p < _eepromManager->numTotalRfPath && p < 4; p++) {
    uint32_t v = _rf_chnlbw_cache[p];
    /* Merge exactly as phy_set_rf_reg would (no masking of the shifted field,
     * matching its semantics). */
    v = (v & ~mod_ag_mask) | (mod_ag << mod_ag_shift);
    v = (v & ~(uint32_t)bMaskByte0) | channelToSW;
    v &= bLSSIWrite_data_Jaguar;
    phy_set_rf_reg((RfPath)p, RF_CHNLBW_Jaguar, bLSSIWrite_data_Jaguar, v);
    _rf_chnlbw_cache[p] = v;
  }
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
    _logger->info("[{}]: _swChannel {}, _setChannelBw {}", __func__,
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
  InitTimer timer(_logger, "channel_set");
  if (_swChannel) {
    phy_SwChnl8812();
    _swChannel = false;
  }
  timer.stage("sw_chnl");

  if (_setChannelBw) {
    phy_PostSetBwMode8812();
    _setChannelBw = false;
  }
  timer.stage("set_bw");
  /* 8814A uses a packed single-DWord write to BB 0x1998 per (path,rate)
   * instead of the 8812's per-rate per-byte register fanout (see the
   * CHIP_8814A branch at the top of PHY_SetTxPowerIndex_8812A). The
   * earlier "skip TX power on 8814" workaround was a symptom of the
   * 8812 register layout being wrong for 8814 — every write hit the
   * wrong bits and the chip's BB stalled on each one. Setting
   * DEVOURER_SKIP_TXPWR=1 keeps the old skip behaviour as an escape
   * hatch (e.g. for RX-only experiments). */
  if (_fast_skip_heavy || _tuning.skip_txpwr) {
    if (!_fast_skip_heavy)
      _logger->info("tuning.skip_txpwr — skipping TX power setup");
  } else {
    PHY_SetTxPowerLevel8812(_currentChannel);
  }
  timer.stage("tx_power");

  /* Run phydm thermal-meter pwrtrk once per channel-set. Mirrors the
   * upstream watchdog tick — reads RF[A][0x42], folds into the
   * thermal-value rolling average, walks the delta-swing table for
   * the (band, channel) bucket, and writes the resulting BB-swing
   * index to 0xc1c[31:21] / 0xe1c[31:21].
   *
   * 8812A-only. The delta-swing tables + `PowerTracking8812a` logic
   * came from `aircrack-ng/rtl8812au/hal/phydm/halrf/rtl8812a/
   * halrf_8812a_ce.c`. The 8821AU has its own `halrf_8821a_ce.c`
   * variant with different per-band tables and 1T1R-specific math —
   * running 8812A code on 8821A produced wrong values at ch6 BB
   * 0xc1c[31:21] (T1 8821 canary diff caught it as kern 0x200/0dB vs
   * dev 0x1C8/-1dB). Until the 8821 pwrtrk is ported, skip the tick
   * on non-8812 chips. */
  if (!_fast_skip_heavy &&
      _eepromManager->version_id.ICType == CHIP_8812) {
    _pwrTrk.TickThermalMeter(current_band_type, _currentChannel);
  }

  /* Kernel cross-validation oracle: when debug.dump_canary is set
   * is set, dump the canary BB/MAC/RF registers after channel-set is
   * complete. Output format matches `iwpriv <iface> read 4,<addr>` /
   * `iwpriv <iface> rfr <path> <addr>` so kernel and devourer dumps
   * can be diffed line-by-line.
   *
   * BB 0xc1c[31:21] / 0xe1c[31:21] are now phydm-managed; the value
   * is thermal-meter-driven so small (~1-step) divergence is expected
   * if devourer and kernel sampled the chip at non-identical
   * temperatures. Capture both dumps within a few seconds for clean
   * parity.
   *
   * The IQK trigger BELOW runs phydm I/Q calibration which touches
   * RF[*][0x0] / RF[*][0x8f] + BB IQK-output regs (0xc90 / 0xe90 /
   * 0xcc4 / etc). We capture the canary AFTER IQK so it reflects the
   * post-calibration state — matching kernel semantics where IQK is
   * part of the channel-set callback. */
  /* (Canary register dump is DumpCanary(), called below and from the fast
   * 40/80 path so its channel/BW register state is parity-checkable.) */

  /* Trigger I/Q calibration. Set by `phy_SwBand8812` on band
   * transitions and (post-init) on the very first channel-set via
   * `HalModule::rtl8812au_hal_init` → `ArmIQKOnNextChannelSet`.
   * Optional override: `DEVOURER_FORCE_IQK=1` runs IQK on every
   * channel-set (used for canary-diff workflow against kernel). */
  if ((_needIQK || _tuning.force_iqk) && !_tuning.disable_iqk) {
    if (_eepromManager->version_id.ICType == CHIP_8812) {
      _iqk.Calibrate(_currentChannel, current_band_type,
                     /*is_recovery=*/false);
    }
#if defined(DEVOURER_HAVE_8814)
    else if (_eepromManager->version_id.ICType == CHIP_8814A) {
      _iqk8814.Calibrate(_currentChannel, current_band_type,
                         /*is_recovery=*/false);
    }
#endif
  }
  _needIQK = false;
  timer.stage("iqk");
  timer.total();

  /* Canary dump runs LAST so it captures the post-IQK / post-pwrtrk
   * state — same observation order as kernel iface reads via
   * `iwpriv read 4,<addr>`. */
  if (_dump_canary) {
    DumpCanary();
  }
}

/* Canary register dump for kernel cross-validation / fast-vs-full parity. Reads
 * a fixed per-chip set of BB/MAC/RF registers and logs them in a stable format
 * (matches `iwpriv read`). See the call sites in phy_SwChnlAndSetBwMode8812 and
 * fast_retune. */
void RadioManagementModule::DumpCanary() {
  /* Per-chip canary set. 8821AU = path A only; 8812AU = A+B; 8814AU = A+B+C+D
   * (RF[C]/RF[D] are write-only, so no RF reads for them). MAC set is shared. */
  static const uint16_t bb_anchors[] = {
      0x808, 0x80c, 0x82c, 0x830, 0x834, 0x838, 0x84c, 0x860, 0x8ac,
      0x8b0, 0x8c4};
  static const uint16_t bb_pathA[] = {
      0xc00, 0xc1c, 0xc20, 0xc24, 0xc28, 0xc2c, 0xc30, 0xc34, 0xc38,
      0xc3c, 0xc40, 0xc50, 0xc54, 0xc60, 0xc64, 0xc68, 0xc6c, 0xc70,
      0xc10, 0xc14, 0xc90, 0xc94};
  static const uint16_t bb_pathB[] = {
      0xe1c, 0xe20, 0xe24, 0xe28, 0xe2c, 0xe30, 0xe34, 0xe38, 0xe3c,
      0xe40, 0xe50, 0xe54, 0xe10, 0xe14, 0xe90, 0xe94};
  static const uint16_t bb_pathC[] = {
      0x1820, 0x1824, 0x1828, 0x182c, 0x1830, 0x1834, 0x1838, 0x183c,
      0x1840, 0x181c, 0x1850};
  static const uint16_t bb_pathD[] = {
      0x1a20, 0x1a24, 0x1a28, 0x1a2c, 0x1a30, 0x1a34, 0x1a38, 0x1a3c,
      0x1a40, 0x1a1c, 0x1a50};
  static const uint16_t mac_canary[] = {0x40,  0xcf,  0xf0,  0x100,
                                        0x102, 0x420, 0x4c8, 0x508,
                                        0x522, 0x550, 0x560, 0x610,
                                        0x614};
  static const uint32_t rf_canary[] = {0x00, 0x05, 0x18, 0x42, 0x65, 0x8f};

  const auto ictype = _eepromManager->version_id.ICType;
  const bool has_pathB = (ictype != CHIP_8821);
  const bool has_pathCD = (ictype == CHIP_8814A);

  _logger->info("=== DEVOURER_DUMP_CANARY (post channel-set ch={}) ===",
                unsigned(_currentChannel));
  for (uint16_t a : bb_anchors)
    _logger->info("BB 0x{:03x} = 0x{:08X}", a, _device.rtw_read32(a));
  for (uint16_t a : bb_pathA)
    _logger->info("BB 0x{:03x} = 0x{:08X}", a, _device.rtw_read32(a));
  if (has_pathB)
    for (uint16_t a : bb_pathB)
      _logger->info("BB 0x{:03x} = 0x{:08X}", a, _device.rtw_read32(a));
  if (has_pathCD) {
    for (uint16_t a : bb_pathC)
      _logger->info("BB 0x{:04x} = 0x{:08X}", a, _device.rtw_read32(a));
    for (uint16_t a : bb_pathD)
      _logger->info("BB 0x{:04x} = 0x{:08X}", a, _device.rtw_read32(a));
  }
  for (uint16_t a : mac_canary)
    _logger->info("MAC 0x{:03x} = 0x{:08X}", a, _device.rtw_read32(a));
  for (uint32_t a : rf_canary)
    _logger->info("RF[A] 0x{:02x} = 0x{:05X}", a,
                  phy_query_rf_reg(RfPath::RF_PATH_A, a, 0xfffffu));
  if (has_pathB)
    for (uint32_t a : rf_canary)
      _logger->info("RF[B] 0x{:02x} = 0x{:05X}", a,
                    phy_query_rf_reg(RfPath::RF_PATH_B, a, 0xfffffu));
  _logger->info("=== END DEVOURER_DUMP_CANARY ===");
}

void RadioManagementModule::phy_set_rf_reg(RfPath eRFPath, uint16_t RegAddr,
                                           uint32_t BitMask, uint32_t Data) {
  uint32_t data = Data;
  //DVR_DEBUG(_logger, "RFREG;{};{:X};{:X};{:X}", (uint8_t)eRFPath, (uint)RegAddr,
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
  if (_eepromManager->version_id.ICType == CHIP_8814A) {
    /* Kernel phy_RFRead_8814A (rtl8814a_phycfg.c:86-122): 8814 RF
     * registers are read back through per-path direct BB shadow blocks,
     * NOT the 8812 HSSI/LSSI serial mechanism below. This is also the
     * only read path that works for paths C/D — the SI readback returns
     * garbage there, which corrupted every masked (read-modify-write) RF
     * write on those paths: the channel and bandwidth RMWs of RF 0x18
     * destroyed all bits outside their masks on C/D at every channel
     * set. */
    static constexpr uint16_t kDirectBase[4] = {0x2800, 0x2c00, 0x3800,
                                                0x3c00};
    const uint16_t direct_addr = (uint16_t)(
        kDirectBase[static_cast<int>(eRFPath) & 3] + (Offset & 0xff) * 4);
    return phy_query_bb_reg(direct_addr, 0xfffff /* bRFRegOffsetMask */);
  }

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

/* Port of upstream `PHY_SetRFEReg8814A` band-switch path (bInit=false)
 * from `aircrack-ng/rtl8812au/hal/rtl8814a/rtl8814a_phycfg.c:1567`.
 * 8814AU has its own RFE pinmux for all four paths (A/B/C/D) at
 * 0xCB0 / 0xEB0 / 0x18B4 / 0x1AB4 plus the 0x1ABC[27:20] tail;
 * the 8812 RFE function never touches the path-C/D regs so running
 * it on 8814 leaves the LNA in SW-managed mode (visible as RF[A] 0x00
 * bit 15 = 1 in canary diff) and the path-C/D antenna mux unprogrammed.
 *
 * rfe_type comes from EFUSE. Cases 0/1/2 are the only ones upstream
 * 8814A handles; other rfe_type values fall through to case 0/default. */
void RadioManagementModule::phy_SetRFEReg8814A(BandType Band) {
  const auto rfe_type = _eepromManager->rfe_type;
  if (Band == BandType::BAND_ON_2_4G) {
    switch (rfe_type) {
    case 2:
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x72707270);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x72707270);
      _device.phy_set_bb_reg(0x18B4, bMaskDWord, 0x72707270); /* rC_RFE_Pinmux */
      _device.phy_set_bb_reg(0x1AB4, bMaskDWord, 0x77707770); /* rD_RFE_Pinmux */
      _device.phy_set_bb_reg(0x1ABC, 0x0FF00000, 0x72);        /* [27:20] */
      break;
    case 1:
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x77777777);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x77777777);
      _device.phy_set_bb_reg(0x18B4, bMaskDWord, 0x77777777);
      _device.phy_set_bb_reg(0x1AB4, bMaskDWord, 0x77777777);
      _device.phy_set_bb_reg(0x1ABC, 0x0FF00000, 0x77);
      break;
    case 0:
    default:
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x77777777);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x77777777);
      _device.phy_set_bb_reg(0x18B4, bMaskDWord, 0x77777777);
      /* Upstream case-0/default skips rD_RFE_Pinmux entirely. */
      _device.phy_set_bb_reg(0x1ABC, 0x0FF00000, 0x77);
      break;
    }
  } else {
    switch (rfe_type) {
    case 2:
      /* Kernel PHY_SetRFEReg8814A 5G case 2: 0x37173717 on A/B/C — the
       * previous 0x33173717 carried rfe-1's [27:24] nibble (copy slip
       * between adjacent cases; flagged independently by two audit
       * passes against the rtl8814au reference). */
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x37173717);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x37173717);
      _device.phy_set_bb_reg(0x18B4, bMaskDWord, 0x37173717);
      _device.phy_set_bb_reg(0x1AB4, bMaskDWord, 0x77177717);
      _device.phy_set_bb_reg(0x1ABC, 0x0FF00000, 0x37);
      break;
    case 1:
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x33173317);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x33173317);
      _device.phy_set_bb_reg(0x18B4, bMaskDWord, 0x33173317);
      _device.phy_set_bb_reg(0x1AB4, bMaskDWord, 0x77177717);
      _device.phy_set_bb_reg(0x1ABC, 0x0FF00000, 0x33);
      break;
    case 0:
    default:
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x54775477);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x54775477);
      _device.phy_set_bb_reg(0x18B4, bMaskDWord, 0x54775477);
      _device.phy_set_bb_reg(0x1AB4, bMaskDWord, 0x54775477);
      _device.phy_set_bb_reg(0x1ABC, 0x0FF00000, 0x54);
      break;
    }
  }
}

void RadioManagementModule::InitRFEGpio8814A() {
  /* Mirror of the kernel PHY_SetRFEReg8814A(bInit=TRUE) branch
   * (rtl8814a_phycfg.c:1026-1039, called once from usb_halinit.c:1279).
   * Enables the GPIO pins that physically drive the external RFE (PA + T/R
   * antenna switch). devourer's per-band phy_SetRFEReg8814A programs only the
   * RFE pinmux *functions* (0xCB0/0xEB0/0x18B4/0x1AB4); without this one-time
   * pin-select the pins are never enabled as RFE outputs, so the external
   * PA/T-R switch never engages on TX — TX submits succeed (err:0) but nothing
   * reaches the air, while RX still works (issue surfaced after the chip
   * stopped inheriting a prior kernel-set GPIO state). */
  const auto rfe_type = _eepromManager->rfe_type;
  constexpr uint16_t REG_GPIO_IO_SEL_8814A = 0x0042; /* byte 2 of the 0x40 dword */
  _device.phy_set_bb_reg(0x1994, 0xf, 0xf); /* 0x1994[3:0] = 0xf */
  const uint8_t sel = _device.rtw_read8(REG_GPIO_IO_SEL_8814A);
  /* rfe_type 1/2 -> 0x40[23:20]=0xf (0x42 |= 0xf0); type 0 -> [23:22]=11b (0xc0) */
  const uint8_t orv = (rfe_type == 0) ? 0xc0 : 0xf0;
  _device.rtw_write8(REG_GPIO_IO_SEL_8814A, (uint8_t)(sel | orv));
  _logger->info("8814A RFE GPIO pin-select (rfe_type={}, 0x42 |= 0x{:02x})",
                (int)rfe_type, orv);
}

/* Port of upstream `phy_SetBwRegAdc_8814A`
 * (rtl8814a_phycfg.c:1454). Programs rRFMOD_Jaguar (0x8AC) bits [1:0]
 * per bandwidth; both bands write the same value here. */
void RadioManagementModule::phy_SetBwRegAdc_8814A(BandType Band,
                                                  ChannelWidth_t bw) {
  /* NB: the 8814A ADC-clock BW config is a DIFFERENT mechanism from the
   * 8812/8821 0x8ac[9:8] divider — here 0x8ac[1:0] is a mode selector
   * (20M=0/40M=1/80M=2) paired with phy_SetBwRegAgc_8814A. The 8812AU
   * narrowband divide (5/10 MHz) does not transplant here; 8814 narrowband
   * is a separate, unported research effort (caps gate it off — see
   * RtlJaguarDevice::GetAdapterCaps). */
  (void)Band;
  uint32_t val;
  switch (bw) {
  case ChannelWidth_t::CHANNEL_WIDTH_5:  /* SPIKE: narrowband -> 20M mode */
  case ChannelWidth_t::CHANNEL_WIDTH_10:
  case ChannelWidth_t::CHANNEL_WIDTH_20:
    val = 0x0;
    break;
  case ChannelWidth_t::CHANNEL_WIDTH_40:
    val = 0x1;
    break;
  case ChannelWidth_t::CHANNEL_WIDTH_80:
    val = 0x2;
    break;
  default:
    _logger->error("phy_SetBwRegAdc_8814A: unknown bw {}",
                   static_cast<int>(bw));
    return;
  }
  _device.phy_set_bb_reg(rRFMOD_Jaguar, BIT1 | BIT0, val);
}

/* Port of upstream `phy_SetBwRegAgc_8814A`
 * (rtl8814a_phycfg.c:1496). 0x82C[15:12] AGC value: 20MHz=6, 80MHz=3,
 * 40MHz=7 (2.4G) or 8 (5G). */
void RadioManagementModule::phy_SetBwRegAgc_8814A(BandType Band,
                                                  ChannelWidth_t bw) {
  uint32_t agc;
  switch (bw) {
  case ChannelWidth_t::CHANNEL_WIDTH_5:  /* SPIKE: narrowband -> 20M AGC */
  case ChannelWidth_t::CHANNEL_WIDTH_10:
  case ChannelWidth_t::CHANNEL_WIDTH_20:
    agc = 6;
    break;
  case ChannelWidth_t::CHANNEL_WIDTH_40:
    agc = (Band == BandType::BAND_ON_5G) ? 8 : 7;
    break;
  case ChannelWidth_t::CHANNEL_WIDTH_80:
    agc = 3;
    break;
  default:
    _logger->error("phy_SetBwRegAgc_8814A: unknown bw {}",
                   static_cast<int>(bw));
    return;
  }
  _device.phy_set_bb_reg(rAGC_table_Jaguar, 0xf000, agc);
}

/* Port of upstream `phy_SetBBSwingByBand_8814A` (rtl8814a_phycfg.c:1652).
 * Writes TX scale bits 31:21 for all four paths. Reuses the existing
 * `phy_get_tx_bb_swing_8812a` (extended to handle path C/D bit
 * extraction from the EFUSE swing byte). 0x181C / 0x1A1C are the
 * path-C/D TX scale registers per `hal/Hal8814PhyReg.h`. */
void RadioManagementModule::phy_SetBBSwingByBand_8814A(BandType Band) {
  _device.phy_set_bb_reg(
      rA_TxScale_Jaguar, 0xFFE00000,
      phy_get_tx_bb_swing_8812a(Band, RfPath::RF_PATH_A));
  _device.phy_set_bb_reg(
      rB_TxScale_Jaguar, 0xFFE00000,
      phy_get_tx_bb_swing_8812a(Band, RfPath::RF_PATH_B));
  _device.phy_set_bb_reg(
      0x181c, 0xFFE00000,
      phy_get_tx_bb_swing_8812a(Band, RfPath::RF_PATH_C));
  _device.phy_set_bb_reg(
      0x1a1c, 0xFFE00000,
      phy_get_tx_bb_swing_8812a(Band, RfPath::RF_PATH_D));
}

/* Port of upstream `PHY_SwitchWirelessBand8814A`
 * (rtl8814a_phycfg.c:1688). 8814 has its own band-switch sequence,
 * not a superset of the 8812 path. Running the 8812 band-switch on
 * 8814 leaves path C/D RFE unprogrammed and the LNA in SW-managed
 * mode (RF[A] 0x00 bit 15 = 1 at 5G in canary diff); the AGC table
 * register and per-band rTxPath / rCCK_RX values also differ. The
 * CCK+OFDM clock-gate cycle around the switch (`REG_SYS_CFG3_8814A`
 * bit 16) is unique to 8814; upstream gates the chip's BB clocks
 * off for the switch then re-enables. */
void RadioManagementModule::PHY_SwitchWirelessBand8814A(BandType Band) {
  /* `REG_SYS_CFG3_8814A = 0x1000` per `hal/rtl8814a_spec.h`; bit 16 of
   * the dword lives in the +2 byte. */
  constexpr uint16_t kRegSysCfg38814AHi = 0x1002;
  constexpr uint16_t kRegCckCheck8814A  = 0x0454;

  _logger->info("[{}] {}", __func__,
                Band == BandType::BAND_ON_2_4G ? "2.4G" : "5G");

  current_band_type = Band;

  /* Disable BB CCK+OFDM clocks for the switch. */
  uint8_t sys_cfg3 = _device.rtw_read8(kRegSysCfg38814AHi);
  _device.rtw_write8(kRegSysCfg38814AHi, (uint8_t)(sys_cfg3 & ~BIT0));

  if (Band == BandType::BAND_ON_2_4G) {
    /* 8814 AGC table select lives at 0x958[4:0]
     * (`rAGC_table_Jaguar2` in `hal/Hal8814PhyReg.h`), NOT 0x82C[1:0]
     * (`rAGC_table_Jaguar`) — different register and different mask
     * from the 8812 path. */
    _device.phy_set_bb_reg(0x958, 0x1F, 0);
    phy_SetRFEReg8814A(Band);
    _device.phy_set_bb_reg(rTxPath_Jaguar, 0xf0, 0x2);    /* 0x80C[7:4] */
    _device.phy_set_bb_reg(rCCK_RX_Jaguar, 0x0f000000, 0x5);
    _device.phy_set_bb_reg(rOFDMCCKEN_Jaguar,
                           bOFDMEN_Jaguar | bCCKEN_Jaguar, 0x3);
    _device.rtw_write8(kRegCckCheck8814A, 0x0);
    _device.phy_set_bb_reg(0xa80, BIT18, 0x0);            /* CCK Tx disable */
  } else {
    _device.rtw_write8(kRegCckCheck8814A, 0x80);
    _device.phy_set_bb_reg(0xa80, BIT18, 0x1);            /* CCK Tx enable */
    /* AGC table select postponed to channel switch per upstream comment. */
    phy_SetRFEReg8814A(Band);
    _device.phy_set_bb_reg(rTxPath_Jaguar, 0xf0, 0x0);
    _device.phy_set_bb_reg(rCCK_RX_Jaguar, 0x0f000000, 0xF);
    _device.phy_set_bb_reg(rOFDMCCKEN_Jaguar,
                           bOFDMEN_Jaguar | bCCKEN_Jaguar, 0x02);
  }

  phy_SetBBSwingByBand_8814A(Band);
  phy_SetBwRegAdc_8814A(Band, _currentChannelBw);
  phy_SetBwRegAgc_8814A(Band, _currentChannelBw);

  /* Re-enable BB CCK+OFDM clocks. */
  sys_cfg3 = _device.rtw_read8(kRegSysCfg38814AHi);
  _device.rtw_write8(kRegSysCfg38814AHi, (uint8_t)(sys_cfg3 | BIT0));
}

void RadioManagementModule::phy_SetBBSwingByBand_8812A(BandType Band) {
  _device.phy_set_bb_reg(
      rA_TxScale_Jaguar, 0xFFE00000,
      phy_get_tx_bb_swing_8812a(Band, RfPath::RF_PATH_A)); /* 0xC1C[31:21] */
  _device.phy_set_bb_reg(
      rB_TxScale_Jaguar, 0xFFE00000,
      phy_get_tx_bb_swing_8812a(Band, RfPath::RF_PATH_B)); /* 0xE1C[31:21] */

  /* Mirror upstream `phy_SetBBSwingByBand_8812A` which calls
   * `odm_clear_txpowertracking_state(pDM_Odm)` after rewriting the
   * BB-swing base. Without this the next pwrtrk tick sees
   * delta_abs==0 (thermal_value unchanged since the previous tick)
   * and short-circuits the re-apply — leaving 0xc1c[31:21] stuck at
   * the BB-init base (0x200) even when the previous channel-set's
   * tick had walked it up to the thermal-warmed value. */
  _pwrTrk.ClearState();
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
        swing = 0x55; /* -3 dB (all 4 paths; matches PHY_GetTxBBSwing_8814A) */
      else if (bbSwing_2G == -6)
        swing = 0xAA; /* -6 dB (all 4 paths) */
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
        swing = 0x55; /* -3 dB (all 4 paths; matches PHY_GetTxBBSwing_8814A) */
      else if (bbSwing_5G == -6)
        swing = 0xAA; /* -6 dB (all 4 paths) */
      else if (bbSwing_5G == -9)
        swing = 0xFF; /* -9 dB */
      else
        swing = 0x00;
    }

    if (RFPath == RfPath::RF_PATH_A) {
      onePathSwing = (uint8_t)((swing & 0x3) >> 0); /* 0xC6/C7[1:0] */
    } else if (RFPath == RfPath::RF_PATH_B) {
      onePathSwing = (uint8_t)((swing & 0xC) >> 2); /* 0xC6/C7[3:2] */
    } else if (RFPath == RfPath::RF_PATH_C) {
      onePathSwing = (uint8_t)((swing & 0x30) >> 4); /* 0xC6/C7[5:4] — 8814 */
    } else if (RFPath == RfPath::RF_PATH_D) {
      onePathSwing = (uint8_t)((swing & 0xC0) >> 6); /* 0xC6/C7[7:6] — 8814 */
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
  /* If `HalModule::rtl8812au_hal_init` already programmed the same
   * (channel, bw, offset) AND the chip is an 8821AU, skip the
   * reset-and-redo. The second channel-set wedges 8821AU at ch100
   * mid-TX-power loop (chip stops ACK'ing USB control transfers
   * after 2 BB writes; verified on hardware that the second
   * channel-set is the trigger, not the band-switch). Other chips
   * (8812AU, 8814AU) tolerate the second pass; some 8814AU init
   * steps appear to depend on it (gating the skip caused 8814 ch6
   * to lose its RX loop entry).
   *
   * Falls through to the historical reset+redo path for everything
   * else. */
  bool same_target =
      (_currentChannel == pmlmeext.Channel) &&
      (_currentChannelBw == pmlmeext.ChannelWidth) &&
      (_cur40MhzPrimeSc == pmlmeext.ChannelOffset) &&
      (current_band_type != BandType::BAND_MAX);
  bool is_8821 = _eepromManager->version_id.ICType == CHIP_8821;
  if (same_target && is_8821) {
    Set_HW_VAR_ENABLE_RX_BAR(true);
    return;
  }

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
  /* Extended-synth channels (below-band 15..35 / above 177, freq =
   * 5000 + 5*ch up to ch 253): the RF tunes them, but the power tables and
   * per-channel constants are clamped from the nearest characterized
   * channel. Warn once so an unexpected retune there is visible. */
  if (!_warned_uncharacterized &&
      ((_currentChannel >= 15 && _currentChannel <= 35) ||
       _currentChannel > 177)) {
    _warned_uncharacterized = true;
    _logger->warn("channel {} is outside the characterized range — TX power "
                  "and per-channel constants extrapolated from the nearest "
                  "table entry",
                  _currentChannel);
  }

  /* 8814 has its own channel-set: different fc_area boundaries, RF_MOD_AG
   * channel ranges, a 5G AGC-table sub-select at 0x958[4:0], 2.4G CCK
   * TX DFIR writes, and the combined channel+mod RF write pattern. The
   * 8812 path's `phy_FixSpur_8812A` workaround is also 8812-specific
   * (cut-C ADC FIFO clock at ch11) and shouldn't run on 8814. */
  if (_eepromManager->version_id.ICType == CHIP_8814A) {
    phy_SwChnl8814A();
    return;
  }

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

  /* 8814AU uses REG_CCK_CHECK_8814A (0x0454); 8812/8821 use
   * REG_CCK_CHECK_8812 (typically 0x4CA). Reading the wrong register
   * yields a bogus "current band" and triggers band-switch when it
   * shouldn't (or skips it when it should). */
  constexpr uint16_t kRegCckCheck8814A = 0x0454;
  const bool is_8814 =
      _eepromManager->version_id.ICType == CHIP_8814A;
  const uint16_t cck_check_reg =
      is_8814 ? kRegCckCheck8814A : REG_CCK_CHECK_8812;
  u1Btmp = _device.rtw_read8(cck_check_reg);
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

  /* The shadow band-type must track the channel's band even when no HW
   * band-switch is needed (the chip is already on that band). It used to be
   * updated only inside PHY_SwitchWirelessBand8812, so after init_hw_mlme_ext
   * reset it to BAND_MAX and the following same-band channel-set skipped the
   * switch, current_band_type stayed BAND_MAX. That made bIsIn24G false and
   * silently skipped CCK TX-power programming on every later TX-power apply
   * (and fed BAND_MAX to the thermal tracker + IQK). */
  current_band_type = BandToSW;

  if (BandToSW != Band) {
    /* Per-chip band-switch. 8814 has a completely separate sequence
     * (path-C/D RFE pinmux, 8814 AGC table register, CCK clock-gate
     * cycle, different rTxPath/rCCK_RX values) — see
     * `PHY_SwitchWirelessBand8814A`. 8812 and 8821 share the
     * `PHY_SwitchWirelessBand8812` path (with `is_8821` branches
     * inside for the 8821-specific RFE / AGC writes). */
    if (_eepromManager->version_id.ICType == CHIP_8814A) {
      PHY_SwitchWirelessBand8814A(BandToSW);
    } else {
      PHY_SwitchWirelessBand8812(BandToSW);
    }
    /* Band transition invalidates IQK results — RX LNA, RFE pinmux,
     * BB-swing base all change. Mirror upstream where
     * `PHY_SwitchWirelessBand8812` is followed by `phy_iq_calibrate_*`
     * on the next watchdog tick. */
    _needIQK = true;
  }

  return ret_value;
}

/* Port of upstream `phy_SwChnl8814A` (rtl8814a_phycfg.c:2448). The 8814
 * channel-set differs from the 8812 path on several fronts:
 *   - fc_area boundaries: 8814 splits 50-64 / 100-116 / 118+; 8812 uses
 *     50-80 / 82-116 + an extra 15-35 case.
 *   - RF_MOD_AG channel ranges: 8814 uses 36-64 / 100-140 / 140+ for
 *     0x101/0x301/0x501; 8812 uses 36-80 / 82-140 / 140<.
 *   - 5G AGC table sub-select: 8814 writes 0x958[4:0] = 1/2/3 per 5G
 *     channel band (36-64 / 100-144 / >=149); 8812 has no equivalent.
 *   - 2.4G CCK TX DFIR coefficients (channels 1-14): 8814 reprograms
 *     rCCK0_TxFilter1/2 and rCCK0_DebugPort per channel range; 8812
 *     doesn't.
 *   - 8812-specific `phy_FixSpur_8812A` (cut-C ADC FIFO clock workaround
 *     for ch11) is skipped entirely on 8814.
 *
 * Skips MP-mode-only paths (phy_ADC_CLK_8814A, phy_SpurCalibration_8814A,
 * phy_ModifyInitialGain_8814A) — devourer only runs monitor mode.
 * Skips the FW-offload `H2C_CHNL_SWITCH_OFFLOAD` path — devourer doesn't
 * have the H2C mailbox plumbing. */
void RadioManagementModule::phy_SwChnl8814A() {
  const uint8_t channelToSW = _currentChannel;

  if (phy_SwBand8812(channelToSW) == false) {
    _logger->error("error Chnl {} !", channelToSW);
  }

  /* fc_area — 8814A boundaries. Below-band ch 15..35 rides the low 5G bucket
   * (extended-synth clamp, matching the 8812 path); >177 is already covered by
   * the open-ended 118<= case. */
  uint32_t fc_area;
  if (15 <= channelToSW && channelToSW <= 48) {
    fc_area = 0x494;
  } else if (50 <= channelToSW && channelToSW <= 64) {
    fc_area = 0x453;
  } else if (100 <= channelToSW && channelToSW <= 116) {
    fc_area = 0x452;
  } else if (118 <= channelToSW) {
    fc_area = 0x412;
  } else {
    fc_area = 0x96a;
  }
  _device.phy_set_bb_reg(rFc_area_Jaguar, 0x1ffe0000, fc_area);

  for (uint8_t eRFPath = 0; eRFPath < _eepromManager->numTotalRfPath;
       ++eRFPath) {
    /* RF_MOD_AG — 8814A boundaries. ch 15..35 -> low band (extended-synth). */
    uint32_t rf_val;
    if (15 <= channelToSW && channelToSW <= 64) {
      rf_val = 0x101;
    } else if (100 <= channelToSW && channelToSW <= 140) {
      rf_val = 0x301;
    } else if (140 < channelToSW) {
      rf_val = 0x501;
    } else {
      rf_val = 0x000;
    }
    /* Combined RF write: RF_MOD_AG bits + channel byte, single RMW.
     * Mask BIT18|BIT17|BIT16|BIT9|BIT8 has lowest bit = BIT8, so
     * phy_set_rf_reg's BitShift = 0 for the combined mask
     * (BIT18|BIT17|BIT16|BIT9|BIT8|bMaskByte0 → lowest bit is BIT0).
     * Pre-shift rf_val into bits 16:8, OR with channel byte. */
    const uint32_t combined = (rf_val << 8) | channelToSW;
    phy_set_rf_reg(static_cast<RfPath>(eRFPath), RF_CHNLBW_Jaguar,
                   BIT18 | BIT17 | BIT16 | BIT9 | BIT8 | bMaskByte0,
                   combined);
  }

  /* 5G AGC table sub-select (rAGC_table_Jaguar2 = 0x958, 8814-only).
   * ch 15..35 -> low-band table (extended-synth clamp). */
  if (15 <= channelToSW && channelToSW <= 64) {
    _device.phy_set_bb_reg(0x958, 0x1F, 1);
  } else if (100 <= channelToSW && channelToSW <= 144) {
    _device.phy_set_bb_reg(0x958, 0x1F, 2);
  } else if (channelToSW >= 149) {
    _device.phy_set_bb_reg(0x958, 0x1F, 3);
  }

  /* 2.4G CCK TX DFIR coefficient reprogramming per channel range. */
  if (channelToSW >= 1 && channelToSW <= 11) {
    _device.phy_set_bb_reg(rCCK0_TxFilter1, bMaskDWord, 0x1a1b0030);
    _device.phy_set_bb_reg(rCCK0_TxFilter2, bMaskDWord, 0x090e1317);
    _device.phy_set_bb_reg(rCCK0_DebugPort, bMaskDWord, 0x00000204);
  } else if (channelToSW >= 12 && channelToSW <= 13) {
    _device.phy_set_bb_reg(rCCK0_TxFilter1, bMaskDWord, 0x1a1b0030);
    _device.phy_set_bb_reg(rCCK0_TxFilter2, bMaskDWord, 0x090e1217);
    _device.phy_set_bb_reg(rCCK0_DebugPort, bMaskDWord, 0x00000305);
  } else if (channelToSW == 14) {
    _device.phy_set_bb_reg(rCCK0_TxFilter1, bMaskDWord, 0x1a1b0030);
    _device.phy_set_bb_reg(rCCK0_TxFilter2, bMaskDWord, 0x00000E17);
    _device.phy_set_bb_reg(rCCK0_DebugPort, bMaskDWord, 0x00000000);
  }
}

void RadioManagementModule::phy_FixSpur_8812A(ChannelWidth_t Bandwidth,
                                              uint8_t Channel) {
  /* 8812-only — upstream's `PHY_FixSpur_8814A` is empty / nonexistent.
   * Returns early on 8814 even though the inner IS_C_CUT guard would
   * also skip on B-cut chips; defends against future cut-C 8814 silicon
   * incorrectly hitting the 8812-specific spur workaround. */
  if (_eepromManager->version_id.ICType != CHIP_8812) {
    return;
  }
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

/* Port of upstream `phy_SetBwMode8814A` (rtl8814a_phycfg.c:2182). 8814
 * BW post-config writes a much smaller set of BB regs than the 8812
 * path: it skips `rADC_Buf_Clk_Jaguar` (0x8C4 BIT30), `rL1PeakTH_Jaguar`
 * (0x848[25:22]), `rCCAonSec_Jaguar` (0xf0000000), and uses a narrower
 * `rRFMOD_Jaguar` mask. 8814 instead programs rRFMOD_Jaguar[1:0] and
 * 0x82C[15:12] via the already-ported `phy_SetBwRegAdc_8814A` /
 * `phy_SetBwRegAgc_8814A` helpers. Skipped here because they're
 * either A-cut-only no-ops (phy_ADC_CLK_8814A) or specific-40MHz-
 * channel workarounds (phy_SpurCalibration_8814A) that don't apply
 * to devourer's 20 MHz monitor use case. */
void RadioManagementModule::phy_PostSetBwMode8814A() {
  /* 0x668 BW write (REG_TRXPTCL_CTL_8814A == REG_WMAC_TRXPTCL_CTL,
   * same address). The existing 8812 helper does identical writes. */
  phy_SetRegBW_8812(_currentChannelBw);

  const auto SubChnlNum = phy_GetSecondaryChnl_8812();
  /* REG_DATA_SC_8814A and REG_DATA_SC_8812 are both 0x0483. */
  _device.rtw_write8(REG_DATA_SC_8812, SubChnlNum);

  phy_SetBwRegAdc_8814A(current_band_type, _currentChannelBw);
  phy_SetBwRegAgc_8814A(current_band_type, _currentChannelBw);

  switch (_currentChannelBw) {
  case ChannelWidth_t::CHANNEL_WIDTH_20:
    /* No extra writes for 20 MHz on 8814. */
    break;
  case ChannelWidth_t::CHANNEL_WIDTH_5:
  case ChannelWidth_t::CHANNEL_WIDTH_10: {
    /* 8814AU narrowband. The 8814 sets its BW ADC clock via 0x8ac[1:0]
     * (a 20/40/80 mode selector; phy_SetBwRegAdc_8814A), which has no sub-20
     * MHz value — BUT its phy_SetBwRegAdc comment documents the full field set
     * as [28, 21:20, 16, 9:6, 1:0], i.e. the 8814 shares the 8822B 0x8ac
     * layout (ADC clock [9:8]+[16], DAC clock [21:20]+[28]) rather than the
     * 8812A's ([9:8]/[21:20] only — dividing those alone did nothing here; the
     * [16]/[28] high bits dominate). Clear the [16]/[28] high bits and set the
     * low divide fields + small-BW; the codes are the 8821C/8822B narrowband
     * codes verbatim (10 MHz -> 3/3, 5 MHz -> 2/2). Bench-verified TX+RX both
     * directions. Sweepable via DEVOURER_NB_ADC / DEVOURER_NB_DAC. */
    const bool is5 = (_currentChannelBw == CHANNEL_WIDTH_5);
    uint32_t adc = is5 ? 2u : 3u;
    uint32_t dac = is5 ? 2u : 3u;
    const uint32_t smallbw = is5 ? 1u : 2u;
    if (_tuning.nb_adc)
      adc = *_tuning.nb_adc & 0x3;
    if (_tuning.nb_dac)
      dac = *_tuning.nb_dac & 0x3;
    /* mask [28]+[21:20]+[16]+[9:6]+[1:0]; clears the [28]/[16] clock high
     * bits, keeps [1:0]=0 (20M mode set above). */
    _device.phy_set_bb_reg(rRFMOD_Jaguar, 0x103103C3,
                           (dac << 20) | (adc << 8) | (smallbw << 6));
    _logger->info("SPIKE 8814AU narrowband: {} MHz (adc={} dac={} smallbw={})",
                  is5 ? 5 : 10, adc, dac, smallbw);
    break;
  }
  case ChannelWidth_t::CHANNEL_WIDTH_40:
    _device.phy_set_bb_reg(rRFMOD_Jaguar, 0x3C, SubChnlNum);
    if (SubChnlNum ==
        static_cast<uint8_t>(VHT_DATA_SC::VHT_DATA_SC_20_UPPER_OF_80MHZ)) {
      _device.phy_set_bb_reg(rCCK_System_Jaguar, bCCK_System_Jaguar, 1);
    } else {
      _device.phy_set_bb_reg(rCCK_System_Jaguar, bCCK_System_Jaguar, 0);
    }
    break;
  case ChannelWidth_t::CHANNEL_WIDTH_80:
    _device.phy_set_bb_reg(rRFMOD_Jaguar, 0x3C, SubChnlNum);
    break;
  default:
    _logger->error("phy_PostSetBwMode8814A: unknown Bandwidth {}",
                   static_cast<int>(_currentChannelBw));
    break;
  }

  /* RF[A/B/C/D] 0x18[11:10] BW bits — devourer's existing function
   * loops over `numTotalRfPath` (4 on 8814) with the same per-BW
   * values upstream `PHY_RF6052SetBandwidth8814A` uses, so it
   * already covers paths C/D. */
  PHY_RF6052SetBandwidth8812(_currentChannelBw);
}

void RadioManagementModule::phy_PostSetBwMode8812() {
  /* Per-chip BW post-config. 8814 has a separate sequence (no
   * rADC_Buf_Clk / rL1PeakTH / rCCAonSec writes, narrower
   * rRFMOD_Jaguar mask) — see `phy_PostSetBwMode8814A`. */
  if (_eepromManager->version_id.ICType == CHIP_8814A) {
    phy_PostSetBwMode8814A();
    return;
  }

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

  case CHANNEL_WIDTH_5:
  case CHANNEL_WIDTH_10: {
    /* SPIKE — 5/10 MHz narrowband on Jaguar1 (no vendor reference). The
     * 8812A shares the 8822B/8821C baseband clock block at 0x8ac: bits
     * [9:8] are the ADC clock (phy_FixSpur_8812A calls [9:8]=3 "ADC 160M",
     * [9:8]=2 "80M" — the normal 20 MHz value), [21:20] the DAC clock,
     * [7:6] the (8822B) small-BW field. Divide the ADC/DAC clocks down and
     * set small-BW to shrink the occupied bandwidth while the RF stays a
     * 20 MHz tune — exactly the Jaguar2/3 trick. The divide codes are
     * CHARACTERIZED on the 8812AU (SDR occupied-bandwidth grid + cross-RX vs
     * a Jaguar3 peer — tests/jaguar1_nb_divide_sweep.sh): an octave step off
     * the 20 MHz values (DAC 3 / ADC 2):
     *   10 MHz -> DAC 2, ADC 1   (TX lobe 8.2 MHz; RX best at ADC 1: 4700
     *                             vs 1900 hits at ADC 0)
     *    5 MHz -> DAC 1, ADC 0   (TX lobe 4.1 MHz)
     * The DAC code sets the emitted lobe width, the ADC code sets receive
     * sensitivity. Overridable via DEVOURER_NB_ADC / DEVOURER_NB_DAC. */
    /* 8812 die only — the 8821A's clock tree starves TX when the DAC clock
     * is divided (see RtlJaguarDevice::GetAdapterCaps). Fall back to a plain
     * 20 MHz baseband there rather than wedge the TX path. */
    if (_eepromManager->version_id.ICType != CHIP_8812) {
      _logger->warn("narrowband not supported on this Jaguar1 die — "
                    "applying 20 MHz baseband");
      _device.phy_set_bb_reg(rRFMOD_Jaguar, 0x003003C3, 0x00300200);
      _device.phy_set_bb_reg(rADC_Buf_Clk_Jaguar, BIT30, 0);
      break;
    }
    const bool is5 = (_currentChannelBw == CHANNEL_WIDTH_5);
    uint32_t adc = is5 ? 0u : 1u;
    uint32_t dac = is5 ? 1u : 2u;
    const uint32_t smallbw = is5 ? 1u : 2u;
    if (_tuning.nb_adc)
      adc = *_tuning.nb_adc & 0x3;
    if (_tuning.nb_dac)
      dac = *_tuning.nb_dac & 0x3;
    const uint32_t v8ac =
        (dac << 20) | (adc << 8) | (smallbw << 6) | 0x0u /* rf mode 20M */;
    _device.phy_set_bb_reg(rRFMOD_Jaguar, 0x003003C3, v8ac);
    _device.phy_set_bb_reg(rADC_Buf_Clk_Jaguar, BIT30, 0); /* 0x8c4[30]=0 */
    if (_eepromManager->numTotalRfPath >= 2)
      _device.phy_set_bb_reg(rL1PeakTH_Jaguar, 0x03C00000, 7);
    else
      _device.phy_set_bb_reg(rL1PeakTH_Jaguar, 0x03C00000, 8);
    _logger->info("SPIKE Jaguar1 narrowband: {} MHz — 0x8ac={:#010x} "
                  "(adc={} dac={} smallbw={})",
                  is5 ? 5 : 10, v8ac, adc, dac, smallbw);
    break;
  }

  default:
    _logger->error("phy_PostSetBWMode8812():	unknown Bandwidth: {}",
                   (int)_currentChannelBw);
    break;
  }

  /* <20121109, Kordan> A workaround for 8812A only. phy_FixSpur rewrites the
   * 0x8ac[9:8] ADC clock field — skip it for narrowband so it doesn't stomp
   * the divided clock. */
  if (_currentChannelBw != CHANNEL_WIDTH_5 &&
      _currentChannelBw != CHANNEL_WIDTH_10)
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
  case CHANNEL_WIDTH_5:  /* narrowband: MAC stays 20 MHz (BIT7=BIT8=0) */
  case CHANNEL_WIDTH_10:
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
  case CHANNEL_WIDTH_5:  /* narrowband: RF stays in its 20 MHz mode */
  case CHANNEL_WIDTH_10:
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
  const bool is_8814a = _eepromManager->version_id.ICType == CHIP_8814A;
  /* Fresh saturation snapshot for this apply pass: ComputeTxPowerIndex sets
   * the flags whenever the offset/override pushes any rate onto a 6-bit rail. */
  txpwr_sat_low_ = false;
  txpwr_sat_high_ = false;
  for (uint8_t path = 0; (uint8_t)path < _eepromManager->numTotalRfPath;
       path++) {
    phy_set_tx_power_level_by_path(Channel, (RfPath)path);
    /* TX power training is an 8812 mechanism: kernel
     * PHY_SetTxPowerLevel8814 (rtl8814a_phycfg.c:636) only loops
     * phy_set_tx_power_level_by_path — no training write exists anywhere
     * in the 8814 tree, and its BB table inits 0xC54/0xE54 to 0. Running
     * the 8812 trainee on 8814 wrote a non-zero word into 0xC54 and
     * collapsed paths B/C/D onto 0xE54 (last-writer-wins) on every
     * channel set. */
    if (!is_8814a) {
      PHY_TxPowerTrainingByPath_8812((RfPath)path);
    }
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
  /* T1 fix: per-rate TX power from EFUSE-derived tables (port of
   * upstream `PHY_GetTxPowerIndexBase`). Before this, every rate got
   * the same `power` (set once via SetTxPower) which produced the
   * uniform 0x28 across the 0xc20..0xc40 TX-AGC cluster and diverged
   * from kernel's per-rate per-channel values (0x2D..0x31 at ch6).
   *
   * Fallback when EFUSE tables not loaded (autoload failed for this
   * chip, e.g. 8814 pre-LateInit): the legacy `power` shortcut. Once
   * EFUSE is loaded all rates compute against base + per-Ntx diff. */
  /* ntx_idx is the rate's stream count - 1, NOT the chip's RfPath count.
   * Mirrors upstream `phy_get_current_tx_num`: OFDM/MCS0-7/VHT1SS → 0,
   * MCS8-15/VHT2SS → 1, MCS16-23/VHT3SS → 2, MCS24-31/VHT4SS → 3. */
  auto rate_ntx = [](uint8_t r) -> uint8_t {
    if (r >= 0x88 && r <= 0x8F) return 1; /* MCS8-15 */
    if (r >= 0x90 && r <= 0x97) return 2; /* MCS16-23 */
    if (r >= 0x98 && r <= 0x9F) return 3; /* MCS24-31 */
    if (r >= 0xAA && r <= 0xB3) return 1; /* VHT2SS */
    if (r >= 0xB4 && r <= 0xBD) return 2; /* VHT3SS */
    if (r >= 0xBE && r <= 0xC7) return 3; /* VHT4SS */
    return 0;
  };
  for (int i = 0; i < rates.size(); ++i) {
    MGN_RATE rate = rates[i];
    const uint32_t powerIndex = ComputeTxPowerIndex(
        static_cast<uint8_t>(rfPath), static_cast<uint8_t>(rate),
        rate_ntx(static_cast<uint8_t>(rate)));
    PHY_SetTxPowerIndex_8812A(powerIndex, rfPath, rate);
  }
}

uint8_t RadioManagementModule::ComputeTxPowerIndex(uint8_t path, uint8_t rate,
                                                   uint8_t ntx_idx) {
  int idx;
  if (txpwr_override_ >= 0) {
    /* Experiment override: force every rate to the same TXAGC index,
     * bypassing the EFUSE per-rate table. */
    idx = txpwr_override_;
  } else if (_eepromManager->TxPowerInfoLoaded) {
    idx = _eepromManager->GetTxPowerIndexBase(
        path, rate, ntx_idx, static_cast<uint8_t>(_currentChannelBw),
        _currentChannel);
  } else {
    idx = power;
  }
  /* Runtime offset folds AFTER the per-rate table (whose values arrive
   * regulatory-bounded from GetTxPowerIndexBase), inside the hardware clamp:
   * the calibrated per-rate shape is preserved until individual rates
   * saturate at a rail, and positive offsets get the full 6-bit headroom —
   * same permissiveness as the flat override. */
  idx += txpwr_offset_steps_;
  if (idx < 0) {
    idx = 0;
    txpwr_sat_low_ = true;
  }
  if (idx > 63) {
    idx = 63;
    txpwr_sat_high_ = true;
  }
  return static_cast<uint8_t>(idx);
}

void RadioManagementModule::PHY_SetTxPowerIndex_8812A(uint32_t powerIndex,
                                                      RfPath rfPath,
                                                      MGN_RATE rate) {

  DVR_DEBUG(_logger, "PHY_SetTxPowerIndex {} {} {}", powerIndex, (int)rfPath, rate);

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
  /* Upstream `rtl8812a_phycfg.c:629` — workaround for the 8812A/8821A
   * TEST CHIPS only, which had a bug accepting odd Tx-power indexes.
   * Normal-production silicon doesn't need it. Devourer was applying
   * the decrement unconditionally and introducing a systematic -1
   * offset in the per-rate TX-power canary diff vs kernel. */
  if (!IS_NORMAL_CHIP(_eepromManager->version_id) &&
      powerIndex % 2 == 1) {
    powerIndex -= 1;
  }
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
  DVR_DEBUG(_logger, "SET_TX_POWER {} - {} - {}", (int)rfPath, (int)channel,
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
  /* Upstream `PHY_TxPowerTrainingByPath_8812` uses
   * `phy_get_tx_power_index(adapter, path, MGN_MCS7, bw, channel)` as the
   * starting PowerLevel — i.e. the per-channel per-Ntx TX-power index for
   * HT MCS7 (1-stream). devourer used to read the uniform `power` class
   * member instead, which produced 0xc54 = 0x10161E vs kernel's 0x171D25
   * at ch6 (the T1 canary diff's last outstanding divergence in the
   * TX-power cluster). MGN_MCS7 = 0x87, ntx_idx = 0 (1-stream rate).
   *
   * ComputeTxPowerIndex is the same computation the per-rate apply loop
   * uses, so the flat override AND the runtime offset now reach the
   * training word too (previously the override was ignored here — the one
   * TX-power register the ramp didn't move). Signed arithmetic: an offset
   * can legitimately push the level below the -10/-8/-6 ladder, where the
   * old uint32_t underflowed past the >2 floor. */
  int powerLevel = ComputeTxPowerIndex(static_cast<uint8_t>(rfPath),
                                       /*rate MGN_MCS7=*/0x87, /*ntx_idx=*/0);

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
    writeData |= static_cast<uint32_t>((powerLevel > 2) ? powerLevel : 2)
                 << (i * 8);
  }

  _device.phy_set_bb_reg(writeOffset, 0xffffff, writeData);
}
