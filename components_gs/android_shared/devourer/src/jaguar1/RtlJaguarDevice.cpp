#include "RtlJaguarDevice.h"
#include "BeamformingSounder.h"
#include "ChannelFreq.h"
#include "EepromManager.h"
#include "Hal8812PhyReg.h"
#include "NhmReader.h"
#include "RadioManagementModule.h"
#include "SignalStop.h"
#include "ToneMask.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* comma-joined 0xNN hex dump for the DVR_TRACE TX-buffer dumps (argument is
 * only evaluated when trace is enabled). */
[[maybe_unused]] static std::string hex_join(const uint8_t *p, size_t n) {
  std::string s;
  s.reserve(n * 5);
  char b[8];
  for (size_t i = 0; i < n; i++) {
    std::snprintf(b, sizeof(b), "%s0x%02x", i ? "," : "", p[i]);
    s += b;
  }
  return s;
}

/* Per-queue free-page registers (8814A only — 0x0230..0x0240 don't decode
 * the same way on 8812/8821). Cross-checked against hal/rtl8814a_spec.h
 * and src/HalModule.cpp's REG_FIFOPAGE_INFO_*_8814A constants. */
static constexpr uint16_t kFifoPageInfoRegs_8814A[5] = {
    0x0230, 0x0234, 0x0238, 0x023C, 0x0240,
};

/* RF LO-enable register (vendor `lna_low_gain_3`) — RF 0x58 bit1 gates the
 * bare local-oscillator carrier for MP single-tone. Not aliased in devourer's
 * hal/ headers, so named here. */
static constexpr uint16_t RF_LNA_LOW_GAIN_3 = 0x58;

/* 8814A path-C / path-D TX-scale BB registers (rC/rD_TxScale_Jaguar2). Only
 * defined in hal/Hal8814PhyReg.h, which can't be included here without clashing
 * with Hal8812PhyReg.h's overlapping Jaguar symbols — so named locally, the
 * same pattern RadioManagementModule uses for the 8814 path-C/D offsets. */
static constexpr uint16_t rC_TxScale_8814 = 0x181C;
static constexpr uint16_t rD_TxScale_8814 = 0x1A1C;

RtlJaguarDevice::RtlJaguarDevice(RtlAdapter device, Logger_t logger,
                                 devourer::DeviceConfig cfg)
    : _cfg{std::move(cfg)},
      _eepromManager{std::make_shared<EepromManager>(device, logger, _cfg)},
      _radioManagement{std::make_shared<RadioManagementModule>(
          device, _eepromManager, logger, _cfg)},
      _device{device},
      _halModule{device, _eepromManager, _radioManagement, logger, _cfg},
      _logger{logger} {}

void RtlJaguarDevice::InitWrite(SelectedChannel channel) {
  StartWithMonitorMode(channel);
  SetMonitorChannel(channel);
  _logger->info("In Monitor Mode");

  /* DEVOURER_XTAL_CAP — crystal-cap trim (issue #217, narrowband CFO lever). */
  if (_cfg.tuning.xtal_cap)
    SetXtalCap(*_cfg.tuning.xtal_cap);

  /* DEVOURER_BF_ARM_SOUNDER=1 — beamforming self-sounding probe (beamformer
   * side): arm the MAC's hardware sounding engine so a TX-descriptor-marked
   * NDPA (DEVOURER_TX_NDPA=1) is followed by a hardware-generated NDP. See
   * BeamformingSounder.h for the vendor register recipe. */
  if (_cfg.bf.arm_sounder || _cfg.bf.sounder_self_mac) {
    devourer::bf::arm_sounder(_device);
    _logger->info("BF sounder armed (beamformer side)");
  }

  /* DEVOURER_CW_TONE — radiate a bare RF LO carrier at the channel center
   * (MP single-tone). The channel is already tuned above, so the LO sits at the
   * center frequency. DEVOURER_CW_TONE_GAIN=0..31 sets RF 0x00[4:0]. */
  if (_cfg.tx.cw_tone)
    StartCwTone(_cfg.tx.cw_tone_gain & 0x1F);
}

/* MP single-tone (CW carrier), Jaguar-1 path A. The RF writes are common to the
 * whole family (path A -> TX mode + gain + LO enable); the baseband setup that
 * keys the TX path differs by chip, so the two families branch:
 *   8812/8821 — hal_mpt_SetSingleToneTx() JAGUAR branch: OFDM/CCK modulators
 *               off + RFE pinmux forced to TX (+ ext-PA pins).
 *   8814      — mpt_SetSingleTone_8814A(): CCA off + per-path TX-scale zeroed.
 * The pre-tone RF/BB state is snapshotted for StopCwTone(). */
void RtlJaguarDevice::StartCwTone(uint8_t gain) {
  if (_cw_active)
    return;

  const bool is8814 = _eepromManager->version_id.ICType == CHIP_8814A;

  /* RF 0x00 is a 20-bit register — snapshot the full width for a clean restore
   * (bLSSIWrite_data_Jaguar mask). */
  _cw_rf00 = _radioManagement->phy_query_rf_reg(RfPath::RF_PATH_A, RF_AC_Jaguar,
                                                bLSSIWrite_data_Jaguar);

  /* Common RF path-A key-up: TX mode (0x00[19:16]=2), gain index (0x00[4:0]),
   * LO enable (0x58 bit1) — the bare local-oscillator carrier. */
  auto key_rf_path_a = [&] {
    _radioManagement->phy_set_rf_reg(RfPath::RF_PATH_A, RF_AC_Jaguar, 0xF0000,
                                     0x2);
    _radioManagement->phy_set_rf_reg(RfPath::RF_PATH_A, RF_AC_Jaguar, 0x1F,
                                     gain & 0x1F);
    _radioManagement->phy_set_rf_reg(RfPath::RF_PATH_A, RF_LNA_LOW_GAIN_3, BIT1,
                                     0x1);
  };

  if (is8814) {
    /* Snapshot the four per-path TX-scale words. */
    _cw_bb[0] = _radioManagement->phy_query_bb_reg_public(rA_TxScale_Jaguar,
                                                          bMaskDWord);
    _cw_bb[1] = _radioManagement->phy_query_bb_reg_public(rB_TxScale_Jaguar,
                                                          bMaskDWord);
    _cw_bb[2] =
        _radioManagement->phy_query_bb_reg_public(rC_TxScale_8814, bMaskDWord);
    _cw_bb[3] =
        _radioManagement->phy_query_bb_reg_public(rD_TxScale_8814, bMaskDWord);

    /* Disable secondary CCA (0x838 bit1). */
    _device.phy_set_bb_reg(rCCAonSec_Jaguar, BIT1, 0x1);

    key_rf_path_a();

    /* Zero the TX-scale [31:21] on all four paths (kills any residual digital
     * signal; the bare LO still radiates). */
    _device.phy_set_bb_reg(rA_TxScale_Jaguar, 0xFFE00000, 0x0);
    _device.phy_set_bb_reg(rB_TxScale_Jaguar, 0xFFE00000, 0x0);
    _device.phy_set_bb_reg(rC_TxScale_8814, 0xFFE00000, 0x0);
    _device.phy_set_bb_reg(rD_TxScale_8814, 0xFFE00000, 0x0);
  } else {
    /* Snapshot the RFE-pinmux words (0xCB0/0xEB0 and their +4 siblings). */
    _cw_bb[0] = _radioManagement->phy_query_bb_reg_public(rA_RFE_Pinmux_Jaguar,
                                                          bMaskDWord);
    _cw_bb[1] = _radioManagement->phy_query_bb_reg_public(rB_RFE_Pinmux_Jaguar,
                                                          bMaskDWord);
    _cw_bb[2] = _radioManagement->phy_query_bb_reg_public(
        rA_RFE_Pinmux_Jaguar + 4, bMaskDWord);
    _cw_bb[3] = _radioManagement->phy_query_bb_reg_public(
        rB_RFE_Pinmux_Jaguar + 4, bMaskDWord);

    /* Disable OFDM + CCK modulators (0x808[29:28] = 0). */
    _device.phy_set_bb_reg(rOFDMCCKEN_Jaguar, BIT29 | BIT28, 0x0);

    key_rf_path_a();

    /* RFE pinmux to force the TX path. */
    _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, 0xFF00F0, 0x77007);
    _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, 0xFF00F0, 0x77007);

    /* External-PA parts only: enable the ext-PA pin (vendor priority — 5G flag
     * takes 0x12, else 2G flag takes 0x11). Parts with no ext PA skip this. */
    if (_eepromManager->GetExternalPa5G()) {
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar + 4, 0xFF00000, 0x12);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar + 4, 0xFF00000, 0x12);
    } else if (_eepromManager->ExternalPA_2G) {
      _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar + 4, 0xFF00000, 0x11);
      _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar + 4, 0xFF00000, 0x11);
    }
  }

  _cw_active = true;
  _logger->info("CW single-tone armed @ ch{} gain={} ({})", _channel.Channel,
                static_cast<int>(gain & 0x1F), is8814 ? "8814A" : "8812/8821");
}

/* Mirror of the vendor STOP path: undo the baseband key-up, restore RF 0x00 and
 * the four BB words captured in StartCwTone(), and disable the LO. */
void RtlJaguarDevice::StopCwTone() {
  if (!_cw_active)
    return;

  const bool is8814 = _eepromManager->version_id.ICType == CHIP_8814A;

  if (is8814) {
    _radioManagement->phy_set_rf_reg(RfPath::RF_PATH_A, RF_LNA_LOW_GAIN_3, BIT1,
                                     0x0);
    _radioManagement->phy_set_rf_reg(RfPath::RF_PATH_A, RF_AC_Jaguar,
                                     bLSSIWrite_data_Jaguar, _cw_rf00);
    _device.phy_set_bb_reg(rCCAonSec_Jaguar, BIT1, 0x0); /* re-enable CCA */
    _device.phy_set_bb_reg(rA_TxScale_Jaguar, bMaskDWord, _cw_bb[0]);
    _device.phy_set_bb_reg(rB_TxScale_Jaguar, bMaskDWord, _cw_bb[1]);
    _device.phy_set_bb_reg(rC_TxScale_8814, bMaskDWord, _cw_bb[2]);
    _device.phy_set_bb_reg(rD_TxScale_8814, bMaskDWord, _cw_bb[3]);
  } else {
    _device.phy_set_bb_reg(rOFDMCCKEN_Jaguar, BIT29 | BIT28, 0x3);
    _radioManagement->phy_set_rf_reg(RfPath::RF_PATH_A, RF_AC_Jaguar,
                                     bLSSIWrite_data_Jaguar, _cw_rf00);
    _radioManagement->phy_set_rf_reg(RfPath::RF_PATH_A, RF_LNA_LOW_GAIN_3, BIT1,
                                     0x0);
    _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar, bMaskDWord, _cw_bb[0]);
    _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar, bMaskDWord, _cw_bb[1]);
    _device.phy_set_bb_reg(rA_RFE_Pinmux_Jaguar + 4, bMaskDWord, _cw_bb[2]);
    _device.phy_set_bb_reg(rB_RFE_Pinmux_Jaguar + 4, bMaskDWord, _cw_bb[3]);
  }

  _cw_active = false;
  _logger->info("CW single-tone stopped — chip restored");
}

/* Modulated continuous TX — vendor mpt_StartOfdmContTx (hal_mp.c). Unlike the
 * CW tone this leaves the OFDM modulator + scrambler running and drives the
 * normal TX pipeline; the caller feeds frames with send_packet and the chip
 * holds a continuous modulated carrier. Rate comes from `mode` (SetTxMode);
 * per-rate TXAGC/power is the normal path (SetTxPowerOverride/efuse). */
void RtlJaguarDevice::StartContinuousTx(const devourer::TxMode &mode) {
  if (_cont_active)
    return;
  SetTxMode(mode);

  /* OFDM block on. */
  if (!(_radioManagement->phy_query_bb_reg_public(rFPGA0_RFMOD, bOFDMEn)))
    _device.phy_set_bb_reg(rFPGA0_RFMOD, bOFDMEn, 1);
  /* CCK test mode off, scrambler on. */
  _device.phy_set_bb_reg(rCCK0_System, bCCKBBMode, 0);
  _device.phy_set_bb_reg(rCCK0_System, bCCKScramble, 1);
  /* Continuous-TX mode bits 0x914[18:16] = OFDM_ContinuousTx (1). */
  _device.phy_set_bb_reg(rSingleTone_ContTx_Jaguar, BIT18 | BIT17 | BIT16, 0x1);
  /* HSSI params the vendor sets for the continuous stream. */
  _device.phy_set_bb_reg(rFPGA0_XA_HSSIParameter1, bMaskDWord, 0x01000500);
  _device.phy_set_bb_reg(rFPGA0_XB_HSSIParameter1, bMaskDWord, 0x01000500);

  _cont_active = true;
  _logger->info("Modulated continuous TX armed @ ch{} (feed frames via "
                "send_packet; StopContinuousTx to end)",
                _channel.Channel);
}

/* Mirror of the vendor mpt_StopOfdmContTx: clear the continuous-TX mode bits,
 * settle, pulse a BB reset, restore the HSSI params. */
void RtlJaguarDevice::StopContinuousTx() {
  if (!_cont_active)
    return;
  _device.phy_set_bb_reg(rSingleTone_ContTx_Jaguar, BIT18 | BIT17 | BIT16, 0x0);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  /* BB reset pulse. */
  _device.phy_set_bb_reg(rPMAC_Reset, bBBResetB, 0x0);
  _device.phy_set_bb_reg(rPMAC_Reset, bBBResetB, 0x1);
  _device.phy_set_bb_reg(rFPGA0_XA_HSSIParameter1, bMaskDWord, 0x01000100);
  _device.phy_set_bb_reg(rFPGA0_XB_HSSIParameter1, bMaskDWord, 0x01000100);

  _cont_active = false;
  _logger->info("Modulated continuous TX stopped — chip restored");
}

/* Frame-free RX energy snapshot for the AC (Jaguar-1) BB. Reads the phydm
 * OFDM/CCK false-alarm (0xF48/0xA5C) + CCA (0xF08) counters and the DIG IGI
 * (0xC50), then resets the counters (phydm_false_alarm_counter_reg_reset AC:
 * 0x9a4[17], 0xa2c[15], 0xb58[0]) so the next call sees only the delta. Same
 * register set PhydmWatchdog::ReadFaCountersAc uses. */
RxEnergy RtlJaguarDevice::GetRxEnergy() {
  RxEnergy e;
  auto bb = [this](uint16_t addr) {
    return _radioManagement->phy_query_bb_reg_public(addr, 0xFFFFFFFF);
  };
  e.fa_ofdm = bb(0x0F48) & 0xFFFF;
  e.fa_cck = bb(0x0A5C) & 0xFFFF;
  const uint32_t cca = bb(0x0F08);
  e.cca_ofdm = (cca >> 16) & 0xFFFF;
  e.cca_cck = cca & 0xFFFF;
  e.valid_fa = true;
  e.igi = static_cast<uint8_t>(_device.rtw_read8(0x0C50) & 0x7F);
  e.valid_igi = true;

  /* Reset the FA/CCA counter latches (read-then-reset = per-call delta). */
  _device.phy_set_bb_reg(0x09A4, 1u << 17, 1);
  _device.phy_set_bb_reg(0x09A4, 1u << 17, 0);
  _device.phy_set_bb_reg(0x0A2C, 1u << 15, 0);
  _device.phy_set_bb_reg(0x0A2C, 1u << 15, 1);
  _device.phy_set_bb_reg(0x0B58, 1u << 0, 1);
  _device.phy_set_bb_reg(0x0B58, 1u << 0, 0);

  /* NHM 12-bucket power histogram (frame-free, 11AC register map). */
  devourer::read_nhm(
      devourer::nhm_regs_11ac(), e.igi,
      [this](uint16_t a) { return _device.rtw_read<uint32_t>(a); },
      [this](uint16_t a, uint32_t m, uint32_t v) {
        _device.phy_set_bb_reg(a, m, v);
      },
      e);
  return e;
}

SelectedChannel RtlJaguarDevice::GetSelectedChannel() { return _channel; }

uint64_t RtlJaguarDevice::ReadTsf() {
  /* REG_TSFTR (0x0560) = TSF low 32, 0x0564 = TSF high 32. Read hi, lo, hi
   * again and retry the pair once if the low word wrapped between the reads. */
  uint32_t hi = _device.rtw_read<uint32_t>(0x0564);
  uint32_t lo = _device.rtw_read<uint32_t>(0x0560);
  if (_device.rtw_read<uint32_t>(0x0564) != hi) {
    hi = _device.rtw_read<uint32_t>(0x0564);
    lo = _device.rtw_read<uint32_t>(0x0560);
  }
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

bool RtlJaguarDevice::send_packet(const uint8_t *packet, size_t length) {
  struct tx_desc *ptxdesc;
  bool resp;
  uint8_t *usb_frame;
  int real_packet_length, usb_frame_length, radiotap_length;

  bool vht = false;
  int ret = 0;
  int qos_len = 0;
  u8 fixed_rate = MGN_1M, sgi = 0, bwidth = 0, ldpc = 0, stbc = 0;
  u16 txflags = 0;
  int rate_id = 0;
  /* True once the radiotap carries a rate (RATE / HT-MCS-index / VHT). When it
   * stays false the device TX-mode default (SetTxMode) — else MGN_1M — applies. */
  bool rate_from_radiotap = false;
  /* Per-packet hop target from a radiotap CHANNEL field (0 = none present). */
  int radiotap_channel = 0;
  if (length < sizeof(struct ieee80211_radiotap_header)) {
    return false;
  }
  radiotap_length = get_unaligned_le16(packet + 2);
  if (radiotap_length == 0 || (size_t)radiotap_length >= length) {
    return false;
  }
  real_packet_length = length - radiotap_length;

  if (radiotap_length != 0x0d)
    vht = true;

  usb_frame_length = real_packet_length + TXDESC_SIZE;

  DVR_DEBUG(_logger, "radiotap length is {}, 80211 length is {}, usb_frame length "
                "should be {}",
                radiotap_length, real_packet_length, usb_frame_length);

  struct ieee80211_radiotap_header *rtap_hdr;
  rtap_hdr = (struct ieee80211_radiotap_header *)packet;
  struct ieee80211_radiotap_iterator iterator;
  ret = ieee80211_radiotap_iterator_init(&iterator, rtap_hdr, radiotap_length,
                                         NULL);
  while (!ret) {
    ret = ieee80211_radiotap_iterator_next(&iterator);

    if (ret)
      continue;

    /* see if this argument is something we can use */
    switch (iterator.this_arg_index) {

    case IEEE80211_RADIOTAP_RATE:
      fixed_rate = *iterator.this_arg;
      rate_from_radiotap = true;
      break;

    case IEEE80211_RADIOTAP_TX_FLAGS:
      txflags = get_unaligned_le16(iterator.this_arg);
      break;

    case IEEE80211_RADIOTAP_CHANNEL:
      /* 2 x __le16: frequency (MHz), then flags. Frequency is authoritative
       * for the per-packet hop target; flags are ignored (rate/BW come from
       * the RATE/MCS/VHT fields). */
      radiotap_channel =
          devourer::freq_to_chan(get_unaligned_le16(iterator.this_arg));
      break;

    case IEEE80211_RADIOTAP_MCS: {
      u8 mcs_known = iterator.this_arg[0];
      u8 mcs_flags = iterator.this_arg[1];

      uint8_t mcs_bw_field = mcs_flags & IEEE80211_RADIOTAP_MCS_BW_MASK;
      if (mcs_bw_field == IEEE80211_RADIOTAP_MCS_BW_40) {
        bwidth = CHANNEL_WIDTH_40;
      } else if (mcs_bw_field == IEEE80211_RADIOTAP_MCS_BW_20 ||
                 mcs_bw_field == IEEE80211_RADIOTAP_MCS_BW_20L ||
                 mcs_bw_field == IEEE80211_RADIOTAP_MCS_BW_20U) {
        bwidth = CHANNEL_WIDTH_20;
      }

      if (mcs_flags & 0x04) {
        sgi = 1;
      } else {
        sgi = 0;
      }

      /* STBC (radiotap MCS known bit5 / flags bits5-6) and FEC=LDPC (known bit4 /
       * flags bit4). The HT branch previously read neither, so an HT frame tagged
       * STBC/LDPC in radiotap silently transmitted as BCC SISO -- only the VHT
       * branch honoured them. Reading them here lets txdemo emit a real
       * HT STBC / HT LDPC frame (needed as a chip reference for the gr-ieee802-11
       * fork's modern-format TX). */
      if (mcs_known & 0x20) {
        stbc = (mcs_flags >> 5) & 0x3;
      }
      if ((mcs_known & 0x10) && (mcs_flags & 0x10)) {
        ldpc = 1;
      }

      /* The radiotap is authoritative for per-packet rate: honour the HT MCS
       * index from byte 2 unconditionally. (Previously gated behind the
       * DEVOURER_TX_HT_MCS env var, so a valid HT radiotap silently fell back
       * to 1M CCK — now replaced by the programmatic SetTxMode default, applied
       * after this loop only when no rate is present in the radiotap.) */
      if (mcs_known & IEEE80211_RADIOTAP_MCS_HAVE_MCS) {
        uint8_t mcs_index = iterator.this_arg[2];
        if (mcs_index <= 31) {
          fixed_rate = MGN_MCS0 + mcs_index;
          rate_from_radiotap = true;
        }
      }
    } break;

    case IEEE80211_RADIOTAP_VHT: {
      u8 known = iterator.this_arg[0];
      u8 flags = iterator.this_arg[2];
      unsigned int mcs, nss;
      if ((known & 4) && (flags & 4))
        sgi = 1;
      if ((known & 1) && (flags & 1))
        stbc = 1;
      if (known & 0x40) {
        auto bw = iterator.this_arg[3] & 0x1f;
        /* Map radiotap VHT bandwidth codes to CHANNEL_WIDTH enums — the
         * descriptor BW switch below compares against the enums (as the
         * HT path above does). The previous MHz literals (40/80) never
         * matched CHANNEL_WIDTH_40(1)/CHANNEL_WIDTH_80(2), so VHT 40/80
         * silently transmitted as 20MHz. */
        if (bw >= 1 && bw <= 3)
          bwidth = CHANNEL_WIDTH_40;
        else if (bw >= 4 && bw <= 10)
          bwidth = CHANNEL_WIDTH_80;
        else
          bwidth = CHANNEL_WIDTH_20;
      }

      if (iterator.this_arg[8] & 1)
        ldpc = 1;
      mcs = (iterator.this_arg[4] >> 4) & 0x0f;
      nss = iterator.this_arg[4] & 0x0f;
      if (nss > 0) {
        if (nss > 4)
          nss = 4;
        if (mcs > 9)
          mcs = 9;
        fixed_rate = MGN_VHT1SS_MCS0 + ((nss - 1) * 10 + mcs);
        rate_from_radiotap = true;
      }
    } break;

    default:
      break;
    }
  }

  /* Radiotap CHANNEL is authoritative for per-packet frequency, exactly as
   * RATE/MCS are for rate: a frame asking for a different channel triggers a
   * lean FastRetune before TX (intra-band 20 MHz ~1.6 ms; cross-band/non-20MHz
   * falls back to the full path). A no-op when it equals the current channel,
   * so an informational CHANNEL field costs nothing. This makes frequency
   * hopping radiotap-driven — any caller can hop per-packet without an env knob
   * — at the honest cost of stalling this send by the retune latency. Done
   * before the 5 GHz CCK clamp below so the clamp keys off the new channel. */
  if (radiotap_channel > 0 && radiotap_channel != _channel.Channel) {
    FastRetune(static_cast<uint8_t>(radiotap_channel), /*cache_rf=*/true);
  }

  /* The radiotap carried no rate → apply the runtime TX-mode default set via
   * SetTxMode (modulation / MCS / BW / GI / FEC / STBC). With no default set,
   * the MGN_1M fallback from above stands. Per-packet radiotap always wins. */
  if (!rate_from_radiotap && _tx_mode_default.has_value()) {
    const devourer::TxParams tp =
        devourer::tx_mode_to_params(*_tx_mode_default);
    fixed_rate = tp.fixed_rate;
    vht = tp.vht;
    sgi = tp.sgi;
    ldpc = tp.ldpc;
    stbc = tp.stbc;
    bwidth = static_cast<u8>(tp.bwidth);
  }

  /* CCK rates (1/2/5.5/11M) do not exist at 5GHz. The RTL8814AU silently
   * drops a CCK-rated frame on a 5GHz channel — the bulk-OUT completes but
   * nothing goes on-air (verified on hardware: default MGN_1M beacon = 0
   * frames at ch36/ch100, but ~14k on-air once the rate is OFDM). 2.4GHz
   * CCK is fine. So on a 5GHz channel, clamp a CCK rate to the lowest OFDM
   * rate. (The 8812 chip happens to auto-fall-back CCK->OFDM at 5G; the
   * 8814 does not, so we must do it in software.) */
  if (_channel.Channel > 14 &&
      (fixed_rate == MGN_1M || fixed_rate == MGN_2M ||
       fixed_rate == MGN_5_5M || fixed_rate == MGN_11M)) {
    fixed_rate = MGN_6M;
  }

  usb_frame = new uint8_t[usb_frame_length]();

  ptxdesc = (struct tx_desc *)usb_frame;

  /* Drop an STBC request the chip can't honour: STBC needs >=2 TX chains, so a
   * 1T1R part (8811AU/8821AU) that airs an STBC-marked frame produces a
   * malformed PPDU that never decodes (a known adaptive-link footgun). Warn
   * once; leave 2T2R/4T4R untouched. */
  if (stbc && !GetTxCaps().stbc_ok) {
    static bool warned = false;
    if (!warned) {
      _logger->warn("STBC requested but this chip is 1T1R (no STBC) — dropping "
                    "the STBC flag to keep frames decodable");
      warned = true;
    }
    stbc = 0;
  }
  DVR_DEBUG(_logger, "fixed rate:{}, sgi:{}, radiotap_bwidth:{}, ldpc:{}, stbc:{}",
                (int)fixed_rate, (int)sgi, (int)bwidth, (int)ldpc, (int)stbc);

  uint8_t BWSettingOfDesc;
  if (bwidth == CHANNEL_WIDTH_40) {
    BWSettingOfDesc = 1;
  } else if (bwidth == CHANNEL_WIDTH_80) {
    BWSettingOfDesc = 2;
  } else {
    BWSettingOfDesc = 0;
  }
  DVR_DEBUG(_logger, "TX DESC BW decision: _channel.ChannelWidth(RX)={}, radiotap_bwidth(TX)={}, BWSettingOfDesc(TX_DESC)={}",
                (int)_channel.ChannelWidth, (int)bwidth, (int)BWSettingOfDesc);

  SET_TX_DESC_DATA_BW_8812(usb_frame, BWSettingOfDesc);

  /* The SET_TX_DESC_*_8812 macros have bit-identical positions to the
   * SET_TX_DESC_*_8814A macros (verified against hal/rtl8814a_xmit.h). But
   * a few of the field NAMES differ on 8814A, and a usbmon byte-diff
   * against a working VM-passthrough 88XXau monitor-injection session shows
   * three field-value mismatches on 8814A:
   *
   *   Dword 0 bit 31 — 8812 calls it OWN, 8814A calls it DISQSELSEQ.
   *     88XXau leaves bit 31 = 0 for monitor-injected frames; devourer's
   *     SET_TX_DESC_OWN_8812(usb_frame, 1) sets it to 1, which on 8814A
   *     means DISQSELSEQ=1 (disable queue-select-based sequence numbering).
   *   Dword 2 bits 24-29 (GID) — 88XXau leaves at 0 for injection;
   *     devourer writes 0x3F.
   *   Dword 4 bits 18-23 (DATA_RETRY_LIMIT) — 88XXau leaves at 0 for
   *     injection; devourer writes 12.
   *
   * Skip those writes on 8814A to byte-match aircrack-ng's reference
   * monitor-injection descriptor. Does NOT resolve #50 (on-air silence
   * has a different root cause that vendor-control-write replay can't
   * reach), but aligns devourer's TX descriptor with the working
   * kernel-driver format. Override with DEVOURER_TX_LEGACY_8812_DESC=1
   * to restore the old behaviour without rebuilding.
   *
   * 8812AU and 8821AU paths are bit-for-bit identical to current master --
   * is_8814a is false there and all writes fire as before. */
  const bool is_8814a =
      _eepromManager->version_id.ICType == CHIP_8814A &&
      !_cfg.tx.legacy_8812_desc;

  /* Single-fragment frame: LAST_SEG=1 (no FIRST_SEG). */
  SET_TX_DESC_LAST_SEG_8812(usb_frame, 1);
  if (!is_8814a) {
    /* OWN=1 needed on 8812/8821 so chip processes the descriptor. On
     * 8814A the same bit is DISQSELSEQ -- leave at 0 to match 88XXau. */
    SET_TX_DESC_OWN_8812(usb_frame, 1);
  }

  SET_TX_DESC_PKT_SIZE_8812(usb_frame,
                            static_cast<uint32_t>(real_packet_length));

  SET_TX_DESC_OFFSET_8812(usb_frame,
                          static_cast<uint8_t>(TXDESC_SIZE + OFFSET_SZ));

  /* Match kernel-driver TX descriptor field-for-field. Verified by
   * usbmon capture of working kernel-driver TX in monitor mode +
   * byte-for-byte diff. Previous values were based on speculative
   * comments; the chip silently dropped frames whose descriptor didn't
   * match the kernel's. */
  SET_TX_DESC_MACID_8812(usb_frame, static_cast<uint8_t>(0x01));

  if (!vht) {
    rate_id = 8;
  } else {
    rate_id = 9;
  }

  SET_TX_DESC_BMC_8812(usb_frame, 1);
  SET_TX_DESC_RATE_ID_8812(usb_frame, static_cast<uint8_t>(rate_id));

  SET_TX_DESC_QUEUE_SEL_8812(usb_frame, 0x12);
  SET_TX_DESC_HWSEQ_EN_8812(usb_frame, static_cast<uint8_t>(1));
  if (!is_8814a) {
    /* 88XXau leaves GID=0 for monitor injection on 8814A. */
    SET_TX_DESC_GID_8812(usb_frame, static_cast<uint8_t>(0x3F));
  }
  SET_TX_DESC_SW_DEFINE_8812(usb_frame, static_cast<uint16_t>(0x001));
  SET_TX_DESC_RETRY_LIMIT_ENABLE_8812(usb_frame, 1);
  if (!is_8814a) {
    /* 88XXau leaves DATA_RETRY_LIMIT=0 for monitor injection on 8814A
     * (RETRY_LIMIT_ENABLE stays set to 1 in both). */
    SET_TX_DESC_DATA_RETRY_LIMIT_8812(usb_frame, 12);
  }
  if (sgi) {
    _logger->info("short gi enabled,set sgi");
    SET_TX_DESC_DATA_SHORT_8812(usb_frame, 1);
  }
  SET_TX_DESC_USE_RATE_8812(usb_frame, 1);
  SET_TX_DESC_TX_RATE_8812(usb_frame,
                           static_cast<uint8_t>(MRateToHwRate(
                               fixed_rate)));

  if (ldpc) {
    SET_TX_DESC_DATA_LDPC_8812(usb_frame, ldpc);
  }

  SET_TX_DESC_DATA_STBC_8812(usb_frame, stbc & 3);

  /* DEVOURER_TX_NDPA=1 — beamforming self-sounding probe: mark the injected
   * frame as an NDPA (TX-desc Dword3 [23:22]=1) so the MAC sounding engine
   * follows it with a hardware-generated NDP (pairs with
   * DEVOURER_BF_ARM_SOUNDER + the txdemo NDPA builder DEVOURER_TX_NDPA_RA).
   * The descriptor layout is Jaguar-1-specific, so this stays in the HAL
   * (the shared BeamformingSounder.h only holds the generation-neutral MAC
   * register recipe). NDPA is a unicast control frame: no HW sequence stamp,
   * not broadcast, use-header NAV, no rate fallback. */
  if (_cfg.bf.ndpa_period > 0) {
    SET_TX_DESC_NDPA_8812(usb_frame, 1);
    SET_TX_DESC_HWSEQ_EN_8812(usb_frame, 0);
    SET_TX_DESC_BMC_8812(usb_frame, 0);
    SET_TX_DESC_NAV_USE_HDR_8812(usb_frame, 1);
    SET_TX_DESC_DISABLE_FB_8812(usb_frame, 1);
  }

  rtl8812a_cal_txdesc_chksum(usb_frame);
  DVR_TRACE(_logger, "tx desc formed: {}",
            hex_join(usb_frame, usb_frame_length));
  uint8_t *addr = usb_frame + TXDESC_SIZE;
  memcpy(addr, packet + radiotap_length, real_packet_length);
  DVR_TRACE(_logger, "packet formed: {}",
            hex_join(usb_frame, usb_frame_length));
  resp = _device.send_packet(usb_frame, usb_frame_length);
  delete[] usb_frame;

  return resp;
}

void RtlJaguarDevice::Init(Action_ParsedRadioPacket packetProcessor,
                          SelectedChannel channel) {
  StartWithMonitorMode(channel);
  SetMonitorChannel(channel);

  /* DEVOURER_XTAL_CAP — crystal-cap trim (issue #217, narrowband CFO lever). */
  if (_cfg.tuning.xtal_cap)
    SetXtalCap(*_cfg.tuning.xtal_cap);

  /* DEVOURER_BF_ARM_BFEE=aa:bb:cc:dd:ee:ff — beamforming self-sounding probe
   * (beamformee side): arm the hardware CSI responder so an NDPA+NDP from the
   * given beamformer MAC triggers a hardware-built VHT Compressed Beamforming
   * report, with NO association. The chip's own MAC (0x610, from EFUSE) is the
   * NDPA RA match. See BeamformingSounder.h for the register recipe. */
  if (_cfg.bf.beamformee_of) {
    const uint8_t *mac = _cfg.bf.beamformee_of->data();
    devourer::bf::arm_beamformee(_device, mac, devourer::bf::kBfeeJaguar1);
    _logger->info("BF beamformee armed for beamformer "
                  "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }

  StartRxLoop(std::move(packetProcessor));
}

void RtlJaguarDevice::StartRxLoop(Action_ParsedRadioPacket packetProcessor) {
  _packetProcessor = std::move(packetProcessor);
  /* Restartable: clear any stop request left by a prior StopRxLoop(). */
  should_stop = false;

  /* DEVOURER_RX_PATHS=0xNN restricts which RX chains the chip enables/combines,
   * by masking the RX-path-enable register (0x808 byte 0: bits 0/4 = path A
   * CCK/OFDM, 1/5 = B, 2/6 = C, 3/7 = D). Default is all paths (0xFF, from the
   * BB table). This is the knob for measuring the chip's hardware maximal-ratio
   * combining gain: capture per-chain metrics (DEVOURER_RX_ALLPATHS) at 0x11
   * (A only), 0x33 (A+B), 0x77 (A+B+C), 0xFF (all) and compare frame delivery
   * at a marginal link. Written after SetMonitorChannel so it is the final word
   * (IQK saves/restores 0x808); a later channel switch reverts it to the table
   * default, so it targets a single-channel RX capture. The 8814 has 4 paths;
   * on 8812/8821 the high bits are no-ops. */
  /* A toggle spec `0xAA:0xBB[:0xCC]@<ms>` cycles the mask on a timer thread
   * instead of setting it once — the stimulus for the mobile/fading combining
   * measurement: alternate a fixed single chain vs all-4 fast relative to the
   * operator's motion, so both configs sample the same fading process. Each
   * switch emits an `rx.path_mask` event inline with the frame
   * stream so the analyser can tag each frame with the active mask. A plain
   * `0xNN` applies once, as before. */
  if (_cfg.rx.path_spec) {
    const std::string &spec = *_cfg.rx.path_spec;
    auto at = spec.find('@');
    if (at != std::string::npos && spec.find(':') != std::string::npos) {
      uint32_t interval_ms =
          static_cast<uint32_t>(std::strtoul(spec.c_str() + at + 1, nullptr, 0));
      std::vector<uint8_t> masks;
      for (size_t pos = 0; pos < at;) {
        size_t colon = spec.find(':', pos);
        size_t end = (colon == std::string::npos || colon > at) ? at : colon;
        masks.push_back(static_cast<uint8_t>(
            std::strtoul(spec.substr(pos, end - pos).c_str(), nullptr, 0)));
        pos = end + 1;
      }
      start_rx_path_toggle(masks, interval_ms);
    } else {
      auto mask = static_cast<uint8_t>(std::strtoul(spec.c_str(), nullptr, 0));
      SetRxPathMask(mask);
      _logger->info("rx.path_spec: RX-path mask 0x808[7:0]=0x{:02x}", mask);
    }
  }

  /* DEVOURER_RX_CSI_MASK / DEVOURER_RX_NBI — RX-side per-subcarrier masking
   * (receive-equalizer CSI mask / narrowband notch), the poor-man's preamble
   * puncturing. Applied here for the same reason as DEVOURER_RX_PATHS: after
   * the channel set it is the final word; a later channel switch reverts it,
   * so it targets a single-channel RX capture. See ToneMask.h. */
  {
    auto fam = (_eepromManager->version_id.ICType == CHIP_8814A)
                   ? devourer::tonemask::Family::AC2_8814
                   : devourer::tonemask::Family::AC1;
    devourer::tonemask::apply(
        _device, _logger, fam, _channel,
        (_eepromManager->version_id.ICType == CHIP_8814A) ? 4 : 2,
        _cfg.rx.csi_mask ? _cfg.rx.csi_mask->c_str() : nullptr,
        _cfg.rx.nbi ? _cfg.rx.nbi->c_str() : nullptr);
  }

  _logger->info("Listening air...");
  /* Unified RX transport (shared with Jaguar2/3): a single-threaded async
   * bulk-IN URB queue keeps N URBs always-posted on the bulk-IN endpoint and
   * parses each completion inline (while its buffer is alive), instead of a
   * pool of blocking bulk_read_raw worker threads. The always-posted URBs give
   * the 8814's RX-DMA ring the continuity the old worker pool existed to
   * provide (no gap between transfers), and removing the blocking worker
   * threads removes the multi-thread USB contention that starved concurrent
   * NDP-generation TX on some xhci hosts — the beamforming-sounding wedge on
   * the radxa-x4. Consumes on the event pump, so no proc_mu is needed.
   * DEVOURER_RX_URBS sets the queue depth (default 8). */
  int rx_urbs = _cfg.rx.urbs.value_or(8);
  if (rx_urbs < 1)
    rx_urbs = 1;

  /* Closed-loop CFO tracking on the receiver (#217): tick the controller on a
   * ~2 s cadence from the RX loop, mirroring Jaguar2/3. The crystal-cap field
   * (0x2C, per-die position — see SetXtalCap) is written READ-FREE from a base
   * cached before the RX flood: a control-transfer read races the async
   * bulk-IN and throws under load. */
  uint32_t r2c_base = 0, r2c_mask = 0x7FF80000;
  int r2c_shift = 19;
  bool cfo_ok = false;
  if (_cfg.tuning.cfo_track) {
    switch (_eepromManager->version_id.ICType) {
    case CHIP_8814A: r2c_mask = 0x07FF8000; r2c_shift = 15; break;
    case CHIP_8821:  r2c_mask = 0x00FFF000; r2c_shift = 12; break;
    default:         r2c_mask = 0x7FF80000; r2c_shift = 19; break;
    }
    try {
      r2c_base = _device.rtw_read<uint32_t>(0x002C) & ~r2c_mask;
      cfo_ok = true;
    } catch (...) {
      _logger->info("Jaguar1 cfo.track: AFE base read failed — disabled");
    }
  }
  auto cfo_next = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  auto cfo_tick = [&]() {
    if (!cfo_ok || std::chrono::steady_clock::now() < cfo_next)
      return;
    cfo_next += std::chrono::seconds(2);
    double avg_khz = 0;
    const int cur = _xtal_cap < 0 ? (_eepromManager->crystal_cap & 0x3F)
                                  : _xtal_cap;
    const int nc = _cfo.step(cur, 0x3f, &avg_khz); /* 6-bit on Jaguar1 */
    DVR_DEBUG(_logger, "Jaguar1 cfo.track tick: cfo~{} (raw*2.5) cap=0x{:02x} {}",
              static_cast<int>(avg_khz), cur,
              nc >= 0 ? "step" : "hold");
    if (nc >= 0) {
      try {
        const uint32_t c = static_cast<uint32_t>(nc);
        const uint32_t field = (c | (c << 6)) << r2c_shift;
        _device.rtw_write<uint32_t>(0x002C, r2c_base | (field & r2c_mask));
        _xtal_cap = nc;
        _logger->info("Jaguar1 cfo.track: cfo~{} (raw*2.5) xtal_cap=0x{:02x}",
                      static_cast<int>(avg_khz), _xtal_cap);
      } catch (...) {
      }
    }
  };

  auto on_data = [&](const uint8_t *data, int n) {
    cfo_tick();
    FrameParser fp{_logger};
    for (auto &p : fp.recvbuf2recvframe(
             std::span<uint8_t>{const_cast<uint8_t *>(data), (size_t)n})) {
      if (should_stop || g_devourer_should_stop)
        break;
      if (!p.RxAtrib.crc_err) {
        _rxq.add(p.RxAtrib.rssi[0], p.RxAtrib.snr[0], p.RxAtrib.evm[0]);
        _rxpaths.add(p.RxAtrib.rssi, _eepromManager->numTotalRfPath);
        if (_cfg.tuning.cfo_track)
          _cfo.add(p.RxAtrib.cfo_tail); /* closed-loop CFO input (#217) */
      }
      _packetProcessor(p);
    }
  };
  _device.bulk_read_async_loop(32 * 1024, rx_urbs, on_data, [this]() -> bool {
    return should_stop || g_devourer_should_stop;
  });

  _rxmask_stop.store(true);
  if (_rxmask_thread.joinable())
    _rxmask_thread.join();

#if 0
  _device.UsbDevice.SetBulkDataHandler(BulkDataHandler);
  _readTask = Task.Run(() = > _device.UsbDevice.InfinityRead());
#endif
}

void RtlJaguarDevice::SetMonitorChannel(SelectedChannel channel) {
  /* Keep the device-level channel state current: send_packet's 5GHz
   * CCK->OFDM clamp keys off _channel.Channel. Before this assignment
   * existed, _channel was never written anywhere — the clamp read an
   * uninitialised member and fired nondeterministically. */
  _channel = channel;
  _radioManagement->set_channel_bwmode(channel.Channel, channel.ChannelOffset,
                                       channel.ChannelWidth);
  /* Re-apply a runtime RX-path mask: the channel set runs IQK, which
   * saves/restores 0x808 and thereby reverts the mask to the table default.
   * Gated on the mask having been set (SetRxPathMask), so the default
   * all-paths behaviour is byte-identical when the knob is unused. */
  if (_rx_path_mask >= 0)
    _device.rtw_write8(0x808, static_cast<uint8_t>(_rx_path_mask.load()));
}

void RtlJaguarDevice::SetRxPathMask(uint8_t mask) {
  _rx_path_mask = mask;
  _device.rtw_write8(0x808, mask);
}

int RtlJaguarDevice::GetRxPathMask() {
  return _radioManagement->phy_query_bb_reg_public(0x808, 0x000000ff);
}

void RtlJaguarDevice::FastRetune(uint8_t channel, bool cache_rf) {
  if (_radioManagement->fast_retune(channel, cache_rf)) {
    _channel.Channel = channel;
    return;
  }
  /* Fast path declined (band change / non-20MHz) — do the full channel set,
   * preserving the current bandwidth + offset. */
  SetMonitorChannel(SelectedChannel{.Channel = channel,
                                    .ChannelOffset = _channel.ChannelOffset,
                                    .ChannelWidth = _channel.ChannelWidth});
}

void RtlJaguarDevice::FastSetBandwidth(ChannelWidth_t bw) {
  if (_radioManagement->fast_set_bandwidth(bw)) {
    _channel.ChannelWidth = bw;
    return;
  }
  /* Fast path declined (40/80 MHz endpoint, non-8812 die, or no clean 20 MHz
   * cache) — do the full channel set at the current channel/offset. */
  SetMonitorChannel(SelectedChannel{.Channel = _channel.Channel,
                                    .ChannelOffset = _channel.ChannelOffset,
                                    .ChannelWidth = bw});
}

void RtlJaguarDevice::StartWithMonitorMode(SelectedChannel selectedChannel) {
  if (NetDevOpen(selectedChannel) == false) {
    throw std::ios_base::failure("StartWithMonitorMode failed NetDevOpen");
  }

  _radioManagement->SetMonitorMode();
  _brought_up = true;
}

devourer::TxPowerCaps RtlJaguarDevice::GetTxPowerCaps() {
  devourer::TxPowerCaps caps;
  caps.supported = true;
  caps.index_max = 63;
  caps.step_qdb = 2; /* 0.5 dB per TXAGC index step */
  /* On-air slope (tests/txpwr_offset_onair.sh, chip-RSSI ground station):
   * 8812AU 0.496 dB/idx, 8814AU 0.425 dB/idx @ ch36 — nominal confirmed.
   * 8821AU: 0.50 dB/idx exactly at 2.4 GHz but FLAT at 5 GHz (registers move
   * — regcheck readback — while on-air power stays pinned: the 5 GHz chain
   * on that part ignores BB TXAGC), so its measured flag stays false and the
   * power lever should be treated as 2.4 GHz-only there. */
  caps.step_measured =
      _eepromManager->version_id.ICType != CHIP_8821;
  caps.offset_min_qdb = -126;
  caps.offset_max_qdb = 126;
  return caps;
}

int RtlJaguarDevice::SetTxPowerOffsetQdb(int qdb) {
  if (_cw_active) {
    _logger->warn("SetTxPowerOffsetQdb refused: CW tone active (TXAGC does "
                  "not modulate a bare LO carrier)");
    return 0;
  }
  int steps = 0;
  const int applied = devourer::quantize_offset_qdb(qdb, GetTxPowerCaps(),
                                                    &steps);
  _radioManagement->SetTxPowerOffsetSteps(steps);
  if (_brought_up)
    _radioManagement->ApplyTxPower();
  _logger->info("TX-power offset: {} qdB requested -> {} qdB applied "
                "({} steps){}",
                qdb, applied, steps,
                _brought_up ? "" : " (recorded; applies at bring-up)");
  return applied;
}

void RtlJaguarDevice::SetTxPowerIndexOverride(int idx) {
  if (_cw_active) {
    _logger->warn("SetTxPowerIndexOverride refused: CW tone active");
    return;
  }
  _radioManagement->SetTxPowerOverride(idx < 0 ? -1 : (idx > 63 ? 63 : idx));
  if (_brought_up)
    _radioManagement->ApplyTxPower();
}

bool RtlJaguarDevice::ReApplyTxPower() {
  if (!_brought_up || _cw_active)
    return false;
  _radioManagement->ApplyTxPower();
  return true;
}

devourer::TxCaps RtlJaguarDevice::GetTxCaps() {
  /* numTotalRfPath is the TX chain count the EFUSE RF-type resolves to (1 on
   * the 8811AU/8821AU 1T1R cuts, 2 on 8812AU, 4 on 8814AU). All Jaguar-1 AC
   * parts do LDPC/SGI and VHT80. */
  return devourer::tx_caps_for_chains(_eepromManager->numTotalRfPath);
}

int RtlJaguarDevice::SetXtalCap(int cap) {
  /* hal_set_crystal_cap (8812/8821/8814A): the 6-bit trim goes into REG
   * MAC_PHY_CTRL (0x2C) as cap|(cap<<6), but the field position differs per
   * die: 8812/8811 [30:19], 8814A [26:15], 8821A [23:12]. cap < 0 reverts to
   * the efuse default (EepromManager::crystal_cap). */
  const uint8_t c = cap < 0 ? (_eepromManager->crystal_cap & 0x3F)
                            : static_cast<uint8_t>(cap & 0x3F);
  const uint32_t val = static_cast<uint32_t>(c) | (static_cast<uint32_t>(c) << 6);
  uint32_t mask;
  switch (_eepromManager->version_id.ICType) {
  case CHIP_8814A: mask = 0x07FF8000; break; /* 0x2C[26:21]=[20:15] */
  case CHIP_8821:  mask = 0x00FFF000; break; /* 0x2C[23:18]=[17:12] */
  default:         mask = 0x7FF80000; break; /* 8812/8811: 0x2C[30:25]=[24:19] */
  }
  _device.phy_set_bb_reg(0x002C, mask, val);
  _xtal_cap = c;
  _logger->info("Jaguar1: crystal-cap set to 0x{:02x}{}", c,
                cap < 0 ? " (efuse default)" : "");
  return c;
}

devourer::AdapterCaps RtlJaguarDevice::GetAdapterCaps() {
  devourer::AdapterCaps c;
  c.supported = true;
  c.generation = devourer::ChipGeneration::Jaguar1;
  c.transport = _device.is_usb() ? "usb" : "pcie";
  c.tx = GetTxCaps();
  c.txpwr = GetTxPowerCaps();
  const uint8_t chains = _eepromManager->numTotalRfPath;
  c.tx_chains = chains;
  c.rx_chains = chains;
  c.per_chain_rssi = chains >= 2;
  c.bw_mask = devourer::bw_mask_for_generation(c.generation);
  /* 5/10 MHz narrowband on the 8812 die (8812AU/8811AU) and the 8814AU. Both
   * share the Jaguar2 0x8ac baseband clock-divider block; the codes are
   * bench-characterized, TX+RX both directions, both widths
   * (tests/jaguar1_nb_divide_sweep.sh, docs/narrowband.md). The two dies use
   * DIFFERENT field encodings of the same register: the 8812A divides via
   * [9:8]/[21:20], while the 8814A shares the 8822B layout (ADC [9:8]+[16],
   * DAC [21:20]+[28]) and reuses the identical 8821C/8822B divide codes.
   * EXCLUDED: the 8821A — its 1T1R clock tree couples the DAC clock to the TX
   * DMA/USB path, so dividing it starves TX (bulk-out submission failures
   * scale with divide depth: dac=2 ~35% fail, dac=1 ~72%, vs 0% at full clock;
   * 20 MHz unaffected). Experimental (5 MHz@5 GHz is CFO-limited). */
  if (_eepromManager->version_id.ICType == CHIP_8812 ||
      _eepromManager->version_id.ICType == CHIP_8814A) {
    c.bw_mask |= devourer::kBw5 | devourer::kBw10;
    c.narrowband_ok = true;
  }
  c.fastretune_ok = true; /* phy_SwChnl8812_fast (8812/8821) + full-path fallback */
  c.xtal_cap_max = 0x3f; /* 6-bit AFE crystal-cap trim (0x2C) */
  c.xtal_cap_default = _eepromManager->crystal_cap & 0x3f;
  devourer::set_standard_freq_ranges(c);

  /* Identity from the EFUSE version-id. The die name is refined by the RF-type:
   * the 8812 die shipped as both the 2T2R 8812AU and the 1T1R 8811AU cut. */
  switch (_eepromManager->version_id.ICType) {
  case CHIP_8814A:
    c.chip_name = "RTL8814A";
    c.marketing_names = "RTL8814AU";
    c.chip_id = 0x08;
    c.variant = "8814A";
    break;
  case CHIP_8821:
    c.chip_name = "RTL8821A";
    c.marketing_names = "RTL8821AU";
    c.chip_id = 0x05;
    c.variant = "8821A";
    break;
  default: /* CHIP_8812 */
    if (_eepromManager->version_id.RFType == RF_TYPE_1T1R) {
      c.chip_name = "RTL8811A";
      c.marketing_names = "RTL8811AU/RTL8811AR";
      c.variant = "8811A";
    } else {
      c.chip_name = "RTL8812A";
      c.marketing_names = "RTL8812AU/RTL8812AR";
      c.variant = "8812A";
    }
    c.chip_id = 0x04;
    break;
  }
  return c;
}

devourer::TxPowerState RtlJaguarDevice::GetTxPowerState() {
  devourer::TxPowerState s;
  s.valid = true;
  s.flat_index = static_cast<int16_t>(_radioManagement->GetTxPowerOverride());
  s.offset_steps =
      static_cast<int16_t>(_radioManagement->GetTxPowerOffsetSteps());
  s.offset_qdb = static_cast<int16_t>(s.offset_steps * 2);
  s.saturated_low = _radioManagement->TxPowerSaturatedLow();
  s.saturated_high = _radioManagement->TxPowerSaturatedHigh();
  const bool is_8814 = _eepromManager->version_id.ICType == CHIP_8814A;
  if (_brought_up && !is_8814 && !_cw_active) {
    /* Path-A representative indices straight from the TXAGC registers:
     * 0xc20[7:0] = CCK 1M, 0xc24[7:0] = OFDM 6M, 0xc30[31:24] = HT MCS7. */
    s.cck_index = static_cast<int16_t>(
        _radioManagement->phy_query_bb_reg_public(0xc20, 0x000000ff));
    s.ofdm_index = static_cast<int16_t>(
        _radioManagement->phy_query_bb_reg_public(0xc24, 0x000000ff));
    s.mcs7_index = static_cast<int16_t>(
        _radioManagement->phy_query_bb_reg_public(0xc30, 0xff000000));
    s.hw_readback = true;
  } else {
    /* 8814A (write-only packed TXAGC port) / pre-bring-up: software shadow —
     * the same computation the apply loop writes. */
    s.cck_index = _radioManagement->ComputeTxPowerIndex(
        0, static_cast<uint8_t>(MGN_1M), 0);
    s.ofdm_index = _radioManagement->ComputeTxPowerIndex(
        0, static_cast<uint8_t>(MGN_6M), 0);
    s.mcs7_index = _radioManagement->ComputeTxPowerIndex(
        0, static_cast<uint8_t>(MGN_MCS7), 0);
    s.hw_readback = false;
  }
  return s;
}

void RtlJaguarDevice::SetTxMode(const devourer::TxMode& mode) {
  _tx_mode_default = mode;
}

void RtlJaguarDevice::ClearTxMode() { _tx_mode_default.reset(); }

uint32_t RtlJaguarDevice::ReadBBReg(uint16_t addr, uint32_t mask) {
  return _radioManagement->phy_query_bb_reg_public(addr, mask);
}

devourer::EfuseStability RtlJaguarDevice::ProbeEfuseStability(int reads) {
  if (!_brought_up)
    return {}; /* supported=false — needs a powered, brought-up chip */
  auto st = devourer::ProbeEfuseStabilityImpl(
      [this](uint8_t *buf) {
        return _eepromManager->ReadPhysicalEfuseMap(buf, EFUSE_MAP_LEN_JAGUAR);
      },
      EFUSE_MAP_LEN_JAGUAR, reads);
  _logger->info(
      "efuse-stability: reads={} mismatched={} invalid_id={} id=0x{:04x}",
      st.reads, st.mismatched_reads, st.invalid_id_reads, st.eeprom_id);
  return st;
}

bool RtlJaguarDevice::NetDevOpen(SelectedChannel selectedChannel) {
  auto status = _halModule.rtw_hal_init(selectedChannel);
  if (status == false) {
    return false;
  }

  return true;
}

RtlJaguarDevice::~RtlJaguarDevice() {
  /* Safety net: if a CW tone is still armed (caller forgot StopCwTone), restore
   * the chip before teardown so it isn't left radiating a bare carrier. */
  StopCwTone();
  _qd_stop.store(true);
  if (_qd_thread.joinable()) {
    _qd_thread.join();
  }
  _therm_stop.store(true);
  if (_therm_thread.joinable()) {
    _therm_thread.join();
  }
  _rxmask_stop.store(true);
  if (_rxmask_thread.joinable()) {
    _rxmask_thread.join();
  }
}

/* Cycle the RX-path mask (0x808 byte 0) through `masks` every `interval_ms` on a
 * background thread, emitting an `rx.path_mask` event on each switch.
 * The control-transfer write runs concurrently with the RX bulk-IN workers on a
 * different endpoint, which libusb permits (see the queue-depth poller). Used by
 * the mobile/fading combining measurement. */
void RtlJaguarDevice::start_rx_path_toggle(const std::vector<uint8_t> &masks,
                                           uint32_t interval_ms) {
  if (masks.empty() || interval_ms == 0)
    return;
  _logger->info("DEVOURER_RX_PATHS toggle: {} masks @ {} ms", masks.size(),
                interval_ms);
  _rxmask_thread = std::thread([this, masks, interval_ms]() {
    size_t i = 0;
    while (!_rxmask_stop.load()) {
      uint8_t m = masks[i++ % masks.size()];
      _device.rtw_write8(0x808, m);
      devourer::Ev(_logger->events(), "rx.path_mask").t().hexf("mask", m, 2);
      for (uint32_t slept = 0; slept < interval_ms && !_rxmask_stop.load();
           slept += 25)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  });
}

void RtlJaguarDevice::start_queue_depth_poller(uint32_t interval_ms) {
  if (interval_ms == 0) return;
  if (_qd_thread.joinable()) {
    _logger->warn("queue-depth poller already running");
    return;
  }
  if (_eepromManager->version_id.ICType != CHIP_8814A) {
    _logger->warn(
        "DEVOURER_QUEUE_POLL_MS set but chip is not 8814A — REG_FIFOPAGE_INFO_*"
        " registers don't decode as per-queue free pages on this chip; poller"
        " disabled");
    return;
  }
  _qd_thread = std::thread([this, interval_ms]() {
    /* libusb-1.0 allows concurrent transfers on different endpoints from
     * different threads — vendor control (ep 0) doesn't conflict with the
     * RX-loop's bulk-IN. The reads here are synchronous control transfers
     * via _device.rtw_read32, so no completion-callback plumbing needed. */
    while (!_qd_stop.load()) {
      for (size_t i = 0; i < 5; ++i) {
        if (_qd_stop.load()) break;
        uint32_t v = _device.rtw_read32(kFifoPageInfoRegs_8814A[i]);
        _qd_snap[i].store(v, std::memory_order_relaxed);
      }
      /* Sleep in short slices so destruction doesn't block for a full
       * interval after _qd_stop is set. */
      for (uint32_t slept = 0; slept < interval_ms && !_qd_stop.load();
           slept += 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  });
}

std::array<uint32_t, 5> RtlJaguarDevice::get_queue_depth() const {
  std::array<uint32_t, 5> out{};
  for (size_t i = 0; i < 5; ++i) {
    out[i] = _qd_snap[i].load(std::memory_order_relaxed);
  }
  return out;
}

static uint32_t pack_thermal(const ThermalStatus &s) {
  int8_t d = static_cast<int8_t>(
      s.delta > 127 ? 127 : (s.delta < -128 ? -128 : s.delta));
  return (s.valid ? 1u : 0u) | (uint32_t(s.raw) << 8) |
         (uint32_t(s.baseline) << 16) |
         (uint32_t(static_cast<uint8_t>(d)) << 24);
}

static ThermalStatus unpack_thermal(uint32_t v) {
  ThermalStatus s;
  s.valid = (v & 1u) != 0;
  s.raw = static_cast<uint8_t>((v >> 8) & 0xFF);
  s.baseline = static_cast<uint8_t>((v >> 16) & 0xFF);
  s.delta = static_cast<int8_t>((v >> 24) & 0xFF);
  return s;
}

ThermalStatus RtlJaguarDevice::GetThermalStatus() {
  return _radioManagement->ReadThermalStatus();
}

ThermalStatus RtlJaguarDevice::get_thermal_snapshot() const {
  return unpack_thermal(_therm_snap.load(std::memory_order_relaxed));
}

void RtlJaguarDevice::start_thermal_poller(uint32_t interval_ms,
                                           int warn_delta) {
  if (interval_ms == 0) return;
  if (_therm_thread.joinable()) {
    _logger->warn("thermal poller already running");
    return;
  }
  _therm_thread = std::thread([this, interval_ms, warn_delta]() {
    bool warned = false;
    bool baseline_note = false;
    while (!_therm_stop.load()) {
      ThermalStatus s = _radioManagement->ReadThermalStatus();
      _therm_snap.store(pack_thermal(s), std::memory_order_relaxed);
      if (!s.valid) {
        if (!baseline_note) {
          _logger->info(
              "thermal: no EFUSE baseline (0xFF) — reporting raw only "
              "(raw={})",
              unsigned(s.raw));
          baseline_note = true;
        }
      } else if (s.delta >= warn_delta) {
        if (!warned) {
          _logger->warn(
              "thermal: chip running hot ({}) — raw={} baseline={} delta=+{} "
              "(>= {}); TX power tracking backing off, sustained TX may "
              "degrade the PA",
              ThermalBucket(s), unsigned(s.raw), unsigned(s.baseline), s.delta,
              warn_delta);
          warned = true;
        }
      } else {
        warned = false; /* re-arm once it cools back under the threshold */
      }
      /* Sleep in short slices so destruction doesn't block for a full
       * interval after _therm_stop is set. */
      for (uint32_t slept = 0; slept < interval_ms && !_therm_stop.load();
           slept += 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  });
}

uint32_t RtlJaguarDevice::read_bb_dbgport(uint32_t selector) {
  if (!_bb_dbgport) {
    _bb_dbgport = std::make_unique<devourer::BbDbgportReader>(_device, _logger);
  }
  return _bb_dbgport->read_dbgport(selector);
}

bool RtlJaguarDevice::bb_dbgport_wedged() const {
  return _bb_dbgport && _bb_dbgport->is_wedged();
}
