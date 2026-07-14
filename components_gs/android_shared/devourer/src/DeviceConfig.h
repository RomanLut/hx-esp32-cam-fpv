#pragma once

/* DeviceConfig — construction-time configuration, passed to
 * WiFiDriver::CreateRtlDevice (defaulted: CreateRtlDevice(handle) gives stock
 * behaviour). Fields are fixed for the device's lifetime; knobs that change
 * mid-session are runtime setters on IRtlDevice / the concrete device classes
 * (SetTxMode, SetTxPowerOffsetQdb, SetRxPathMask, SetCcaMode, ...).
 *
 * The example binaries populate this from DEVOURER_* environment variables via
 * examples/common/env_config.{h,cpp}; each field's `env:` tag names its
 * spelling in that translator and its value grammar. */

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace devourer {

/* 6-byte MAC address carrier (replaces the per-site uint8_t[6] + sscanf). */
struct MacAddr {
  std::array<uint8_t, 6> bytes{};
  const uint8_t *data() const { return bytes.data(); }
};

/* "aa:bb:cc:dd:ee:ff" (hex, case-insensitive) -> MacAddr; nullopt on any
 * malformed input. */
inline std::optional<MacAddr> parse_mac(std::string_view s) {
  MacAddr m;
  unsigned v[6];
  char tail;
  /* string_view is not NUL-terminated in general — bounce through a string. */
  std::string z(s);
  if (std::sscanf(z.c_str(), "%x:%x:%x:%x:%x:%x%c", &v[0], &v[1], &v[2], &v[3],
                  &v[4], &v[5], &tail) != 6)
    return std::nullopt;
  for (int i = 0; i < 6; ++i) {
    if (v[i] > 0xFF)
      return std::nullopt;
    m.bytes[i] = static_cast<uint8_t>(v[i]);
  }
  return m;
}

/* Jaguar-1 regulatory domain for the efuse TX-power limit tables. */
enum class Regulation : uint8_t { FCC = 0, ETSI = 1, MKK = 2, WW = 3 };

/* 8814A firmware-download path select. */
enum class Fwdl8814Path : uint8_t {
  KernelBracket, /* kernel-faithful bracket (default) */
  Rtw88,         /* legacy rtw88-mimic sequence (A/B fallback) */
};

struct DeviceConfig {
  /* ---- RX ------------------------------------------------------------- */
  struct Rx {
    /* env: DEVOURER_RX_KEEP_CORRUPTED — pass frames that fail the 802.11 FCS
     * (CRC32) or decryption-ICV check up to the host instead of dropping them
     * at the WMAC filter (sets RCR ACRC32|AICV). Jaguar1 + Jaguar2. */
    bool keep_corrupted = false;
    /* env: DEVOURER_TX_WITH_RX — Jaguar3 only: keep the RX filters open and
     * enable the RX path during a TX (InitWrite) bring-up so StartRxLoop can
     * run on the same handle. Must be decided before InitWrite (retrofitting
     * RX onto a TX-only bring-up is unreliable on this generation). */
    bool enable_with_tx = false;
    /* env: DEVOURER_RX_CSI_MASK — "<f_lo>[-<f_hi>][/wgt]" MHz: de-weight a
     * frequency range in the RX equalizer's per-tone CSI mask (ToneMask.h).
     * Applied when the RX loop starts; a channel switch reverts it. */
    std::optional<std::string> csi_mask;
    /* env: DEVOURER_RX_NBI — "<f_mhz>": arm the narrowband-interference notch
     * at one in-channel frequency (LUT-quantized). Same lifecycle as
     * csi_mask. */
    std::optional<std::string> nbi;
    /* env: DEVOURER_RX_PATHS — Jaguar1 RX-chain mask spec: "0xNN" applies one
     * mask (sticky across channel sets, see SetRxPathMask); the toggle form
     * "0xAA:0xBB[:0xCC]@<ms>" cycles masks on a timer thread. */
    std::optional<std::string> path_spec;
    /* env: DEVOURER_RX_URBS — Jaguar1 async bulk-IN URB queue depth
     * (default 8, clamped to >= 1). */
    std::optional<int> urbs;
    /* env: DEVOURER_8821C_NO_PHYST (inverted) — 8821C: prepend the 32-byte
     * PHY-status to RX frames (per-frame RSSI/SNR/EVM). Disable only for the
     * leanest possible RX path. */
    bool phy_status_8821c = true;
    /* env: DEVOURER_IGI — Jaguar2 fixed initial-gain index override, 7 bits
     * (unset = 0x40, the FA-rate-validated default). */
    std::optional<uint8_t> igi;
  } rx;

  /* ---- TX ------------------------------------------------------------- */
  struct Tx {
    /* env: DEVOURER_TX_EP — bulk-OUT endpoint override (unset = first
     * descriptor-walked bulk-OUT endpoint, fallback 0x02). */
    std::optional<uint8_t> ep;
    /* env: DEVOURER_TX_TIMEOUT_MS — TX bulk-OUT transfer timeout (unset =
     * USB_TIMEOUT). */
    std::optional<unsigned> timeout_ms;
    /* env: DEVOURER_TX_LEGACY_8812_DESC — 8814A: keep the legacy 8812-style
     * TX-descriptor bits instead of the 8814-native layout. */
    bool legacy_8812_desc = false;
    /* env: DEVOURER_TX_PWR — Jaguar2: flat TXAGC index (0..63) applied at the
     * end of InitWrite via SetTxPowerIndexOverride (composes with the offset
     * knob and shows up in GetTxPowerState). Debug/SDR-visibility knob. */
    std::optional<int> power_index;
    /* env: DEVOURER_TX_RF_BW — Jaguar3 40 MHz: force the 0x9b0 TX_RF_BW field
     * (2 bits; kernel-stall study knob). */
    std::optional<uint8_t> rf_bw;
    /* env: DEVOURER_CW_TONE / DEVOURER_CW_TONE_GAIN — radiate a bare RF LO
     * carrier (MP single-tone) after bring-up; gain = RF 0x00[4:0], 0..31.
     * Runtime equivalent: StartCwTone/StopCwTone on the concrete device. */
    bool cw_tone = false;
    uint8_t cw_tone_gain = 0;
  } tx;

  /* ---- Beamforming (bring-up-time arming; see docs/beamforming-*.md) --- */
  struct Bf {
    /* env: DEVOURER_BF_ARM_SOUNDER — arm the MAC's hardware sounding engine
     * (beamformer side) so a descriptor-marked NDPA is followed by a
     * hardware-generated NDP. */
    bool arm_sounder = false;
    /* env: DEVOURER_BF_ARM_SOUNDER=<mac> — Jaguar2/3 also program the self-MAC
     * (0x0610) the sounding engine matches the NDPA TA against. Unset = bare
     * arming (Jaguar-1 semantics: MAC left unprogrammed). */
    std::optional<MacAddr> sounder_self_mac;
    /* env: DEVOURER_BF_ARM_BFEE=<mac> — arm the hardware CSI responder
     * (beamformee side) for NDPA+NDP from this beamformer MAC. */
    std::optional<MacAddr> beamformee_of;
    /* env: DEVOURER_BF_ARM_BFEE_MU — upgrade the responder to MU beamformee
     * (per-subcarrier delta-SNR report). Jaguar2/3, needs beamformee_of. */
    bool mu = false;
    /* env: DEVOURER_BF_TXBF=<mac> — Jaguar3: configure the beamformer entry
     * for closed-loop TX-beamforming apply toward this peer (the apply itself
     * gates on the first CBR seen by the RX loop; needs rx.enable_with_tx). */
    std::optional<MacAddr> txbf_peer;
    /* env: DEVOURER_TX_NDPA — 0 = off; 1 = mark every injected frame NDPA
     * (self-sounding capture); N>1 = every Nth frame (Jaguar3 periodic
     * re-sounding; Jaguar1/2 treat any nonzero as on). */
    int ndpa_period = 0;
  } bf;

  /* ---- Bring-up / calibration experiment gates ------------------------ */
  struct Tuning {
    bool skip_iqk = false;          /* env: DEVOURER_SKIP_IQK (J2, J3) */
    bool force_iqk = false;         /* env: DEVOURER_FORCE_IQK (J1) */
    bool disable_iqk = false;       /* env: DEVOURER_DISABLE_IQK (J1) */
    bool skip_txpwr = false;        /* env: DEVOURER_SKIP_TXPWR (J1, J2) */
    bool skip_txgapk = false;       /* env: DEVOURER_SKIP_TXGAPK (8822E) */
    bool skip_trx_reassert = false; /* env: DEVOURER_SKIP_TRX_REASSERT (J2) */
    bool skip_rfe_init = false;     /* env: DEVOURER_SKIP_RFEINIT (J2) */
    bool skip_coex = false;         /* env: DEVOURER_SKIP_COEX (J2) */
    bool skip_dig = false;          /* env: DEVOURER_SKIP_DIG (J2) */
    /* env: DEVOURER_THERMAL_TRACK — Jaguar2 thermal TX-power tracking
     * (default ON; "0" disables). A ~2 s tick reads the RF 0x42 thermal
     * meter, computes the delta vs the efuse baseline, and writes the vendor
     * MIX_MODE swing compensation (0xc94/0xe94 TXAGC + 0xc1c/0xe1c BB scale)
     * so on-air power holds flat as the PA heats over a sustained TX link. */
    bool thermal_track = true;
    /* env: DEVOURER_DIS_CCA — Jaguar3 EDCCA-disable at bring-up (before the
     * coex thread starts). Runtime equivalent: SetCcaMode. */
    bool disable_cca = false;
    /* env: DEVOURER_RFE — Jaguar2 RFE type override (antenna/LNA switch
     * variant; unset = efuse, blank efuse falls back per vendor). */
    std::optional<uint8_t> rfe_type;
    /* env: DEVOURER_NB_DAC — 5/10 MHz divider-mapping experiments only.
     * Jaguar3: force the 3-bit DAC-divider code (0x9b4[10:8]). Jaguar2:
     * force the 0x8ac DAC clock field — bits [1:0] -> 0x8ac[21:20],
     * bit 2 -> 0x8ac[28]. */
    std::optional<uint8_t> nb_dac;
    /* env: DEVOURER_NB_ADC — Jaguar2 5/10 MHz: force the 0x8ac ADC clock
     * field — bits [1:0] -> 0x8ac[9:8], bit 2 -> 0x8ac[16] (divider-mapping
     * experiments only). */
    std::optional<uint8_t> nb_adc;
    /* env: DEVOURER_XTAL_CAP — crystal-cap trim code applied at the end of
     * bring-up (IRtlDevice::SetXtalCap). The CFO lever for narrowband at the
     * edge of its budget; unset = efuse/default. Raw code, 0..0x3f (Jaguar1/2)
     * or 0..0x7f (Jaguar3). */
    std::optional<uint8_t> xtal_cap;
    /* env: DEVOURER_CFO_TRACK — closed-loop CFO tracking (Jaguar3): auto-trim
     * the crystal cap from the RX-measured per-frame CFO, tracking warm-up
     * drift so narrowband stays locked (issue #217). Off by default. */
    bool cfo_track = false;
    /* env: DEVOURER_REGULATION — Jaguar1 regulatory domain override for the
     * TX-power limit tables (unset = efuse). */
    std::optional<Regulation> regulation;
    /* env: DEVOURER_ENABLE_TXPWR_BY_RATE — Jaguar1: apply the efuse PG-table
     * per-rate offsets on top of the base index. */
    bool txpwr_by_rate = false;
    /* env: DEVOURER_PHYDM_WATCHDOG — Jaguar1: run the optional periodic phydm
     * watchdog (FA stats + DIG) thread. */
    bool phydm_watchdog = false;
    /* env: DEVOURER_8814_FWDL / DEVOURER_8814_FWDL_CHUNK — 8814A firmware
     * download path + reserved-page chunk size (validated 64..MAX at the use
     * site; out-of-range values are ignored). */
    Fwdl8814Path fwdl_8814 = Fwdl8814Path::KernelBracket;
    std::optional<uint32_t> fwdl_8814_chunk;
  } tuning;

  /* ---- Diagnostics output --------------------------------------------- */
  struct Debug {
    /* env: DEVOURER_DUMP_CANARY — dump the per-chip BB/MAC/RF canary register
     * set after every channel set / fast retune (kernel-parity diffing). */
    bool dump_canary = false;
    /* env: DEVOURER_BB_DUMP — Jaguar2: full BB/RF dump in rtw_proc format. */
    bool bb_dump = false;
    /* env: DEVOURER_EFUSE_DUMP — Jaguar2: dump the efuse logical map. */
    bool efuse_dump = false;
    /* env: DEVOURER_LOG_WRITES — emit every vendor register write as a
     * debug.wreg event (addr/width/val mirror tests/decode_wseq.py). */
    bool log_writes = false;
    /* env: DEVOURER_LOG_TXPWR — Jaguar1: log the per-channel/per-rate TX-power
     * derivation. */
    bool log_txpwr = false;
    /* env: DEVOURER_HOP_PROF — per-stage FastRetune timing on stderr. */
    bool hop_prof = false;
    /* env: DEVOURER_GAINTAB_DBG — 8822E: log the TXGAPK gain-table backup. */
    bool gaintab_dbg = false;
    /* env: DEVOURER_REPLAY_WSEQ — golden-init replay (Jaguar2): path to a
     * register write-sequence file (lines: "<addr_hex> <width 1|2|4>
     * <val_hex>", e.g. extracted from a kernel-driver usbmon capture).
     * Applied verbatim at the end of Init, OVERRIDING devourer's own
     * configuration — a debugging lever for hardware-diffing devourer
     * against the vendor driver's end state. */
    std::string replay_wseq;
  } debug;

  /* ---- USB / process environment -------------------------------------- */
  struct Usb {
    /* env: TMPDIR — directory for the
     * devourer-usb-*.lock files. Empty = "/tmp". */
    std::string lock_dir;
  } usb;

  /* ---- PCIe (DEVOURER_PCIE builds; see src/PcieTransport.h) ------------ */
  struct Pcie {
    /* env: DEVOURER_PCIE_RX_POLL_US — RX ring hardware-index poll interval in
     * microseconds (unset = the transport default, 200). Demo-side only: the
     * demos fold it into PcieTransport::Config before Open(); device selection
     * (DEVOURER_PCIE_BDF) is likewise demo-local, like USB device selection. */
    std::optional<int> rx_poll_us;
  } pcie;
};

} // namespace devourer
