/* BfReportDecode — C++ decoder for 802.11ac VHT Compressed Beamforming reports.
 *
 * A direct port of the reference tool `tools/bf_report_decode.py`: it unpacks the
 * per-subcarrier Givens angles (phi, psi) out of a captured report frame, and a
 * `MotionMeter` turns a stream of reports into a Wi-Fi-sensing signal — the
 * per-tone cross-frame variance of psi. A moving person perturbs the channel, so
 * that variance rises across the whole band (broadband); a narrowband interferer
 * raises it on a few tones (localized). See docs/beamforming-victim-sensing.h.
 *
 * The compressed V matrix is the right singular vectors of the per-tone channel,
 * quantized as Givens rotation angles and packed LSB-first. For the 2-TX / 1-SS
 * sounding devourer drives (Nr=2, Nc=1) each subcarrier carries one phi then one
 * psi; psi = atan(|h_B|/|h_A|) is the relative per-tone channel between the
 * beamformer's two antennas — the quantity that jitters under channel motion.
 *
 * Header-only, like BeamformingSounder.h. No devourer runtime
 * dependency, so it is unit-testable in isolation (examples/sense/bf_report_decode_selftest.cpp). */
#ifndef BF_REPORT_DECODE_H
#define BF_REPORT_DECODE_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace devourer::bf {

/* Subcarriers carried in a report, per bandwidth (0..3 = 20/40/80/160 MHz) and
 * grouping Ng (1/2/4). Matches NS_TABLE in the Python tool. 0 = unknown. */
inline int report_ns(int bw, int ng) {
  static const int table[4][3] = {/* Ng=1, 2, 4 */
                                  {52, 30, 16},
                                  {108, 58, 30},
                                  {234, 122, 62},
                                  {468, 244, 124}};
  int gi = ng == 1 ? 0 : ng == 2 ? 1 : ng == 4 ? 2 : -1;
  if (bw < 0 || bw > 3 || gi < 0)
    return 0;
  return table[bw][gi];
}

/* Parsed report header + the location of the V-angle bytes within the frame. */
struct ReportHdr {
  int nc = 0, nr = 0;   /* streams / rx antennas (already +1) */
  int bw = 0, ng = 0;   /* bandwidth code, grouping */
  bool mu = false;      /* feedback: MU (true) / SU (false) */
  bool vht = false;     /* VHT (true) / HT (false) */
  int ns = 0;           /* subcarriers (report_ns) */
  const uint8_t *angles = nullptr; /* first V-angle byte */
  size_t angle_len = 0; /* V-angle byte count (MU: clamped before the MU-SNR) */
  int per_sc_bits = 0;  /* bits per subcarrier across all angles */
};

/* True if `d`(n) is a VHT/HT Compressed Beamforming report; fills `hdr`. Mirrors
 * parse_frame() + the MU V-angle slice (ns*10 bits) in the Python tool. */
inline bool parse_report(const uint8_t *d, size_t n, ReportHdr &hdr) {
  if (d == nullptr || n < 30)
    return false;
  uint8_t sub = d[0] & 0xF0;
  if (sub != 0xD0 && sub != 0xE0) /* Action / Action No-Ack */
    return false;
  uint8_t cat = d[24], act = d[25];
  bool vht = (cat == 0x15 && act == 0x00);
  bool ht = (cat == 0x07 && act == 0x00);
  if (!vht && !ht)
    return false;
  uint32_t mc = (uint32_t)d[26] | ((uint32_t)d[27] << 8) | ((uint32_t)d[28] << 16);
  hdr.nc = (int)(mc & 0x7) + 1;
  hdr.nr = (int)((mc >> 3) & 0x7) + 1;
  hdr.bw = (int)((mc >> 6) & 0x3);
  int ng_code = (int)((mc >> 8) & 0x3);
  hdr.ng = ng_code == 0 ? 1 : ng_code == 1 ? 2 : 4;
  hdr.mu = ((mc >> 11) & 0x1) != 0;
  hdr.vht = vht;
  hdr.ns = report_ns(hdr.bw, hdr.ng);
  if (hdr.ns <= 0 || n < 30 + (size_t)hdr.nc + 4)
    return false;
  const uint8_t *ab = d + 29 + hdr.nc;          /* after per-column avg SNR */
  size_t ab_len = n - (29 + (size_t)hdr.nc) - 4; /* drop 4-byte FCS */
  if (hdr.mu) {
    /* MU report: the V-angles are the first ns*10 bits (the Realtek compact 2x1
     * codebook); the MU Exclusive per-tone SNR follows and is not decoded here. */
    hdr.per_sc_bits = 10;
    size_t vbytes = ((size_t)hdr.ns * 10 + 7) / 8;
    if (vbytes > ab_len)
      return false;
    ab_len = vbytes;
  } else {
    size_t bits = ab_len * 8;
    if (bits % (size_t)hdr.ns != 0)
      return false;
    hdr.per_sc_bits = (int)(bits / (size_t)hdr.ns);
  }
  hdr.angles = ab;
  hdr.angle_len = ab_len;
  return hdr.per_sc_bits >= 2;
}

/* LSB-first bit reader over the packed angle stream (802.11 packing order). */
class BitReader {
public:
  BitReader(const uint8_t *data, size_t len) : _d(data), _bits(len * 8) {}
  uint32_t read(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
      if (_pos >= _bits)
        break;
      uint32_t bit = (_d[_pos >> 3] >> (_pos & 7)) & 1u;
      v |= bit << i;
      ++_pos;
    }
    return v;
  }

private:
  const uint8_t *_d;
  size_t _bits;
  size_t _pos = 0;
};

inline double dequant_phi(uint32_t q, int b) {
  return (2.0 * q + 1.0) * M_PI / (double)(1u << b);
}
inline double dequant_psi(uint32_t q, int b) {
  return (2.0 * q + 1.0) * M_PI / (double)(1u << (b + 2));
}

/* Number of (phi, psi) angle pairs per subcarrier for the compressed matrix
 * (802.11 §19.3.12.3.6). For the 2x1 case this is 1 phi + 1 psi. */
inline void angle_counts(int nr, int nc, int &nphi, int &npsi) {
  nphi = 0;
  npsi = 0;
  int lim = nc < (nr - 1) ? nc : (nr - 1);
  for (int i = 1; i <= lim; ++i) {
    for (int r = i; r < nr; ++r)
      ++nphi;
    for (int r = i; r < nr; ++r)
      ++npsi;
  }
  if (nphi == 0 && npsi == 0) { /* defensive: treat as 2x1 */
    nphi = 1;
    npsi = 1;
  }
}

/* Decode the first phi and psi angle of every subcarrier for one report, given a
 * bit split. Returns false if the stream is too short. Layout per subcarrier is
 * nphi phi's (bphi bits each) then npsi psi's (bpsi bits each). Either output may
 * be null. */
inline bool decode_angles(const ReportHdr &hdr, int bphi, int bpsi,
                          std::vector<double> *phi_out,
                          std::vector<double> *psi_out) {
  int nphi, npsi;
  angle_counts(hdr.nr, hdr.nc, nphi, npsi);
  size_t need = (size_t)hdr.ns * (size_t)(nphi * bphi + npsi * bpsi);
  if (need > hdr.angle_len * 8)
    return false;
  BitReader br(hdr.angles, hdr.angle_len);
  if (phi_out)
    phi_out->assign(hdr.ns, 0.0);
  if (psi_out)
    psi_out->assign(hdr.ns, 0.0);
  for (int k = 0; k < hdr.ns; ++k) {
    double first_phi = 0.0, first_psi = 0.0;
    for (int p = 0; p < nphi; ++p) {
      double phi = dequant_phi(br.read(bphi), bphi);
      if (p == 0)
        first_phi = phi;
    }
    for (int p = 0; p < npsi; ++p) {
      double psi = dequant_psi(br.read(bpsi), bpsi);
      if (p == 0)
        first_psi = psi;
    }
    if (phi_out)
      (*phi_out)[k] = first_phi;
    if (psi_out)
      (*psi_out)[k] = first_psi;
  }
  return true;
}

/* Convenience: decode only psi (the amplitude-ratio angle). */
inline bool decode_psi(const ReportHdr &hdr, int bphi, int bpsi,
                       std::vector<double> &psi_out) {
  return decode_angles(hdr, bphi, bpsi, nullptr, &psi_out);
}

/* Pick (bphi, bpsi) for a per-subcarrier budget. The 802.11 compressed-Givens
 * codebook ALWAYS pairs b_phi = b_psi + 2 (the codebook table is
 * (bpsi,bphi) in {(1,3),(2,4),(3,5),(4,6),(5,7),(7,9)}), so we only ever
 * consider splits that satisfy it. This is essential, not cosmetic: an
 * unconstrained search that minimises cross-frame psi variance is fooled by a
 * too-coarse psi — e.g. at (bphi=8,bpsi=2) the 2-bit psi is trivially constant
 * (variance 0), so it "wins" the search, but that split is bit-misaligned and
 * its phi decodes to garbage that jitters ~30x more on a static channel than
 * the correct (6,4) split. Among the (usually one) valid Givens splits, break
 * ties by minimum cross-frame psi variance. Returns false if none fits. */
inline bool pick_split(const std::vector<ReportHdr> &batch, int &bphi_out,
                       int &bpsi_out) {
  if (batch.empty())
    return false;
  int per = batch[0].per_sc_bits, ns = batch[0].ns;
  int nphi, npsi;
  angle_counts(batch[0].nr, batch[0].nc, nphi, npsi);
  double best_var = 1e300;
  bool found = false;
  for (int bpsi = 1; bpsi <= per; ++bpsi) {
    int bphi = bpsi + 2; /* enforce the standard Givens codebook relationship */
    if (nphi * bphi + npsi * bpsi != per)
      continue;
    /* mean cross-frame variance of psi[k], averaged over tones */
    std::vector<double> sum(ns, 0.0), sumsq(ns, 0.0);
    int nfr = 0;
    bool ok = true;
    for (const auto &h : batch) {
      std::vector<double> psi;
      if (!decode_psi(h, bphi, bpsi, psi)) {
        ok = false;
        break;
      }
      for (int k = 0; k < ns; ++k) {
        sum[k] += psi[k];
        sumsq[k] += psi[k] * psi[k];
      }
      ++nfr;
    }
    if (!ok || nfr == 0)
      continue;
    double var = 0.0;
    for (int k = 0; k < ns; ++k) {
      double m = sum[k] / nfr;
      var += sumsq[k] / nfr - m * m;
    }
    var /= ns;
    if (var < best_var) {
      best_var = var;
      bphi_out = bphi;
      bpsi_out = bpsi;
      found = true;
    }
  }
  return found;
}

/* Sliding-window Wi-Fi-sensing meter: per-tone cross-frame variance of psi over
 * the last `window` reports, and the aggregate "motion energy" (mean over tones).
 * Fixed split (calibrated once) so the variance is comparable frame to frame. */
class MotionMeter {
public:
  MotionMeter(int ns, int bphi, int bpsi, size_t window = 64)
      : _ns(ns), _bphi(bphi), _bpsi(bpsi), _window(window) {}

  /* Decode one report and add its phi[k] to the window. phi (the inter-antenna
   * phase, finely quantized) is far more sensitive to motion than psi. Returns
   * false if the report's geometry doesn't match this meter (ns) or decode
   * failed. */
  bool push(const ReportHdr &hdr) {
    if (hdr.ns != _ns)
      return false;
    std::vector<double> phi;
    if (!decode_angles(hdr, _bphi, _bpsi, &phi, nullptr))
      return false;
    _buf.push_back(std::move(phi));
    while (_buf.size() > _window)
      _buf.pop_front();
    return true;
  }

  size_t count() const { return _buf.size(); }

  /* Per-tone circular variance of phi over the window: 1 - |mean(e^{i*phi})|,
   * in [0,1]. 0 = the tone's phase is constant (static channel); toward 1 = the
   * phase is scattered (the channel is changing frame-to-frame). Circular so it
   * handles the 0/2*pi wrap. */
  std::vector<double> per_tone_var() const {
    std::vector<double> var(_ns, 0.0);
    size_t n = _buf.size();
    if (n < 2)
      return var;
    for (int k = 0; k < _ns; ++k) {
      double cr = 0.0, ci = 0.0;
      for (const auto &f : _buf) {
        cr += std::cos(f[k]);
        ci += std::sin(f[k]);
      }
      double r = std::sqrt(cr * cr + ci * ci) / n;
      var[k] = 1.0 - r;
    }
    return var;
  }

  /* Aggregate motion energy in [0,1]: mean per-tone circular variance. */
  double motion_energy() const {
    auto v = per_tone_var();
    if (v.empty())
      return 0.0;
    double s = 0.0;
    for (double x : v)
      s += x;
    return s / v.size();
  }

  /* True when the elevated variance is concentrated on a few adjacent tones (a
   * narrowband interferer) rather than spread across the band (human motion).
   * Heuristic: peak tone variance >> median. */
  bool localized() const {
    auto v = per_tone_var();
    if (v.size() < 6)
      return false;
    std::vector<double> s = v;
    std::sort(s.begin(), s.end());
    double median = s[s.size() / 2];
    double peak = s.back();
    /* count tones within 50% of the peak — few = localized, many = broadband */
    double thr = peak * 0.5;
    int hot = 0;
    for (double x : v)
      if (x >= thr)
        ++hot;
    return median > 1e-9 && peak > 6.0 * median && hot <= (int)v.size() / 6;
  }

private:
  int _ns, _bphi, _bpsi;
  size_t _window;
  std::deque<std::vector<double>> _buf;
};

} // namespace devourer::bf

#endif /* BF_REPORT_DECODE_H */
