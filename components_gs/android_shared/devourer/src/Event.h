#ifndef DEVOURER_EVENT_H
#define DEVOURER_EVENT_H

/* Machine event stream — JSON Lines on stdout (the data plane).
 *
 * One event = one line = one JSON object whose first field is always the
 * event name, serialized exactly as
 *
 *   {"ev":"<name>", ...}
 *
 * That first-field guarantee is part of the contract: shell consumers may
 * `grep -F '"ev":"rx.txhit"'` without a JSON parser; everything richer goes
 * through json.loads (see tests/devourer_events.py). Schema: docs/logging.md.
 *
 * Emission is a single fwrite() of the complete line followed by fflush()
 * (FlushPolicy::EveryLine, the default) — fwrite locks the FILE* on every
 * supported libc, so concurrent threads interleave whole lines, never
 * characters, and a python subprocess reading the pipe sees each event the
 * moment it is emitted (stdout through a pipe is fully buffered otherwise).
 *
 * Usage:
 *   Ev(sink, "thermal").t().f("raw", raw).f("delta", d).f("status", s);
 *   // emits on destruction; or call .emit() explicitly.
 *
 * Dependency-free, no allocation for typical events (inline 768-byte buffer;
 * oversized bodies spill to a reused thread-local scratch). Numbers are
 * emitted via snprintf with long long/unsigned long long/double only — no
 * %zu-style portability traps. NaN/Inf serialize as null. A line is hard-
 * capped at 64 KiB: an overflowing field is dropped and "truncated":true is
 * appended. */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string_view>
#include <vector>

namespace devourer {

class EventSink {
public:
  enum class FlushPolicy { EveryLine, Never };

  /* Configure before any worker threads spawn (demo main() start). */
  void configure(std::FILE *out, FlushPolicy fp = FlushPolicy::EveryLine) {
    _out = out;
    _flush = fp;
    _enabled = out != nullptr;
  }
  void disable() { _enabled = false; }
  bool enabled() const { return _enabled; }

  /* p..p+n must be one complete line including the trailing '\n'. A single
   * fwrite keeps the line atomic across threads. */
  void write_line(const char *p, size_t n) {
    if (!_enabled)
      return;
    std::fwrite(p, 1, n, _out);
    if (_flush == FlushPolicy::EveryLine)
      std::fflush(_out);
  }

private:
  std::FILE *_out = stdout;
  FlushPolicy _flush = FlushPolicy::EveryLine;
  bool _enabled = true;
};

/* Monotonic milliseconds since process start, for Ev::t(). The epoch is a
 * function-local static, so "start" is the first event emission — close
 * enough to main() for a telemetry timestamp, and safe under any static-init
 * order. */
inline long long event_uptime_ms() {
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

class Ev {
public:
  static constexpr size_t kMaxLine = 64 * 1024;

  Ev(EventSink &sink, const char *name) : _sink(sink) {
    _buf = _sbuf;
    _cap = sizeof(_sbuf);
    if (!_sink.enabled()) {
      _dead = true;
      return;
    }
    raw("{\"ev\":\"", 7);
    raw(name, std::strlen(name)); /* event names are our literals: no escape */
    raw("\"", 1);
  }
  Ev(const Ev &) = delete;
  Ev &operator=(const Ev &) = delete;
  ~Ev() { emit(); }

  Ev &f(const char *k, long long v) {
    if (begin_field(k)) {
      char tmp[24];
      raw(tmp, (size_t)std::snprintf(tmp, sizeof(tmp), "%lld", v));
    }
    return *this;
  }
  Ev &f(const char *k, unsigned long long v) {
    if (begin_field(k)) {
      char tmp[24];
      raw(tmp, (size_t)std::snprintf(tmp, sizeof(tmp), "%llu", v));
    }
    return *this;
  }
  Ev &f(const char *k, int v) { return f(k, (long long)v); }
  Ev &f(const char *k, long v) { return f(k, (long long)v); }
  Ev &f(const char *k, unsigned v) { return f(k, (unsigned long long)v); }
  Ev &f(const char *k, unsigned long v) { return f(k, (unsigned long long)v); }
  Ev &f(const char *k, double v) {
    if (begin_field(k)) {
      if (std::isfinite(v)) {
        char tmp[32];
        raw(tmp, (size_t)std::snprintf(tmp, sizeof(tmp), "%.10g", v));
      } else {
        raw("null", 4);
      }
    }
    return *this;
  }
  Ev &f(const char *k, bool v) {
    if (begin_field(k))
      v ? raw("true", 4) : raw("false", 5);
    return *this;
  }
  Ev &f(const char *k, std::string_view v) {
    if (begin_field(k))
      str(v);
    return *this;
  }
  Ev &f(const char *k, const char *v) { return f(k, std::string_view(v)); }
  Ev &f(const char *k, std::nullptr_t) {
    if (begin_field(k))
      raw("null", 4);
    return *this;
  }

  /* "k":"0x1c2" — register addresses/values/masks are hex strings by schema
   * convention. digits=0 emits the minimal width. */
  Ev &hexf(const char *k, unsigned long long v, int digits = 0) {
    if (begin_field(k)) {
      char tmp[24];
      raw(tmp, (size_t)std::snprintf(tmp, sizeof(tmp), "\"0x%0*llx\"",
                                     digits, v));
    }
    return *this;
  }

  /* "k":"aabbcc..." — lowercase hex string of a byte blob (frame bodies). */
  Ev &hex(const char *k, const uint8_t *p, size_t n) {
    if (!begin_field(k))
      return *this;
    if (!ensure(n * 2 + 2)) { /* field key already written: close valuelessly */
      raw("null", 4);
      return *this;
    }
    static const char digits[] = "0123456789abcdef";
    _buf[_len++] = '"';
    for (size_t i = 0; i < n; i++) {
      _buf[_len++] = digits[p[i] >> 4];
      _buf[_len++] = digits[p[i] & 0xf];
    }
    _buf[_len++] = '"';
    return *this;
  }

  /* "k":[1,-2,3] — per-chain metrics. */
  Ev &arr(const char *k, const int *v, size_t n) {
    if (!begin_field(k))
      return *this;
    raw("[", 1);
    for (size_t i = 0; i < n; i++) {
      if (i)
        raw(",", 1);
      char tmp[16];
      raw(tmp, (size_t)std::snprintf(tmp, sizeof(tmp), "%d", v[i]));
    }
    raw("]", 1);
    return *this;
  }
  Ev &arr(const char *k, const double *v, size_t n) {
    if (!begin_field(k))
      return *this;
    raw("[", 1);
    for (size_t i = 0; i < n; i++) {
      if (i)
        raw(",", 1);
      if (std::isfinite(v[i])) {
        char tmp[32];
        raw(tmp, (size_t)std::snprintf(tmp, sizeof(tmp), "%.10g", v[i]));
      } else {
        raw("null", 4);
      }
    }
    raw("]", 1);
    return *this;
  }

  /* "t":<monotonic ms since process start> — periodic/marker events carry it;
   * per-frame events rely on seq/tsfl instead. */
  Ev &t() { return f("t", event_uptime_ms()); }

  void emit() {
    if (_dead)
      return;
    /* The closer consumes the kSlack reserve, so it always fits. */
    if (_truncated)
      closer(",\"truncated\":true", 17);
    closer("}\n", 2);
    _dead = true;
    _sink.write_line(_buf, _len);
    release_scratch();
  }

private:
  /* Reused per-thread spill buffer so oversized events (hex frame bodies)
   * don't allocate per emission. A busy flag guards the rare nested-Ev case;
   * the fallback then is a private heap vector. */
  struct Scratch {
    std::vector<char> buf;
    bool busy = false;
  };
  static Scratch &tls_scratch() {
    thread_local Scratch s;
    return s;
  }

  bool begin_field(const char *k) {
    if (_dead)
      return false;
    /* key is always one of our literals — no escaping needed */
    size_t klen = std::strlen(k);
    if (!ensure(klen + 4))
      return false;
    _buf[_len++] = ',';
    _buf[_len++] = '"';
    std::memcpy(_buf + _len, k, klen);
    _len += klen;
    _buf[_len++] = '"';
    _buf[_len++] = ':';
    return true;
  }

  void raw(const char *p, size_t n) {
    if (_dead || !ensure(n))
      return;
    std::memcpy(_buf + _len, p, n);
    _len += n;
  }

  /* emit()-only append that spends the kSlack reserve (every successful
   * ensure() left _len <= kMaxLine - kSlack, so this cannot fail). */
  void closer(const char *p, size_t n) {
    if (!ensure(n, 0))
      return;
    std::memcpy(_buf + _len, p, n);
    _len += n;
  }

  void str(std::string_view v) {
    /* worst case every char escapes to \u00XX (6 bytes) + quotes */
    if (!ensure(v.size() * 6 + 2)) {
      raw("null", 4);
      return;
    }
    _buf[_len++] = '"';
    for (unsigned char c : v) {
      switch (c) {
      case '"':
        _buf[_len++] = '\\';
        _buf[_len++] = '"';
        break;
      case '\\':
        _buf[_len++] = '\\';
        _buf[_len++] = '\\';
        break;
      case '\n':
        _buf[_len++] = '\\';
        _buf[_len++] = 'n';
        break;
      case '\t':
        _buf[_len++] = '\\';
        _buf[_len++] = 't';
        break;
      case '\r':
        _buf[_len++] = '\\';
        _buf[_len++] = 'r';
        break;
      default:
        if (c < 0x20) {
          _len += (size_t)std::snprintf(_buf + _len, 8, "\\u%04x", c);
        } else {
          _buf[_len++] = (char)c;
        }
      }
    }
    _buf[_len++] = '"';
  }

  /* Grow so that n more bytes fit, spilling from the inline buffer to the
   * thread-local scratch. Returns false (and flags truncation) if the line
   * would exceed kMaxLine minus slack for the truncated marker + "}\n". */
  static constexpr size_t kSlack = 32; /* reserved for the emit() closer */

  bool ensure(size_t n, size_t slack = kSlack) {
    if (_len + n + slack > kMaxLine) {
      _truncated = true;
      return false;
    }
    if (_len + n <= _cap)
      return true;
    size_t want = _len + n;
    size_t newcap = _cap * 2 > want ? _cap * 2 : want;
    if (newcap > kMaxLine)
      newcap = kMaxLine;
    if (_buf == _sbuf) {
      Scratch &s = tls_scratch();
      if (!s.busy) {
        s.busy = true;
        _scratch = &s;
        if (s.buf.size() < newcap)
          s.buf.resize(newcap);
        std::memcpy(s.buf.data(), _sbuf, _len);
        _buf = s.buf.data();
        _cap = s.buf.size();
        return true;
      }
      _heap.resize(newcap);
      std::memcpy(_heap.data(), _sbuf, _len);
      _buf = _heap.data();
      _cap = _heap.size();
      return true;
    }
    if (_scratch) {
      _scratch->buf.resize(newcap);
      _buf = _scratch->buf.data();
    } else {
      _heap.resize(newcap);
      _buf = _heap.data();
    }
    _cap = newcap;
    return true;
  }

  void release_scratch() {
    if (_scratch) {
      _scratch->busy = false;
      _scratch = nullptr;
    }
  }

  EventSink &_sink;
  char *_buf = nullptr;
  size_t _cap = 0;
  size_t _len = 0;
  bool _dead = false;
  bool _truncated = false;
  Scratch *_scratch = nullptr;
  std::vector<char> _heap;
  char _sbuf[768];
};

} /* namespace devourer */

#endif /* DEVOURER_EVENT_H */
