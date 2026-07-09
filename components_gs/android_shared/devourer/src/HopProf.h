#ifndef HOP_PROF_H
#define HOP_PROF_H

#include <chrono>
#include <optional>

#include "Event.h"

/* Per-stage timing inside the fast_retune paths (DeviceConfig
 * debug.hop_prof), for driving the hop-latency work (FHSS wants every hop
 * microsecond accounted for). Each fast hop emits one event:
 *
 *   {"ev":"hop.prof","gen":"<tag>","ch":N,"<stage>_us":N,...,"total_us":N}
 *
 * Stages are generation-specific labels passed at mark() time. Zero overhead
 * when disabled beyond a branch. */

namespace devourer {

class HopProf {
public:
  HopProf(EventSink &sink, bool enabled, const char *gen, unsigned channel)
      : _enabled(enabled) {
    if (!_enabled)
      return;
    _t0 = _last = std::chrono::steady_clock::now();
    _ev.emplace(sink, "hop.prof");
    _ev->f("gen", gen).f("ch", channel);
  }

  /* Record the time since the previous mark under `stage`. */
  void mark(const char *stage) {
    if (!_enabled)
      return;
    const auto now = std::chrono::steady_clock::now();
    const long long us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - _last)
            .count();
    _last = now;
    char key[48];
    std::snprintf(key, sizeof(key), "%s_us", stage);
    _ev->f(key, us);
  }

  ~HopProf() {
    if (!_enabled)
      return;
    const long long total =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - _t0)
            .count();
    _ev->f("total_us", total); /* _ev emits on destruction */
  }

private:
  bool _enabled = false;
  std::optional<Ev> _ev;
  std::chrono::steady_clock::time_point _t0{}, _last{};
};

} /* namespace devourer */

#endif /* HOP_PROF_H */
