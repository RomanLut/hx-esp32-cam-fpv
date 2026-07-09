/* Aggregate adapter-capability report — what the opened radio actually IS and
 * what it can do, resolved from the chip identity devourer already determines at
 * construction (SYS_CFG2 chip-id + USB PID + EFUSE RF-type). A dependent app
 * (OpenIPC-FPV tooling, a link manager, a test harness) can enumerate the RF
 * frequency coverage, channel widths, spatial-stream / chain count, and the
 * per-family feature levers (per-packet TX power, narrowband, fast retune)
 * without hardcoding a chip table of its own or scraping bring-up logs.
 *
 * This is the identity+radio superset of the narrower GetTxCaps (modulation
 * features) and GetTxPowerCaps (power-knob range), which it composes by value so
 * there is one source of truth per fact. Like those, it is STATIC — resolved at
 * construction, safe from any thread, callable before Init/InitWrite. The live
 * "which antennas look connected" question is deliberately NOT here (it needs
 * traffic); see IRtlDevice::GetActiveRxPaths / ActiveRxPaths in RxQuality.h.
 *
 * FREQUENCY COVERAGE. The 5 GHz synthesizer on these parts tunes well past the
 * regulatory UNII channels (the vendor rtl88x2bu "monitor_chan_override" hack:
 * chan 16..253, freq = 5000 + 5*chan, ~5080..6165 MHz). devourer drives that
 * whole span in monitor mode; `tune_5g` reports the tunable range while
 * `characterized_5g` reports the sub-range backed by the generated txpwr_lmt /
 * EFUSE PG tables (outside it, per-channel constants and TX power are
 * extrapolated from the nearest characterized channel — the radio still tunes,
 * but power is uncalibrated). Regulatory compliance is the CALLER's problem: the
 * library enforces none.
 */
#ifndef DEVOURER_ADAPTER_CAPS_H
#define DEVOURER_ADAPTER_CAPS_H

#include <cstdint>

#include "TxCaps.h"
#include "TxPower.h"

namespace devourer {

enum class ChipGeneration : uint8_t { Unknown = 0, Jaguar1, Jaguar2, Jaguar3 };

inline const char *generation_name(ChipGeneration g) {
  switch (g) {
  case ChipGeneration::Jaguar1:
    return "jaguar1";
  case ChipGeneration::Jaguar2:
    return "jaguar2";
  case ChipGeneration::Jaguar3:
    return "jaguar3";
  default:
    return "unknown";
  }
}

/* Supported channel widths, one bit per width (MHz). A mask, not a max, because
 * the set is not contiguous per family: Jaguar2/Jaguar3 add 5/10 MHz narrowband
 * BELOW the 20/40/80 all AC families do. */
constexpr uint8_t kBw5 = 1u << 0;
constexpr uint8_t kBw10 = 1u << 1;
constexpr uint8_t kBw20 = 1u << 2;
constexpr uint8_t kBw40 = 1u << 3;
constexpr uint8_t kBw80 = 1u << 4;

/* J1 does 20/40/80; J2 and J3 add the 5/10 MHz narrowband re-clock (J2 packs
 * the ADC/DAC clock word into 0x8ac, J3 into 0x9b0/0x9b4 — same RF-stays-20MHz
 * concept; the J2 8822B additionally needs an RF18 re-latch edge after the
 * re-clock). J1 has no vendor narrowband reference (the rtl8812au trees carry
 * only dead enum values). Pure; unit-tested in tests/adapter_caps_selftest.cpp. */
inline uint8_t bw_mask_for_generation(ChipGeneration g) {
  const uint8_t ac = kBw20 | kBw40 | kBw80;
  return g == ChipGeneration::Jaguar1 ? ac
         : g == ChipGeneration::Unknown ? 0
                                        : (ac | kBw5 | kBw10);
}

/* A tunable / characterized frequency span (MHz). valid=false = band absent. */
struct BandRange {
  bool valid = false;
  uint16_t min_mhz = 0;
  uint16_t max_mhz = 0;
};

struct AdapterCaps; /* fwd for the frequency-range helper below */

/* Fill the 2.4 + 5 GHz tunable / characterized spans shared by all three Jaguar
 * families. tune_* = what the synthesizer reaches in monitor mode; the 5 GHz
 * span runs past the UNII channels (the extended synth ~5080..6165 MHz, chan
 * 16..253 — per-chip lock varies, validated on the bench). characterized_* =
 * the sub-range the generated txpwr_lmt / EFUSE PG tables cover; outside it TX
 * power is extrapolated from the nearest characterized channel. Defined
 * out-of-line below (needs the full AdapterCaps). */
inline void set_standard_freq_ranges(AdapterCaps &c);

struct AdapterCaps {
  bool supported = false; /* false on a generation that hasn't wired this */

  /* --- identity --- */
  const char *chip_name = "";      /* silicon die, no bus suffix: "RTL8822C" */
  const char *marketing_names = "";/* alias list, e.g. "RTL8812CU/RTL8822CU" */
  uint8_t chip_id = 0;             /* SYS_CFG2 (0x00FC) dispatch byte */
  ChipGeneration generation = ChipGeneration::Unknown;
  const char *variant = "";        /* per-family variant tag ("C8822B", ICType) */
  const char *transport = "";      /* "usb" | "pcie" */

  /* --- chains (EFUSE RF-type on Jaguar1; per-variant on Jaguar2/3) --- */
  uint8_t tx_chains = 0;
  uint8_t rx_chains = 0;

  /* --- composed sub-caps (single source of truth) --- */
  TxCaps tx;          /* = GetTxCaps() — modulation features */
  TxPowerCaps txpwr;  /* = GetTxPowerCaps() — power-knob range/step */

  /* --- bandwidth + frequency --- */
  uint8_t bw_mask = 0;                    /* kBw* bits */
  BandRange tune_2g4, tune_5g;            /* synthesizer-tunable spans */
  BandRange characterized_2g4, characterized_5g; /* txpwr-table-backed spans */

  /* --- feature flags --- */
  bool per_packet_txpower = false; /* Jaguar2 descriptor TXPWR_OFSET LUT only */
  bool narrowband_ok = false;      /* 5/10 MHz re-clock (Jaguar2/Jaguar3) */
  uint8_t xtal_cap_max = 0;        /* crystal-cap trim range top (0 = no trim;
                                    * 0x3f on Jaguar1/2, 0x7f on Jaguar3) */
  uint8_t xtal_cap_default = 0;    /* efuse/default crystal-cap code */
  bool fastretune_ok = false;      /* lean FastRetune override exists */
  bool per_chain_rssi = false;     /* frame parser fills per-chain rssi (>=2ch) */
};

inline void set_standard_freq_ranges(AdapterCaps &c) {
  c.tune_2g4 = BandRange{true, 2412, 2484};
  c.characterized_2g4 = BandRange{true, 2412, 2484};
  c.tune_5g = BandRange{true, 5080, 6165};
  c.characterized_5g = BandRange{true, 5180, 5825};
}

} // namespace devourer

#endif /* DEVOURER_ADAPTER_CAPS_H */
