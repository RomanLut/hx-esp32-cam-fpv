#include "TxMode.h"

#include "RateDefinitions.h"  // enum MGN_RATE

namespace devourer {

TxParams tx_mode_to_params(const TxMode& m) {
  TxParams p{};
  p.sgi = m.sgi ? 1 : 0;
  p.ldpc = m.ldpc ? 1 : 0;
  p.stbc = m.stbc ? 1 : 0;

  switch (m.bw_mhz) {
    case 40:  p.bwidth = CHANNEL_WIDTH_40; break;
    case 80:  p.bwidth = CHANNEL_WIDTH_80; break;
    /* The TX descriptor caps at 80 MHz (BWSettingOfDesc 0/1/2); map 160 down. */
    case 160: p.bwidth = CHANNEL_WIDTH_80; break;
    default:  p.bwidth = CHANNEL_WIDTH_20; break;
  }

  switch (m.mode) {
    case TxMode::Mode::HT: {
      const uint8_t mcs = m.ht_mcs <= 31 ? m.ht_mcs : 0;
      p.fixed_rate = static_cast<uint8_t>(MGN_MCS0 + mcs);
      p.vht = false;
      break;
    }
    case TxMode::Mode::VHT: {
      uint8_t nss = m.vht_nss < 1 ? 1 : (m.vht_nss > 4 ? 4 : m.vht_nss);
      uint8_t mcs = m.vht_mcs > 9 ? 9 : m.vht_mcs;
      p.fixed_rate =
          static_cast<uint8_t>(MGN_VHT1SS_MCS0 + (nss - 1) * 10 + mcs);
      p.vht = true;
      break;
    }
    case TxMode::Mode::Legacy:
    default:
      /* legacy_rate_500kbps already equals the MGN_* OFDM byte value. */
      p.fixed_rate = m.legacy_rate_500kbps;
      p.vht = false;
      break;
  }
  return p;
}

}  // namespace devourer
