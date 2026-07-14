/* Adapter-health probes + classifier — the "is my dongle dying" detector.
 *
 * Motivated by a field failure mode measured on a degrading RTL8812AU
 * (OpenIPC/devourer#205): the chip enumerates fine, USB transfers are clean,
 * init completes green — and the radio is stone-deaf, because
 *
 *   (1) the EFUSE read path returns STOCHASTIC garbage — a different
 *       EEPROM ID / calibration map on every physical read (a healthy chip
 *       returns byte-identical content every time), so the driver configures
 *       the wrong RFE type / PA / LNA tables; and
 *   (2) the 8051 MCU never boots firmware — the download checksum report
 *       never asserts (non-fatal for Jaguar1 monitor RX, so nothing aborts).
 *
 * Both are invisible to "did init succeed" checks. The probes here make them
 * measurable: N fresh physical EFUSE reads cross-compared for stability +
 * validity, the firmware-boot outcome of the last bring-up, and an RX smoke
 * count. The classifier maps those to a plain verdict + reason bits.
 *
 * IRtlDevice carries the probe entry points (ProbeEfuseStability,
 * GetFwBootStatus); examples/doctor is the reference consumer.
 */
#ifndef DEVOURER_ADAPTER_HEALTH_H
#define DEVOURER_ADAPTER_HEALTH_H

#include <cstdint>
#include <cstring>
#include <vector>

namespace devourer {

/* Every Realtek WLAN EFUSE logical map opens with this ID; anything else on a
 * programmed retail adapter means the read is corrupt (a BLANK map — all
 * 0xFF, id 0xFFFF — is the separate "autoload fail" case: legitimate on
 * bare dev boards, odd on retail dongles). */
constexpr uint16_t kRtlEepromId = 0x8129;

/* Result of N back-to-back physical EFUSE logical-map reads (not the cached
 * shadow — each pass re-runs the efuse controller read sequence). */
struct EfuseStability {
  bool supported = false; /* family wired + chip brought up when probed */
  int reads = 0;          /* passes performed */
  int mismatched_reads = 0; /* passes whose content != pass #0 */
  int invalid_id_reads = 0; /* passes with EEPROM ID != kRtlEepromId */
  uint16_t eeprom_id = 0;   /* ID from the last pass */
  uint16_t map_len = 0;     /* logical map bytes compared per pass */
  uint16_t first_mismatch_off = 0xFFFF; /* first differing offset, if any */
};

/* Shared probe loop behind IRtlDevice::ProbeEfuseStability — each generation
 * supplies its own fresh-physical-map reader as `read_map(uint8_t *buf)`
 * (return false on transport failure) and this does the cross-compare. */
template <typename ReadFn>
inline EfuseStability ProbeEfuseStabilityImpl(ReadFn &&read_map,
                                              uint16_t map_len, int reads) {
  EfuseStability st;
  if (reads < 1 || map_len < 2)
    return st;
  st.supported = true;
  st.map_len = map_len;
  std::vector<uint8_t> ref(map_len), cur(map_len);
  for (int i = 0; i < reads; ++i) {
    uint8_t *buf = (i == 0) ? ref.data() : cur.data();
    if (!read_map(buf))
      break;
    st.reads++;
    st.eeprom_id = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
    if (st.eeprom_id != kRtlEepromId)
      st.invalid_id_reads++;
    if (i > 0 && std::memcmp(ref.data(), cur.data(), map_len) != 0) {
      st.mismatched_reads++;
      if (st.first_mismatch_off == 0xFFFF) {
        for (uint16_t off = 0; off < map_len; ++off) {
          if (ref[off] != cur[off]) {
            st.first_mismatch_off = off;
            break;
          }
        }
      }
    }
  }
  return st;
}

/* Outcome of the most recent firmware download (populated during
 * Init/InitWrite). On Jaguar1 a failed FW boot does NOT abort init — monitor
 * RX runs without the 8051 — which is exactly why it needs surfacing. */
struct FwBootStatus {
  bool supported = false; /* family records this */
  bool attempted = false; /* a download ran this bring-up */
  bool checksum_ok = false;
  bool ready_ok = false; /* MCU signalled firmware-ready */
};

enum class AdapterVerdict {
  Unknown, /* nothing conclusive was checked */
  Healthy,
  Suspect, /* one soft anomaly — re-run, or supply known traffic */
  Failing, /* hard evidence of dying silicon */
};

/* Reason bits accompanying a verdict. */
enum : uint32_t {
  kAdapterInitFailed = 1u << 0,     /* bring-up aborted */
  kAdapterEfuseUnstable = 1u << 1,  /* reads disagree — the smoking gun */
  kAdapterEfuseIdInvalid = 1u << 2, /* stable but ID != 0x8129 (not blank) */
  kAdapterEfuseBlank = 1u << 3,     /* stable all-0xFF map (autoload fail) */
  kAdapterFwBootFailed = 1u << 4,   /* download attempted, MCU never ready */
  kAdapterRxDeafToTraffic = 1u << 5, /* 0 frames with a known traffic source */
  kAdapterRxSilent = 1u << 6, /* 0 frames, no traffic guarantee (soft) */
};

struct AdapterHealthInput {
  bool init_completed = false;
  EfuseStability efuse;
  FwBootStatus fw;
  bool rx_checked = false;
  /* Operator asserts a known traffic source on the listen channel (bench
   * flood / busy AP) — upgrades "heard nothing" from Suspect to Failing. */
  bool rx_traffic_expected = false;
  uint32_t rx_frames_ok = 0;  /* FCS-clean frames heard */
  uint32_t rx_frames_crc = 0; /* corrupt frames heard (informational) */
};

inline const char *AdapterVerdictName(AdapterVerdict v) {
  switch (v) {
  case AdapterVerdict::Healthy:
    return "HEALTHY";
  case AdapterVerdict::Suspect:
    return "SUSPECT";
  case AdapterVerdict::Failing:
    return "FAILING";
  default:
    return "UNKNOWN";
  }
}

/* Pure classifier — no I/O, unit-tested in tests/adapter_health_selftest.cpp.
 *
 * Grading (measured on the #205 dying unit vs its healthy twin):
 *   - Cross-read EFUSE instability is conclusive by itself: healthy silicon
 *     returns identical maps every read, cold or warm. → Failing.
 *   - A stable-but-invalid ID (not blank) is corrupt calibration → Failing
 *     when the FW also failed, else Suspect (one anomaly, re-run).
 *   - A blank map (all-0xFF / autoload fail) is legitimate on some dev
 *     boards → Suspect, look at the adapter's provenance.
 *   - FW-boot failure alone → Suspect (state can be transient); with any
 *     EFUSE anomaly → Failing (two independent subsystems = the signature).
 *   - Deaf RX only counts as Failing when the operator vouched for traffic
 *     (rx_traffic_expected); otherwise an RF-quiet room is indistinguishable
 *     → Suspect. High corrupt-frame ratios are NOT graded — distant ambient
 *     traffic legitimately decodes mostly-corrupt.
 */
inline AdapterVerdict ClassifyAdapterHealth(const AdapterHealthInput &in,
                                            uint32_t &reasons) {
  reasons = 0;
  bool checked = false;

  if (!in.init_completed) {
    reasons |= kAdapterInitFailed;
    /* Enrich the abort with the fw stage that died (fatal-DLFW generations
     * throw out of bring-up; the status still says checksum vs boot). */
    if (in.fw.supported && in.fw.attempted && !in.fw.ready_ok)
      reasons |= kAdapterFwBootFailed;
    return AdapterVerdict::Failing;
  }

  bool efuse_hard = false, efuse_soft = false;
  if (in.efuse.supported && in.efuse.reads > 0) {
    checked = true;
    const bool blank = in.efuse.mismatched_reads == 0 &&
                       in.efuse.invalid_id_reads == in.efuse.reads &&
                       in.efuse.eeprom_id == 0xFFFF;
    if (in.efuse.mismatched_reads > 0) {
      reasons |= kAdapterEfuseUnstable;
      efuse_hard = true;
    } else if (blank) {
      reasons |= kAdapterEfuseBlank;
      efuse_soft = true;
    } else if (in.efuse.invalid_id_reads > 0) {
      reasons |= kAdapterEfuseIdInvalid;
      efuse_soft = true;
    }
  }

  bool fw_bad = false;
  if (in.fw.supported && in.fw.attempted) {
    checked = true;
    if (!in.fw.ready_ok) {
      reasons |= kAdapterFwBootFailed;
      fw_bad = true;
    }
  }

  bool rx_hard = false, rx_soft = false;
  if (in.rx_checked) {
    checked = true;
    if (in.rx_frames_ok == 0) {
      if (in.rx_traffic_expected) {
        reasons |= kAdapterRxDeafToTraffic;
        rx_hard = true;
      } else {
        reasons |= kAdapterRxSilent;
        rx_soft = true;
      }
    }
  }

  if (!checked)
    return AdapterVerdict::Unknown;
  if (efuse_hard || rx_hard || (fw_bad && (efuse_soft || efuse_hard)) ||
      ((reasons & kAdapterEfuseIdInvalid) && fw_bad))
    return AdapterVerdict::Failing;
  if (efuse_soft || fw_bad || rx_soft)
    return AdapterVerdict::Suspect;
  return AdapterVerdict::Healthy;
}

} // namespace devourer

#endif /* DEVOURER_ADAPTER_HEALTH_H */
