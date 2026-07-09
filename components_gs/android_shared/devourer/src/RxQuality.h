/* Runtime RX link-quality feed — the consolidated per-window snapshot a
 * closed-loop adaptive-link controller (e.g. an alink / Fluke-style GS) reads
 * to steer the link, WITHOUT scraping the demo's stdout.
 *
 * devourer already exposes per-frame RSSI/SNR/EVM (the Packet callback) and
 * frame-free FA/CCA/IGI (GetRxEnergy), but the windowing + the LinkHealth
 * verdict lived only in examples/rx/main.cpp. This promotes that into the library: the
 * device aggregates per-frame RX quality internally, and GetRxQuality() returns
 * one struct fusing the frame aggregate, the frame-free energy, a passive
 * noise-floor estimate, and the plain-language verdict.
 *
 * NOISE FLOOR (passive): per decoded OFDM/HT frame, the effective noise+
 * interference floor the receiver sees is nf_dbm = rssi_dbm - snr_db =
 * (rssi_raw - 110) - snr_raw/2. It's a PASSIVE estimate — updated only when a
 * wanted frame arrives — but that is exactly the self-jamming quantity: raising
 * TX power on a near-field link raises reflections into the front end, SNR drops
 * while RSSI holds, so this rises (the "decrease txpower until NF returns to
 * normal" signal). Works on every family, including Jaguar3 where IGI can't
 * (no background DIG). The frame-free ABSOLUTE idle floor is a separate, heavier
 * measurement (see the active noise-floor path).
 */
#ifndef DEVOURER_RX_QUALITY_H
#define DEVOURER_RX_QUALITY_H

#include <cstdint>
#include <mutex>

#include "LinkHealth.h"
#include "RxSense.h"

namespace devourer {

/* Windowed RX link-quality snapshot. Angles: frame-driven aggregate
 * (rssi/snr/evm/noise-floor), frame-free energy (fa/cca/igi), and the fused
 * LinkHealth verdict. `valid` is false when no frames were decoded in the
 * window. Converted units (dBm / dB) so a controller doesn't repeat the raw
 * conversions. */
struct RxQuality {
  bool valid = false;
  uint32_t frames = 0;

  int rssi_mean_dbm = 0; /* window mean of path-A PWDB (raw - 110) */
  int rssi_max_dbm = 0;  /* window peak — the strength signal (see LinkHealth) */
  double snr_mean_db = 0.0;
  double snr_min_db = 0.0;
  double evm_mean_db = 0.0; /* 0 when evm_valid is false */
  bool evm_valid = false;

  /* Passive noise-floor estimate (dBm), mean of per-frame rssi_dbm - snr_db
   * over OFDM/HT frames in the window. nf_valid false if none carried SNR. */
  double noise_floor_dbm = 0.0;
  bool nf_valid = false;

  /* Frame-free energy (from GetRxEnergy — GetRxQuality subsumes it). */
  bool energy_valid = false;
  uint32_t fa_ofdm = 0;
  uint32_t cca_ofdm = 0;
  bool igi_valid = false;
  int igi = 0;

  /* Fused verdict (classify_link_health). */
  LinkVerdict verdict = LinkVerdict::NoSignal;
  const char *label = "NO_SIGNAL";
  const char *cause = "";
  const char *fix = "";
  bool igi_at_floor = false;
  bool igi_at_ceiling = false;
};

/* Drained per-window aggregate in raw devourer units (the accumulator's
 * output). Kept separate from RxQuality so the unit conversion + verdict live
 * in one place (build_rx_quality). */
struct RxQualitySnapshot {
  uint32_t frames = 0;
  int rssi_mean_raw = 0;
  int rssi_max_raw = 0;
  int snr_mean_raw = 0;
  int snr_min_raw = 0;
  int evm_mean_raw = 0;
  bool evm_valid = false;
  double nf_mean_dbm = 0.0;
  bool nf_valid = false;
};

/* Thread-safe rolling per-frame accumulator. The device feeds add() from the RX
 * loop for every decoded frame; a controller (or the energy emitter) drains it
 * with snapshot() on its own cadence. Mirrors the demo's RxAgg with a
 * passive-noise-floor term added. */
class RxQualityAccumulator {
public:
  /* Raw path-A values straight off rx_pkt_attrib. A frame with no phy-status
   * power (rssi_raw <= 0) is not a quality sample and is skipped. SNR/EVM are
   * folded only when present (raw != 0 — CCK / non-type1 phy-status leaves them
   * 0), so a mixed stream doesn't bias those means toward zero. */
  void add(int rssi_raw, int snr_raw, int evm_raw) {
    if (rssi_raw <= 0)
      return;
    std::lock_guard<std::mutex> lk(mu_);
    ++n_;
    rssi_sum_ += rssi_raw;
    if (rssi_raw > rssi_max_)
      rssi_max_ = rssi_raw;
    snr_sum_ += snr_raw;
    if (snr_raw < snr_min_)
      snr_min_ = snr_raw;
    if (evm_raw != 0) {
      evm_sum_ += evm_raw;
      ++evm_n_;
    }
    if (snr_raw != 0) {
      nf_sum_ += (rssi_raw - 110) - snr_raw / 2.0;
      ++nf_n_;
    }
  }

  /* Snapshot the window into raw aggregates and RESET (delta semantics, like
   * GetRxEnergy). */
  RxQualitySnapshot snapshot() {
    RxQualitySnapshot s;
    std::lock_guard<std::mutex> lk(mu_);
    s.frames = n_;
    if (n_) {
      s.rssi_mean_raw = rssi_sum_ / static_cast<int>(n_);
      s.rssi_max_raw = rssi_max_;
      s.snr_mean_raw = snr_sum_ / static_cast<int>(n_);
      s.snr_min_raw = snr_min_;
    }
    if (evm_n_) {
      s.evm_mean_raw = evm_sum_ / static_cast<int>(evm_n_);
      s.evm_valid = true;
    }
    if (nf_n_) {
      s.nf_mean_dbm = nf_sum_ / static_cast<double>(nf_n_);
      s.nf_valid = true;
    }
    n_ = 0;
    rssi_sum_ = 0;
    rssi_max_ = -128;
    snr_sum_ = 0;
    snr_min_ = 127;
    evm_sum_ = 0;
    evm_n_ = 0;
    nf_sum_ = 0.0;
    nf_n_ = 0;
    return s;
  }

private:
  std::mutex mu_;
  uint32_t n_ = 0;
  int32_t rssi_sum_ = 0, rssi_max_ = -128;
  int32_t snr_sum_ = 0, snr_min_ = 127;
  int32_t evm_sum_ = 0;
  uint32_t evm_n_ = 0;
  double nf_sum_ = 0.0;
  uint32_t nf_n_ = 0;
};

/* Fuse the frame aggregate + frame-free energy into an RxQuality, running the
 * LinkHealth classifier once. Pure — no I/O; the device passes GetRxEnergy()'s
 * result in. IGI rails are the union J1/J2 DIG floor (saturation corroborator)
 * and the J3 ceiling (so a static J3 IGI never reads as a false 'weak' rail),
 * matching the demo's LinkHealth mapping. */
inline RxQuality build_rx_quality(const RxQualitySnapshot &s, const RxEnergy &e,
                                  const LinkHealthThresholds &th = {}) {
  RxQuality q;
  q.frames = s.frames;
  q.valid = s.frames > 0;
  q.rssi_mean_dbm = s.rssi_mean_raw - 110;
  q.rssi_max_dbm = s.rssi_max_raw - 110;
  q.snr_mean_db = s.snr_mean_raw / 2.0;
  q.snr_min_db = s.snr_min_raw / 2.0;
  q.evm_valid = s.evm_valid;
  q.evm_mean_db = s.evm_mean_raw / 2.0;
  q.noise_floor_dbm = s.nf_mean_dbm;
  q.nf_valid = s.nf_valid;
  q.energy_valid = e.valid_fa;
  q.fa_ofdm = e.fa_ofdm;
  q.cca_ofdm = e.cca_ofdm;
  q.igi_valid = e.valid_igi;
  q.igi = e.igi;

  LinkHealthInput in;
  in.frames = s.frames;
  in.rssi_raw = s.frames ? s.rssi_max_raw : 0; /* strength = window peak */
  in.snr_raw = s.snr_mean_raw;
  in.evm_raw = s.evm_mean_raw;
  in.evm_valid = s.evm_valid;
  in.energy_valid = e.valid_fa;
  in.fa_ofdm = e.fa_ofdm;
  in.cca_ofdm = e.cca_ofdm;
  in.igi_valid = e.valid_igi;
  in.igi = e.igi;
  in.igi_min = 0x1c;
  in.igi_max = 0x7f;
  LinkHealthVerdict h = classify_link_health(in, th);
  q.verdict = h.verdict;
  q.label = h.label;
  q.cause = h.cause;
  q.fix = h.fix;
  q.igi_at_floor = h.igi_at_floor;
  q.igi_at_ceiling = h.igi_at_ceiling;
  return q;
}

/* --- Live per-chain RX-path activity (the companion to the static AdapterCaps
 * chain count) ---
 *
 * AdapterCaps.rx_chains says how many chains the silicon HAS; this says how many
 * currently look CONNECTED, inferred from the per-frame per-chain RSSI the frame
 * parser fills (RxAtrib.rssi[0..3]). It's the one antenna question that can't be
 * answered statically: a chain with a disconnected/blocked antenna (or one a
 * caller software-masked via SetRxPathMask) still exists, but its RSSI collapses
 * to the noise floor. Best-effort: only meaningful with an RX loop running and
 * ambient traffic; a strong near-field frame can briefly light a dead chain via
 * coupling, so treat single-window results as a hint, not a verdict. */
struct ActiveRxPaths {
  bool valid = false;    /* false when no frames were sampled in the window */
  uint32_t frames = 0;
  uint8_t n_chains = 0;  /* chains considered (the adapter's rx_chains) */
  uint8_t n_active = 0;  /* chains classified active */
  uint8_t active_mask = 0;      /* bit i set = chain i active */
  int rssi_mean_dbm[4] = {0, 0, 0, 0}; /* per-chain window mean (raw - 110) */
  bool chain_sampled[4] = {false, false, false, false}; /* had any sample */
};

/* Classify which of `n_chains` are active: a chain is active if it was sampled
 * and its mean RSSI is within `margin_db` of the strongest sampled chain. Pure;
 * unit-tested in tests/adapter_caps_selftest.cpp. `rssi_mean_dbm` and
 * `sampled` are the per-chain window aggregates; fills `active_mask` and returns
 * the active count. A single strong chain → mask 0x1; two balanced → 0x3; a
 * chain pinned at the noise floor is excluded. */
inline uint8_t classify_active_paths(const int *rssi_mean_dbm,
                                     const bool *sampled, uint8_t n_chains,
                                     int margin_db, uint8_t *active_mask) {
  int strongest = -1000;
  for (uint8_t i = 0; i < n_chains && i < 4; i++)
    if (sampled[i] && rssi_mean_dbm[i] > strongest)
      strongest = rssi_mean_dbm[i];
  uint8_t mask = 0, count = 0;
  if (strongest > -1000) {
    for (uint8_t i = 0; i < n_chains && i < 4; i++) {
      if (sampled[i] && (strongest - rssi_mean_dbm[i]) <= margin_db) {
        mask |= static_cast<uint8_t>(1u << i);
        ++count;
      }
    }
  }
  if (active_mask)
    *active_mask = mask;
  return count;
}

/* Thread-safe per-chain RSSI accumulator — the ActiveRxPaths analogue of
 * RxQualityAccumulator. The device feeds add() the full rssi[0..3] tuple for
 * every decoded frame; snapshot() drains the window (delta semantics) and
 * classifies. `margin_db` default 20 dB: a chain more than 20 dB below the
 * strongest is a disconnected/masked antenna, not thermal spread across
 * connected chains. */
class RxPathActivityAccumulator {
public:
  /* Feed the per-chain RSSI (raw devourer units) of one decoded frame; only the
   * first `n_chains` are considered. A chain reading <= 0 (no phy-status power)
   * is not a sample for that chain. */
  void add(const uint8_t *rssi_raw, uint8_t n_chains) {
    std::lock_guard<std::mutex> lk(mu_);
    if (n_chains > 4)
      n_chains = 4;
    n_chains_ = n_chains;
    bool any = false;
    for (uint8_t i = 0; i < n_chains; i++) {
      if (rssi_raw[i] == 0)
        continue;
      sum_[i] += rssi_raw[i];
      ++cnt_[i];
      any = true;
    }
    if (any)
      ++frames_;
  }

  ActiveRxPaths snapshot(int margin_db = 20) {
    ActiveRxPaths s;
    std::lock_guard<std::mutex> lk(mu_);
    s.frames = frames_;
    s.n_chains = n_chains_;
    s.valid = frames_ > 0;
    for (uint8_t i = 0; i < n_chains_ && i < 4; i++) {
      if (cnt_[i]) {
        s.rssi_mean_dbm[i] =
            static_cast<int>(sum_[i] / cnt_[i]) - 110;
        s.chain_sampled[i] = true;
      }
    }
    s.n_active = classify_active_paths(s.rssi_mean_dbm, s.chain_sampled,
                                       s.n_chains, margin_db, &s.active_mask);
    /* reset (delta semantics) */
    frames_ = 0;
    for (uint8_t i = 0; i < 4; i++) {
      sum_[i] = 0;
      cnt_[i] = 0;
    }
    return s;
  }

private:
  std::mutex mu_;
  uint32_t frames_ = 0;
  uint8_t n_chains_ = 0;
  uint64_t sum_[4] = {0, 0, 0, 0};
  uint32_t cnt_[4] = {0, 0, 0, 0};
};

} // namespace devourer

#endif /* DEVOURER_RX_QUALITY_H */
