#include "Iqk8812a.h"

#include "Hal8812PhyReg.h"
#include "RadioManagementModule.h"

#include <chrono>
#include <thread>

namespace {

constexpr uint32_t kMaskBit31 = 0x80000000u;
constexpr uint32_t kMaskByte0Bit7 = 0x80u;
constexpr uint32_t kMaskBit18 = 1u << 18;
constexpr uint32_t kMaskBit29 = 1u << 29;
constexpr uint32_t kMaskBit10 = 1u << 10;
constexpr uint32_t kMaskBit11 = 1u << 11;
constexpr uint32_t kMaskBit12 = 1u << 12;
constexpr uint32_t kMask07ff0000 = 0x07ff0000u;
constexpr uint32_t kMask07ff = 0x000007ffu;
constexpr uint32_t kMask03ff = 0x000003ffu;
constexpr uint32_t kMask03ff0000 = 0x03ff0000u;
constexpr uint32_t kMask03ff8000 = 0x03ff8000u;
constexpr uint32_t kMaskF = 0x000f;
constexpr uint32_t kMaskBit26_25_24 = (1u << 26) | (1u << 25) | (1u << 24);
constexpr uint32_t kMaskDWord = 0xFFFFFFFFu;
constexpr uint32_t kRFRegMask = 0xfffffu; /* 20-bit RF reg mask */

inline void DelayMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace

Iqk8812a::Iqk8812a(RtlAdapter device,
                   std::shared_ptr<EepromManager> eepromManager,
                   RadioManagementModule *radio, Logger_t logger)
    : _device(device), _eepromManager(eepromManager), _radio(radio),
      _logger(logger) {}

void Iqk8812a::BackupMacBb(const uint32_t *regs, uint32_t *out, int n) {
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0); /* Page C */
  for (int i = 0; i < n; i++) {
    out[i] = _device.rtw_read32(regs[i]);
  }
}

void Iqk8812a::BackupRf(const uint32_t *regs, uint32_t *outA, uint32_t *outB,
                        int n) {
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0); /* Page C */
  for (int i = 0; i < n; i++) {
    outA[i] = _radio->phy_query_rf_reg(RfPath::RF_PATH_A, regs[i], kMaskDWord);
    outB[i] = _radio->phy_query_rf_reg(RfPath::RF_PATH_B, regs[i], kMaskDWord);
  }
}

void Iqk8812a::BackupAfe(const uint32_t *regs, uint32_t *out, int n) {
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0); /* Page C */
  for (int i = 0; i < n; i++) {
    out[i] = _device.rtw_read32(regs[i]);
  }
}

void Iqk8812a::RestoreMacBb(const uint32_t *regs, const uint32_t *backup,
                            int n) {
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0); /* Page C */
  for (int i = 0; i < n; i++) {
    _device.rtw_write32(regs[i], backup[i]);
  }
}

void Iqk8812a::RestoreRf(RfPath path, const uint32_t *regs,
                         const uint32_t *backup, int n) {
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0); /* Page C */
  for (int i = 0; i < n; i++) {
    _radio->phy_set_rf_reg(path, regs[i], kRFRegMask, backup[i]);
  }
  /* RF 0xef: clear */
  _radio->phy_set_rf_reg(path, 0xef, kRFRegMask, 0x0);
}

void Iqk8812a::RestoreAfe(const uint32_t *regs, const uint32_t *backup, int n) {
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0); /* Page C */
  for (int i = 0; i < n; i++) {
    _device.rtw_write32(regs[i], backup[i]);
  }
  /* Tail of upstream `_iqk_restore_afe_8812a`: page-C1 IQC reset hold. */
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x1); /* Page C1 */
  _device.rtw_write32(0xc80, 0x0);
  _device.rtw_write32(0xc84, 0x0);
  _device.rtw_write32(0xc88, 0x0);
  _device.rtw_write32(0xc8c, 0x3c000000);
  _device.phy_set_bb_reg(0xc90, kMaskByte0Bit7, 0x1);
  _device.phy_set_bb_reg(0xcc4, kMaskBit18, 0x1);
  /* dpk_done == false in devourer (DPK not ported) — always set bit 29. */
  _device.phy_set_bb_reg(0xcc4, kMaskBit29, 0x1);
  _device.phy_set_bb_reg(0xcc8, kMaskBit29, 0x1);
  _device.rtw_write32(0xe80, 0x0);
  _device.rtw_write32(0xe84, 0x0);
  _device.rtw_write32(0xe88, 0x0);
  _device.rtw_write32(0xe8c, 0x3c000000);
  _device.phy_set_bb_reg(0xe90, kMaskByte0Bit7, 0x1);
  _device.phy_set_bb_reg(0xec4, kMaskBit18, 0x1);
  _device.phy_set_bb_reg(0xec4, kMaskBit29, 0x1);
  _device.phy_set_bb_reg(0xec8, kMaskBit29, 0x1);
}

void Iqk8812a::ConfigureMac() {
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0); /* Page C */
  _device.rtw_write8(0x522, 0x3f);
  _device.phy_set_bb_reg(0x550, (1u << 11) | (1u << 3), 0x0);
  _device.rtw_write8(0x808, 0x00); /* RX ante off */
  _device.phy_set_bb_reg(0x838, kMaskF, 0xc); /* CCA off */
  _device.rtw_write8(0xa07, 0xf); /* CCK RX path off */
}

void Iqk8812a::FillTxIqc(RfPath path, int X, int Y) {
  if (path == RfPath::RF_PATH_A) {
    _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x1); /* Page C1 */
    _device.phy_set_bb_reg(0xc90, kMaskByte0Bit7, 0x1);
    _device.phy_set_bb_reg(0xcc4, kMaskBit18, 0x1);
    _device.phy_set_bb_reg(0xcc4, kMaskBit29, 0x1); /* dpk_done false */
    _device.phy_set_bb_reg(0xcc8, kMaskBit29, 0x1);
    _device.phy_set_bb_reg(0xccc, kMask07ff, static_cast<uint32_t>(Y));
    _device.phy_set_bb_reg(0xcd4, kMask07ff, static_cast<uint32_t>(X));
  } else if (path == RfPath::RF_PATH_B) {
    _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x1); /* Page C1 */
    _device.phy_set_bb_reg(0xe90, kMaskByte0Bit7, 0x1);
    _device.phy_set_bb_reg(0xec4, kMaskBit18, 0x1);
    _device.phy_set_bb_reg(0xec4, kMaskBit29, 0x1);
    _device.phy_set_bb_reg(0xec8, kMaskBit29, 0x1);
    _device.phy_set_bb_reg(0xecc, kMask07ff, static_cast<uint32_t>(Y));
    _device.phy_set_bb_reg(0xed4, kMask07ff, static_cast<uint32_t>(X));
  }
}

void Iqk8812a::FillRxIqc(RfPath path, int X, int Y) {
  if (path == RfPath::RF_PATH_A) {
    _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0); /* Page C */
    uint32_t xs = static_cast<uint32_t>(X) >> 1;
    uint32_t ys = static_cast<uint32_t>(Y) >> 1;
    if (xs >= 0x112 || (ys >= 0x12 && ys <= 0x3ee)) {
      _device.phy_set_bb_reg(0xc10, kMask03ff, 0x100);
      _device.phy_set_bb_reg(0xc10, kMask03ff0000, 0);
    } else {
      _device.phy_set_bb_reg(0xc10, kMask03ff, xs);
      _device.phy_set_bb_reg(0xc10, kMask03ff0000, ys);
    }
  } else if (path == RfPath::RF_PATH_B) {
    _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0);
    uint32_t xs = static_cast<uint32_t>(X) >> 1;
    uint32_t ys = static_cast<uint32_t>(Y) >> 1;
    if (xs >= 0x112 || (ys >= 0x12 && ys <= 0x3ee)) {
      _device.phy_set_bb_reg(0xe10, kMask03ff, 0x100);
      _device.phy_set_bb_reg(0xe10, kMask03ff0000, 0);
    } else {
      _device.phy_set_bb_reg(0xe10, kMask03ff, xs);
      _device.phy_set_bb_reg(0xe10, kMask03ff0000, ys);
    }
  }
}

void Iqk8812a::DoTxRxCalibration(uint8_t chnl_idx, BandType band) {
  (void)chnl_idx; /* unused in upstream — kept for signature symmetry */

  int TX_IQC_temp[kCalNum][4]{};
  int RX_IQC_temp[kCalNum][4]{};
  int TX_IQC[4]{};
  int RX_IQC[4]{};
  uint8_t tx0_average = 0, tx1_average = 0;
  uint8_t rx0_average = 0, rx1_average = 0;
  uint8_t cal0_retry = 0, cal1_retry = 0;
  bool TX0_fail = true, RX0_fail = true;
  bool TX1_fail = true, RX1_fail = true;
  bool IQK0_ready = false, IQK1_ready = false;
  bool TX0_finish = false, TX1_finish = false;
  bool RX0_finish = false, RX1_finish = false;

  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0); /* Page C */

  /* ===== path-A/B AFE all on ===== */
  _device.rtw_write32(0xc60, 0x77777777);
  _device.rtw_write32(0xc64, 0x77777777);
  _device.rtw_write32(0xe60, 0x77777777);
  _device.rtw_write32(0xe64, 0x77777777);
  _device.rtw_write32(0xc68, 0x19791979);
  _device.rtw_write32(0xe68, 0x19791979);
  _device.phy_set_bb_reg(0xc00, kMaskF, 0x4); /* HW 3-wire off */
  _device.phy_set_bb_reg(0xe00, kMaskF, 0x4);

  /* DAC/ADC sampling rate 160 MHz */
  _device.phy_set_bb_reg(0xc5c, kMaskBit26_25_24, 0x7);
  _device.phy_set_bb_reg(0xe5c, kMaskBit26_25_24, 0x7);

  /* ===== path A TX IQK RF setting ===== */
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0xef, kRFRegMask, 0x80002);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0x30, kRFRegMask, 0x20000);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0x31, kRFRegMask, 0x3fffd);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0x32, kRFRegMask, 0xfe83f);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0x65, kRFRegMask, 0x931d5);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0x8f, kRFRegMask, 0x8a001);
  /* ===== path B TX IQK RF setting ===== */
  _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0xef, kRFRegMask, 0x80002);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0x30, kRFRegMask, 0x20000);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0x31, kRFRegMask, 0x3fffd);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0x32, kRFRegMask, 0xfe83f);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0x65, kRFRegMask, 0x931d5);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0x8f, kRFRegMask, 0x8a001);

  _device.rtw_write32(0x90c, 0x00008000);
  _device.phy_set_bb_reg(0xc94, 1u, 0x1);
  _device.phy_set_bb_reg(0xe94, 1u, 0x1);
  _device.rtw_write32(0x978, 0x29002000); /* TX (X,Y) */
  _device.rtw_write32(0x97c, 0xa9002000); /* RX (X,Y) */
  _device.rtw_write32(0x984, 0x00462910); /* [0]:AGC_en, [15]:idac_K_Mask */
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x1); /* Page C1 */

  /* RFE-type / ext-PA AFE setup. Devourer doesn't currently track
   * `ext_pa` / `ext_pa_5g` as runtime state; upstream pulls these from
   * EFUSE. EepromManager exposes `ExternalPA_2G` + `external_pa_5g`. */
  bool ext_pa_5g = _eepromManager->GetExternalPa5G() != 0;
  bool ext_pa = _eepromManager->ExternalPA_2G;
  uint16_t rfe = _eepromManager->rfe_type;
  if (ext_pa_5g) {
    if (rfe == 1) {
      _device.rtw_write32(0xc88, 0x821403e3);
      _device.rtw_write32(0xe88, 0x821403e3);
    } else {
      _device.rtw_write32(0xc88, 0x821403f7);
      _device.rtw_write32(0xe88, 0x821403f7);
    }
  } else {
    _device.rtw_write32(0xc88, 0x821403f1);
    _device.rtw_write32(0xe88, 0x821403f1);
  }
  if (band == BandType::BAND_ON_5G) {
    _device.rtw_write32(0xc8c, 0x68163e96);
    _device.rtw_write32(0xe8c, 0x68163e96);
  } else {
    _device.rtw_write32(0xc8c, 0x28163e96);
    _device.rtw_write32(0xe8c, 0x28163e96);
    if (rfe == 3) {
      if (ext_pa) {
        _device.rtw_write32(0xc88, 0x821403e3);
      } else {
        _device.rtw_write32(0xc88, 0x821403f7);
      }
    }
  }

  /* === TX-tone calibration loop (non-VDF / non-160MHz path) === */
  _device.rtw_write32(0xc80, 0x18008c10); /* TX_Tone_idx[9:0], TxK_Mask[29] */
  _device.rtw_write32(0xc84, 0x38008c10); /* RX_Tone_idx[9:0], RxK_Mask[29] */
  _device.rtw_write32(0xce8, 0x00000000);
  _device.rtw_write32(0xe80, 0x18008c10);
  _device.rtw_write32(0xe84, 0x38008c10);
  _device.rtw_write32(0xee8, 0x00000000);

  while (true) {
    /* one-shot */
    _device.rtw_write32(0xcb8, 0x00100000);
    _device.rtw_write32(0xeb8, 0x00100000);
    _device.rtw_write32(0x980, 0xfa000000);
    _device.rtw_write32(0x980, 0xf8000000);

    DelayMs(10);
    _device.rtw_write32(0xcb8, 0x00000000);
    _device.rtw_write32(0xeb8, 0x00000000);

    int delay_count = 0;
    while (true) {
      if (!TX0_finish) {
        IQK0_ready = (_device.rtw_read32(0xd00) & kMaskBit10) != 0;
      }
      if (!TX1_finish) {
        IQK1_ready = (_device.rtw_read32(0xd40) & kMaskBit10) != 0;
      }
      if ((IQK0_ready && IQK1_ready) || delay_count > 20) {
        break;
      }
      DelayMs(1);
      delay_count++;
    }

    if (delay_count < 20) {
      /* ===== TXIQK check ===== */
      TX0_fail = (_device.rtw_read32(0xd00) & kMaskBit12) != 0;
      TX1_fail = (_device.rtw_read32(0xd40) & kMaskBit12) != 0;
      if (!(TX0_fail || TX0_finish)) {
        _device.rtw_write32(0xcb8, 0x02000000);
        TX_IQC_temp[tx0_average][0] = static_cast<int>(
            ((_device.rtw_read32(0xd00) & kMask07ff0000) >> 16) << 21);
        _device.rtw_write32(0xcb8, 0x04000000);
        TX_IQC_temp[tx0_average][1] = static_cast<int>(
            ((_device.rtw_read32(0xd00) & kMask07ff0000) >> 16) << 21);
        tx0_average++;
      } else {
        cal0_retry++;
        if (cal0_retry == 10) {
          break;
        }
      }
      if (!(TX1_fail || TX1_finish)) {
        _device.rtw_write32(0xeb8, 0x02000000);
        TX_IQC_temp[tx1_average][2] = static_cast<int>(
            ((_device.rtw_read32(0xd40) & kMask07ff0000) >> 16) << 21);
        _device.rtw_write32(0xeb8, 0x04000000);
        TX_IQC_temp[tx1_average][3] = static_cast<int>(
            ((_device.rtw_read32(0xd40) & kMask07ff0000) >> 16) << 21);
        tx1_average++;
      } else {
        cal1_retry++;
        if (cal1_retry == 10) {
          break;
        }
      }
    } else {
      cal0_retry++;
      cal1_retry++;
      if (cal0_retry == 10) {
        break;
      }
    }

    /* Average when two samples agree within ±4 */
    if (tx0_average >= 2) {
      for (int i = 0; i < tx0_average && !TX0_finish; i++) {
        for (int ii = i + 1; ii < tx0_average && !TX0_finish; ii++) {
          int dx = (TX_IQC_temp[i][0] >> 21) - (TX_IQC_temp[ii][0] >> 21);
          if (dx < 4 && dx > -4) {
            int dy = (TX_IQC_temp[i][1] >> 21) - (TX_IQC_temp[ii][1] >> 21);
            if (dy < 4 && dy > -4) {
              TX_IQC[0] = ((TX_IQC_temp[i][0] >> 21) +
                           (TX_IQC_temp[ii][0] >> 21)) /
                          2;
              TX_IQC[1] = ((TX_IQC_temp[i][1] >> 21) +
                           (TX_IQC_temp[ii][1] >> 21)) /
                          2;
              TX0_finish = true;
            }
          }
        }
      }
    }
    if (tx1_average >= 2) {
      for (int i = 0; i < tx1_average && !TX1_finish; i++) {
        for (int ii = i + 1; ii < tx1_average && !TX1_finish; ii++) {
          int dx = (TX_IQC_temp[i][2] >> 21) - (TX_IQC_temp[ii][2] >> 21);
          if (dx < 4 && dx > -4) {
            int dy = (TX_IQC_temp[i][3] >> 21) - (TX_IQC_temp[ii][3] >> 21);
            if (dy < 4 && dy > -4) {
              TX_IQC[2] = ((TX_IQC_temp[i][2] >> 21) +
                           (TX_IQC_temp[ii][2] >> 21)) /
                          2;
              TX_IQC[3] = ((TX_IQC_temp[i][3] >> 21) +
                           (TX_IQC_temp[ii][3] >> 21)) /
                          2;
              TX1_finish = true;
            }
          }
        }
      }
    }
    if (TX0_finish && TX1_finish) {
      break;
    }
    if ((cal0_retry + tx0_average) >= 10 ||
        (cal1_retry + tx1_average) >= 10) {
      break;
    }
  }

  DVR_DEBUG(_logger, "Iqk8812a TX: A_done={} B_done={} A_retry={} B_retry={}",
                 unsigned(TX0_finish), unsigned(TX1_finish),
                 unsigned(cal0_retry), unsigned(cal1_retry));

  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0); /* Page C */
  /* Load LOK (lookup-table for local oscillator K) into RF 0x58 from
   * RF 0x08[19:10]. */
  uint32_t lokA = _radio->phy_query_rf_reg(RfPath::RF_PATH_A, 0x8, 0xffc00);
  uint32_t lokB = _radio->phy_query_rf_reg(RfPath::RF_PATH_B, 0x8, 0xffc00);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0x58, 0x7fe00, lokA);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0x58, 0x7fe00, lokB);
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x1); /* Page C1 */

  /* === RX IQK setup === */
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0); /* Page C */
  if (TX0_finish) {
    _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0xef, kRFRegMask, 0x80000);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0x30, kRFRegMask, 0x30000);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0x31, kRFRegMask, 0x3f7ff);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0x32, kRFRegMask, 0xfe7bf);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0x8f, kRFRegMask, 0x88001);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0x65, kRFRegMask, 0x931d1);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0xef, kRFRegMask, 0x00000);
  }
  if (TX1_finish) {
    _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0xef, kRFRegMask, 0x80000);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0x30, kRFRegMask, 0x30000);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0x31, kRFRegMask, 0x3f7ff);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0x32, kRFRegMask, 0xfe7bf);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0x8f, kRFRegMask, 0x88001);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0x65, kRFRegMask, 0x931d1);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0xef, kRFRegMask, 0x00000);
  }

  _device.phy_set_bb_reg(0x978, kMaskBit31, 0x1);
  _device.phy_set_bb_reg(0x97c, kMaskBit31, 0x0);
  _device.rtw_write32(0x90c, 0x00008000);
  /* USB interface */
  _device.rtw_write32(0x984, 0x0046a890);

  if (rfe == 1) {
    _device.rtw_write32(0xcb0, 0x77777717);
    _device.rtw_write32(0xcb4, 0x00000077);
    _device.rtw_write32(0xeb0, 0x77777717);
    _device.rtw_write32(0xeb4, 0x00000077);
  } else {
    _device.rtw_write32(0xcb0, 0x77777717);
    _device.rtw_write32(0xcb4, 0x02000077);
    _device.rtw_write32(0xeb0, 0x77777717);
    _device.rtw_write32(0xeb4, 0x02000077);
  }

  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x1); /* Page C1 */
  if (TX0_finish) {
    _device.rtw_write32(0xc80, 0x38008c10);
    _device.rtw_write32(0xc84, 0x18008c10);
    _device.rtw_write32(0xc88, 0x82140119);
  }
  if (TX1_finish) {
    _device.rtw_write32(0xe80, 0x38008c10);
    _device.rtw_write32(0xe84, 0x18008c10);
    _device.rtw_write32(0xe88, 0x82140119);
  }

  cal0_retry = 0;
  cal1_retry = 0;
  IQK0_ready = false;
  IQK1_ready = false;

  while (true) {
    /* one-shot */
    _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0); /* Page C */
    if (TX0_finish) {
      _device.phy_set_bb_reg(0x978, kMask03ff8000,
                             static_cast<uint32_t>(TX_IQC[0] & 0x000007ff));
      _device.phy_set_bb_reg(0x978, kMask07ff,
                             static_cast<uint32_t>(TX_IQC[1] & 0x000007ff));
      _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x1);
      _device.rtw_write32(0xc8c, (rfe == 1) ? 0x28161500 : 0x28160cc0);
      _device.rtw_write32(0xcb8, 0x00300000);
      _device.rtw_write32(0xcb8, 0x00100000);
      DelayMs(5);
      _device.rtw_write32(0xc8c, 0x3c000000);
      _device.rtw_write32(0xcb8, 0x00000000);
    }
    if (TX1_finish) {
      _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0);
      _device.phy_set_bb_reg(0x978, kMask03ff8000,
                             static_cast<uint32_t>(TX_IQC[2] & 0x000007ff));
      _device.phy_set_bb_reg(0x978, kMask07ff,
                             static_cast<uint32_t>(TX_IQC[3] & 0x000007ff));
      _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x1);
      _device.rtw_write32(0xe8c, (rfe == 1) ? 0x28161500 : 0x28160ca0);
      _device.rtw_write32(0xeb8, 0x00300000);
      _device.rtw_write32(0xeb8, 0x00100000);
      DelayMs(5);
      _device.rtw_write32(0xe8c, 0x3c000000);
      _device.rtw_write32(0xeb8, 0x00000000);
    }

    int delay_count = 0;
    while (true) {
      if (!RX0_finish && TX0_finish) {
        IQK0_ready = (_device.rtw_read32(0xd00) & kMaskBit10) != 0;
      }
      if (!RX1_finish && TX1_finish) {
        IQK1_ready = (_device.rtw_read32(0xd40) & kMaskBit10) != 0;
      }
      if ((IQK0_ready && IQK1_ready) || delay_count > 20) {
        break;
      }
      DelayMs(1);
      delay_count++;
    }

    if (delay_count < 20) {
      RX0_fail = (_device.rtw_read32(0xd00) & kMaskBit11) != 0;
      RX1_fail = (_device.rtw_read32(0xd40) & kMaskBit11) != 0;
      if (!(RX0_fail || RX0_finish) && TX0_finish) {
        _device.rtw_write32(0xcb8, 0x06000000);
        RX_IQC_temp[rx0_average][0] = static_cast<int>(
            ((_device.rtw_read32(0xd00) & kMask07ff0000) >> 16) << 21);
        _device.rtw_write32(0xcb8, 0x08000000);
        RX_IQC_temp[rx0_average][1] = static_cast<int>(
            ((_device.rtw_read32(0xd00) & kMask07ff0000) >> 16) << 21);
        rx0_average++;
      } else {
        cal0_retry++;
        if (cal0_retry == 10) {
          break;
        }
      }
      if (!(RX1_fail || RX1_finish) && TX1_finish) {
        _device.rtw_write32(0xeb8, 0x06000000);
        RX_IQC_temp[rx1_average][2] = static_cast<int>(
            ((_device.rtw_read32(0xd40) & kMask07ff0000) >> 16) << 21);
        _device.rtw_write32(0xeb8, 0x08000000);
        RX_IQC_temp[rx1_average][3] = static_cast<int>(
            ((_device.rtw_read32(0xd40) & kMask07ff0000) >> 16) << 21);
        rx1_average++;
      } else {
        cal1_retry++;
        if (cal1_retry == 10) {
          break;
        }
      }
    } else {
      cal0_retry++;
      cal1_retry++;
      if (cal0_retry == 10) {
        break;
      }
    }

    if (rx0_average >= 2) {
      for (int i = 0; i < rx0_average && !RX0_finish; i++) {
        for (int ii = i + 1; ii < rx0_average && !RX0_finish; ii++) {
          int dx = (RX_IQC_temp[i][0] >> 21) - (RX_IQC_temp[ii][0] >> 21);
          if (dx < 4 && dx > -4) {
            int dy = (RX_IQC_temp[i][1] >> 21) - (RX_IQC_temp[ii][1] >> 21);
            if (dy < 4 && dy > -4) {
              RX_IQC[0] = ((RX_IQC_temp[i][0] >> 21) +
                           (RX_IQC_temp[ii][0] >> 21)) /
                          2;
              RX_IQC[1] = ((RX_IQC_temp[i][1] >> 21) +
                           (RX_IQC_temp[ii][1] >> 21)) /
                          2;
              RX0_finish = true;
              break;
            }
          }
        }
      }
    }
    if (rx1_average >= 2) {
      for (int i = 0; i < rx1_average && !RX1_finish; i++) {
        for (int ii = i + 1; ii < rx1_average && !RX1_finish; ii++) {
          int dx = (RX_IQC_temp[i][2] >> 21) - (RX_IQC_temp[ii][2] >> 21);
          if (dx < 4 && dx > -4) {
            int dy = (RX_IQC_temp[i][3] >> 21) - (RX_IQC_temp[ii][3] >> 21);
            if (dy < 4 && dy > -4) {
              RX_IQC[2] = ((RX_IQC_temp[i][2] >> 21) +
                           (RX_IQC_temp[ii][2] >> 21)) /
                          2;
              RX_IQC[3] = ((RX_IQC_temp[i][3] >> 21) +
                           (RX_IQC_temp[ii][3] >> 21)) /
                          2;
              RX1_finish = true;
              break;
            }
          }
        }
      }
    }
    if ((RX0_finish || !TX0_finish) && (RX1_finish || !TX1_finish)) {
      break;
    }
    if ((cal0_retry + rx0_average) >= 10 ||
        (cal1_retry + rx1_average) >= 10 || rx0_average == 3 ||
        rx1_average == 3) {
      break;
    }
  }

  DVR_DEBUG(_logger, "Iqk8812a RX: A_done={} B_done={} A_retry={} B_retry={}",
                 unsigned(RX0_finish), unsigned(RX1_finish),
                 unsigned(cal0_retry), unsigned(cal1_retry));

  /* Fill IQK results — defaults for paths that didn't converge. */
  if (TX0_finish) {
    FillTxIqc(RfPath::RF_PATH_A, TX_IQC[0], TX_IQC[1]);
  } else {
    FillTxIqc(RfPath::RF_PATH_A, 0x200, 0x0);
  }
  if (RX0_finish) {
    FillRxIqc(RfPath::RF_PATH_A, RX_IQC[0], RX_IQC[1]);
  } else {
    FillRxIqc(RfPath::RF_PATH_A, 0x200, 0x0);
  }
  if (TX1_finish) {
    FillTxIqc(RfPath::RF_PATH_B, TX_IQC[2], TX_IQC[3]);
  } else {
    FillTxIqc(RfPath::RF_PATH_B, 0x200, 0x0);
  }
  if (RX1_finish) {
    FillRxIqc(RfPath::RF_PATH_B, RX_IQC[2], RX_IQC[3]);
  } else {
    FillRxIqc(RfPath::RF_PATH_B, 0x200, 0x0);
  }
}

void Iqk8812a::Calibrate(uint8_t channel, BandType band, bool is_recovery) {
  (void)is_recovery; /* TODO: cached reload path; for now always recompute */

  /* Backup register addresses — verbatim from upstream
   * `_phy_iq_calibrate_8812a` (line 1128). */
  const uint32_t backup_macbb_reg[kMacBbRegNum] = {
      0x520, 0x550, 0x808, 0xa04, 0x90c, 0xc00, 0xe00, 0x838, 0x82c};
  const uint32_t backup_afe_reg[kAfeRegNum] = {
      0xc5c, 0xc60, 0xc64, 0xc68, 0xcb0, 0xcb4,
      0xe5c, 0xe60, 0xe64, 0xe68, 0xeb0, 0xeb4};
  const uint32_t backup_rf_reg[kRfRegNum] = {0x65, 0x8f, 0x0};

  uint32_t MACBB_backup[kMacBbRegNum]{};
  uint32_t AFE_backup[kAfeRegNum]{};
  uint32_t RFA_backup[kRfRegNum]{};
  uint32_t RFB_backup[kRfRegNum]{};

  BackupMacBb(backup_macbb_reg, MACBB_backup, kMacBbRegNum);
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x1); /* Page C1 */
  uint32_t reg_c1b8 = _device.rtw_read32(0xcb8);
  uint32_t reg_e1b8 = _device.rtw_read32(0xeb8);
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0); /* Page C */
  BackupAfe(backup_afe_reg, AFE_backup, kAfeRegNum);
  BackupRf(backup_rf_reg, RFA_backup, RFB_backup, kRfRegNum);

  ConfigureMac();
  DoTxRxCalibration(/*chnl_idx=*/0, band);

  RestoreRf(RfPath::RF_PATH_A, backup_rf_reg, RFA_backup, kRfRegNum);
  RestoreRf(RfPath::RF_PATH_B, backup_rf_reg, RFB_backup, kRfRegNum);
  RestoreAfe(backup_afe_reg, AFE_backup, kAfeRegNum);
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x1); /* Page C1 */
  _device.rtw_write32(0xcb8, reg_c1b8);
  _device.rtw_write32(0xeb8, reg_e1b8);
  _device.phy_set_bb_reg(0x82c, kMaskBit31, 0x0); /* Page C */
  RestoreMacBb(backup_macbb_reg, MACBB_backup, kMacBbRegNum);

  _logger->info("Iqk8812a::Calibrate done (channel={})", unsigned(channel));
}
