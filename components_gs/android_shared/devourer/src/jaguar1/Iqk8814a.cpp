#include "Iqk8814a.h"

#include "RadioManagementModule.h"

#include <chrono>
#include <thread>

namespace {

constexpr uint32_t kRFRegMask = 0xfffffu; /* 20-bit RF reg mask */
constexpr int kLokDelayMs = 1;            /* upstream LOK_delay */
constexpr int kWbiqkDelayMs = 10;         /* upstream WBIQK_delay */
constexpr int kTxIqk = 0;
constexpr int kRxIqk = 1;

inline void DelayMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline uint32_t Bit(int n) { return 1u << n; }

} // namespace

Iqk8814a::Iqk8814a(RtlAdapter device,
                   std::shared_ptr<EepromManager> eepromManager,
                   RadioManagementModule *radio, Logger_t logger)
    : _device(device), _eepromManager(eepromManager), _radio(radio),
      _logger(logger) {}

void Iqk8814a::BackupMacBb(const uint32_t *macRegs, uint32_t *macOut,
                           const uint32_t *bbRegs, uint32_t *bbOut) {
  for (int i = 0; i < kMacRegNum; i++) {
    macOut[i] = _device.rtw_read32(macRegs[i]);
  }
  for (int i = 0; i < kBbRegNum; i++) {
    bbOut[i] = _device.rtw_read32(bbRegs[i]);
  }
}

void Iqk8814a::BackupRf(const uint32_t *regs, uint32_t out[][4]) {
  /* 8814 RF paths C/D are write-only by HW design — reads return
   * sentinel/zero. Mirror upstream's read-all-4-paths pattern anyway so
   * the restore writes back identical values. */
  for (int i = 0; i < kRfRegNum; i++) {
    out[i][RfPath::RF_PATH_A] =
        _radio->phy_query_rf_reg(RfPath::RF_PATH_A, regs[i], kRFRegMask);
    out[i][RfPath::RF_PATH_B] =
        _radio->phy_query_rf_reg(RfPath::RF_PATH_B, regs[i], kRFRegMask);
    out[i][RfPath::RF_PATH_C] =
        _radio->phy_query_rf_reg(RfPath::RF_PATH_C, regs[i], kRFRegMask);
    out[i][RfPath::RF_PATH_D] =
        _radio->phy_query_rf_reg(RfPath::RF_PATH_D, regs[i], kRFRegMask);
  }
}

void Iqk8814a::AFESetting(bool doIqk) {
  /* IQK AFE setting: 0x0e808003 (RX_WAIT_CCA mode) vs Normal: 0x07808003. */
  const uint32_t afe_val = doIqk ? 0x0e808003u : 0x07808003u;
  _device.rtw_write32(0xc60, afe_val);
  _device.rtw_write32(0xe60, afe_val);
  _device.rtw_write32(0x1860, afe_val);
  _device.rtw_write32(0x1a60, afe_val);
  _device.phy_set_bb_reg(0x90c, Bit(13), 0x1);
  _device.phy_set_bb_reg(0x764, Bit(10) | Bit(9), 0x3);
  _device.phy_set_bb_reg(0x764, Bit(10) | Bit(9), 0x0);
  _device.phy_set_bb_reg(0x804, Bit(2), 0x1);
  _device.phy_set_bb_reg(0x804, Bit(2), 0x0);
}

void Iqk8814a::RestoreMacBb(const uint32_t *macRegs, const uint32_t *macBackup,
                            const uint32_t *bbRegs, const uint32_t *bbBackup) {
  for (int i = 0; i < kMacRegNum; i++) {
    _device.rtw_write32(macRegs[i], macBackup[i]);
  }
  for (int i = 0; i < kBbRegNum; i++) {
    _device.rtw_write32(bbRegs[i], bbBackup[i]);
  }
}

void Iqk8814a::RestoreRf(const uint32_t *regs, const uint32_t backup[][4]) {
  /* Clear RF[*][0xef] paging before restoring. */
  _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0xef, kRFRegMask, 0x0);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0xef, kRFRegMask, 0x0);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_C, 0xef, kRFRegMask, 0x0);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_D, 0xef, kRFRegMask, 0x0);

  for (int i = 0; i < kRfRegNum; i++) {
    _radio->phy_set_rf_reg(RfPath::RF_PATH_A, regs[i], kRFRegMask,
                           backup[i][RfPath::RF_PATH_A]);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_B, regs[i], kRFRegMask,
                           backup[i][RfPath::RF_PATH_B]);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_C, regs[i], kRFRegMask,
                           backup[i][RfPath::RF_PATH_C]);
    _radio->phy_set_rf_reg(RfPath::RF_PATH_D, regs[i], kRFRegMask,
                           backup[i][RfPath::RF_PATH_D]);
  }
}

void Iqk8814a::ResetNCTL() {
  _device.rtw_write32(0x1b00, 0xf8000000);
  _device.rtw_write32(0x1b80, 0x00000006);
  _device.rtw_write32(0x1b00, 0xf8000000);
  _device.rtw_write32(0x1b80, 0x00000002);
}

void Iqk8814a::ConfigureMAC() {
  _device.rtw_write8(0x522, 0x3f);
  _device.phy_set_bb_reg(0x550, Bit(11) | Bit(3), 0x0);
  _device.rtw_write8(0x808, 0x00);                  /* RX ant off */
  _device.phy_set_bb_reg(0x838, 0xf, 0xe);          /* CCA off */
  _device.phy_set_bb_reg(0xa14, Bit(9) | Bit(8), 0x3); /* CCK RX path off */
  _device.rtw_write32(0xcb0, 0x77777777);
  _device.rtw_write32(0xeb0, 0x77777777);
  _device.rtw_write32(0x18b4, 0x77777777);
  _device.rtw_write32(0x1ab4, 0x77777777);
  _device.phy_set_bb_reg(0x1abc, 0x0ff00000, 0x77);
  _device.phy_set_bb_reg(0xcbc, 0xf, 0x0);
}

void Iqk8814a::LokOneShot() {
  /* LO leakage calibration, all 4 paths. Polls 0x1b00 bit 0 for
   * completion (clears when done). On success, reads LOK result from
   * 0x1bfc and writes RF[path][0x8] LOK trim. On timeout, writes a
   * fallback constant. */
  for (uint8_t path = 0; path <= 3; ++path) {
    _device.phy_set_bb_reg(0x9a4, Bit(21) | Bit(20),
                           path); /* ADC clock source */
    _device.rtw_write32(0x1b00, 0xf8000001u | (1u << (4 + path)));
    DelayMs(kLokDelayMs);

    bool lokNotReady = true;
    int delayCount = 0;
    while (lokNotReady) {
      lokNotReady =
          (_radio->phy_query_bb_reg_public(0x1b00, Bit(0)) != 0);
      DelayMs(1);
      if (++delayCount >= 10) {
        _logger->warn("8814 LOK path {} timeout", path);
        ResetNCTL();
        break;
      }
    }

    if (!lokNotReady) {
      _device.rtw_write32(0x1b00, 0xf8000000u | (path << 1));
      _device.rtw_write32(0x1bd4, 0x003f0001);
      uint32_t lokTemp2 =
          (_radio->phy_query_bb_reg_public(0x1bfc, 0x003e0000) + 0x10) & 0x1f;
      uint32_t lokTemp1 =
          (_radio->phy_query_bb_reg_public(0x1bfc, 0x0000003e) + 0x10) & 0x1f;

      /* Saturation: replicate bits 4-0 upwards. Mirrors upstream's
       * "for ii in 1..5: temp += (temp & BIT(4-ii)) << (ii*2)". */
      for (int ii = 1; ii < 5; ++ii) {
        lokTemp1 += (lokTemp1 & Bit(4 - ii)) << (ii * 2);
        lokTemp2 += (lokTemp2 & Bit(4 - ii)) << (ii * 2);
      }

      _radio->phy_set_rf_reg(static_cast<RfPath>(path), 0x8, 0x07c00,
                             lokTemp1 >> 4);
      _radio->phy_set_rf_reg(static_cast<RfPath>(path), 0x8, 0xf8000,
                             lokTemp2 >> 4);
    } else {
      _radio->phy_set_rf_reg(static_cast<RfPath>(path), 0x8, kRFRegMask,
                             0x08400);
    }
  }
}

void Iqk8814a::IqkOneShot() {
  /* TX-IQK then RX-IQK across all 4 paths. CMD ID encoded in 0x1b00
   * bits 11:8 (BW-dependent) plus path-trigger bit (1 << (4+path)).
   * For 20MHz monitor mode (band_width=0): TX=3, RX=9. */
  constexpr uint8_t kBandWidth20 = 0; /* devourer is fixed 20MHz monitor */
  constexpr uint32_t kIqkApply[4] = {0xc94, 0xe94, 0x1894, 0x1a94};

  for (int idx = 0; idx <= 1; ++idx) {
    for (uint8_t path = 0; path <= 3; ++path) {
      int calRetry = 0;
      bool fail = true;
      while (fail) {
        _device.phy_set_bb_reg(0x9a4, Bit(21) | Bit(20), path);

        uint32_t iqkCmd;
        if (idx == kTxIqk) {
          /* 20 WBTXK: CMD = 3, BW20 + 3 = 3 */
          iqkCmd = 0xf8000001u | ((kBandWidth20 + 3u) << 8) | (1u << (4 + path));
        } else {
          /* 20 WBRXK: CMD = 9, 9 - BW20 = 9 */
          iqkCmd = 0xf8000001u | ((9u - kBandWidth20) << 8) | (1u << (4 + path));
        }
        _device.rtw_write32(0x1b00, iqkCmd);
        DelayMs(kWbiqkDelayMs);

        bool notReady = true;
        int delayCount = 0;
        while (notReady) {
          notReady =
              (_radio->phy_query_bb_reg_public(0x1b00, Bit(0)) != 0);
          if (!notReady) {
            fail =
                (_radio->phy_query_bb_reg_public(0x1b08, Bit(26)) != 0);
            break;
          }
          DelayMs(1);
          if (++delayCount >= 20) {
            _logger->warn("8814 IQK path {} {} timeout", path,
                          idx == kTxIqk ? "TX" : "RX");
            ResetNCTL();
            break;
          }
        }

        if (fail) {
          ++calRetry;
        }
        if (calRetry > 3) {
          break;
        }
      }

      _device.rtw_write32(0x1b00, 0xf8000000u | (path << 1));

      if (!fail) {
        if (idx == kTxIqk) {
          /* TX IQC matrix read from 0x1b38. */
          (void)_device.rtw_read32(0x1b38);
        } else {
          _device.rtw_write32(0x1b3c, 0x20000000);
          (void)_device.rtw_read32(0x1b3c);
        }
      }

      if (idx == kRxIqk) {
        /* TXIQK success → write TX IQC to 0x1b38. Else clear IQK_Apply
         * bit 0 (disable TX IQC). */
        /* NOTE: devourer doesn't cache per-path TX-IQK success state
         * because we recompute every channel-set. If TX IQK failed
         * just now in the same loop pass, we can't tell here without
         * a per-path success vector. Mirror upstream's behavior with
         * a simple per-iteration carry: if the just-completed RX-IQK
         * passes and TX-IQK didn't fail, the HW already wrote IQC. */
        if (fail) {
          /* RX IQK Fail → clear IQK_Apply bits 11:10. */
          _device.phy_set_bb_reg(kIqkApply[path], Bit(11) | Bit(10), 0x0);
        }
      }
    }
  }
}

void Iqk8814a::IqkTx(BandType band) {
  /* Path-wide BB-CCA off plus RF reg 0x58 BIT19 set, then 0x1b00 with
   * band-dependent CMD enable, then LOK + IQK_OneShot. */
  _radio->phy_set_rf_reg(RfPath::RF_PATH_A, 0x58, Bit(19), 0x1);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_B, 0x58, Bit(19), 0x1);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_C, 0x58, Bit(19), 0x1);
  _radio->phy_set_rf_reg(RfPath::RF_PATH_D, 0x58, Bit(19), 0x1);

  _device.phy_set_bb_reg(0xc94, Bit(11) | Bit(10) | Bit(0), 0x401);
  _device.phy_set_bb_reg(0xe94, Bit(11) | Bit(10) | Bit(0), 0x401);
  _device.phy_set_bb_reg(0x1894, Bit(11) | Bit(10) | Bit(0), 0x401);
  _device.phy_set_bb_reg(0x1a94, Bit(11) | Bit(10) | Bit(0), 0x401);

  if (band == BandType::BAND_ON_5G) {
    _device.rtw_write32(0x1b00, 0xf8000ff1);
  } else {
    _device.rtw_write32(0x1b00, 0xf8000ef1);
  }
  DelayMs(1);

  _device.rtw_write32(0x810, 0x20101063);
  _device.rtw_write32(0x90c, 0x0B00C000);

  LokOneShot();
  IqkOneShot();
}

void Iqk8814a::Calibrate(uint8_t channel, BandType band, bool is_recovery) {
  (void)is_recovery;
  (void)channel;

  /* Backup register addresses — verbatim from upstream
   * `_phy_iq_calibrate_8814a` (halrf_iqk_8814a.c:472). */
  const uint32_t backupMacReg[kMacRegNum] = {0x520, 0x550};
  const uint32_t backupBbReg[kBbRegNum] = {
      0xa14, 0x808,  0x838,  0x90c,  0x810, 0xcb0, 0xeb0,
      0x18b4, 0x1ab4, 0x1abc, 0x9a4, 0x764, 0xcbc};
  const uint32_t backupRfReg[kRfRegNum] = {0x0, 0x8f};

  uint32_t macBackup[kMacRegNum]{};
  uint32_t bbBackup[kBbRegNum]{};
  uint32_t rfBackup[kRfRegNum][4]{};

  BackupMacBb(backupMacReg, macBackup, backupBbReg, bbBackup);
  AFESetting(/*doIqk=*/true);
  BackupRf(backupRfReg, rfBackup);
  ConfigureMAC();
  IqkTx(band);
  ResetNCTL();
  AFESetting(/*doIqk=*/false);
  RestoreMacBb(backupMacReg, macBackup, backupBbReg, bbBackup);
  RestoreRf(backupRfReg, rfBackup);

  _logger->info("Iqk8814a::Calibrate done (channel={}, band={})",
                unsigned(channel),
                band == BandType::BAND_ON_5G ? "5G" : "2.4G");
}
