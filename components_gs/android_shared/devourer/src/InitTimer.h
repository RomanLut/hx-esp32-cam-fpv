#ifndef INIT_TIMER_H
#define INIT_TIMER_H

#include <chrono>
#include <cstdio>
#include <utility>

#include "logger.h"

/* Stage timer for init-path profiling. Emits one event per checkpoint:
 *
 *   {"ev":"init.timing","stage":"<scope>.<stage>","ms":N}
 *
 * `stage()` reports time since the previous checkpoint (or construction);
 * `total()` reports time since construction. Always-on: a handful of events
 * per init, negligible next to the USB transfers being measured.
 * tests/bench_init.py parses these events (docs/logging.md). */
class InitTimer {
  using clock = std::chrono::steady_clock;

public:
  InitTimer(Logger_t logger, const char *scope)
      : _logger{std::move(logger)}, _scope{scope}, _start{clock::now()},
        _last{_start} {}

  void stage(const char *name) {
    const auto now = clock::now();
    emit(name, ms(_last, now));
    _last = now;
  }

  void total() { emit("total", ms(_start, clock::now())); }

private:
  void emit(const char *name, long long millis) {
    char stage[96];
    std::snprintf(stage, sizeof(stage), "%s.%s", _scope, name);
    devourer::Ev(_logger->events(), "init.timing")
        .f("stage", stage)
        .f("ms", millis);
  }

  static long long ms(clock::time_point from, clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from)
        .count();
  }

  Logger_t _logger;
  const char *_scope;
  clock::time_point _start;
  clock::time_point _last;
};

#endif /* INIT_TIMER_H */
