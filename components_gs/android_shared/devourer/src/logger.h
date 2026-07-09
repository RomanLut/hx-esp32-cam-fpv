#ifndef LOGGER_H
#define LOGGER_H

/* Human diagnostics — leveled text lines on stderr (the control plane).
 *
 *   devourer [I] message\n     (level letter: T/D/I/W/E)
 *
 * The machine event stream (JSON Lines on stdout) is the other plane; it
 * lives in Event.h and is reached through this object as logger->events().
 * Contract + schema: docs/logging.md.
 *
 * Each line is assembled fully, then written with one fwrite + fflush —
 * per-line atomicity across threads (RX loop / coex / main TX) and no
 * pipe-buffering stalls for a subprocess supervisor.
 *
 * Verbosity has two gates:
 *  - runtime: set_level() (default Debug — everything but Trace).
 *  - compile time: DEVOURER_LOG_MAX_LEVEL (CMake option of the same name)
 *    removes trace/debug call bodies entirely. Hot-path trace/debug sites
 *    must go through the DVR_TRACE/DVR_DEBUG macros so the *arguments* are
 *    not evaluated either. Default: NDEBUG builds compile debug/trace out
 *    (matching the old NDEBUG behavior), debug builds keep everything.
 *
 * Configure (set_level / set_diag_stream / events().configure) before worker
 * threads spawn; the fields are intentionally unsynchronized. */

#include <memory>
#include <string>

#include <cctype>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <type_traits>

#include "Event.h"

#define ushort uint16_t

/* Compile-time floor: calls below this level compile to nothing. */
#define DEVOURER_LL_TRACE 0
#define DEVOURER_LL_DEBUG 1
#define DEVOURER_LL_INFO 2
#define DEVOURER_LL_WARN 3
#define DEVOURER_LL_ERROR 4
#define DEVOURER_LL_SILENT 5

#ifndef DEVOURER_LOG_MAX_LEVEL
#ifdef NDEBUG
#define DEVOURER_LOG_MAX_LEVEL DEVOURER_LL_INFO
#else
#define DEVOURER_LOG_MAX_LEVEL DEVOURER_LL_TRACE
#endif
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

/* Widen byte-sized integers before streaming: `oss << uint8_t` inserts a raw
 * char, which puts non-UTF-8 bytes into the log (register values like 0xF0)
 * and breaks text consumers such as tests/regress.py. Plain `char` is left
 * alone — it is a distinct type and means "character" at our call sites. */
template<typename T>
decltype(auto) widen_for_stream(const T& value)
{
    if constexpr (std::is_same_v<T, unsigned char>)
        return static_cast<unsigned>(value);
    else if constexpr (std::is_same_v<T, signed char>)
        return static_cast<int>(value);
    else
        return (value);
}

template<typename T>
void format_helper(std::ostringstream& oss,
                   std::string_view& str, const T& value)
{
    std::size_t openBracket = str.find('{');
    if (openBracket == std::string::npos) { return; }
    std::size_t closeBracket = str.find('}', openBracket + 1);
    if (closeBracket == std::string::npos) { return; }

    oss << str.substr(0, openBracket);

    /* Parse a minimal subset of std::format spec inside {...}: optional
     * ':' then optional '0' fill, optional width digits, optional 'x'/'X'
     * for hex. Sufficient for our uses; falls back to default formatting
     * if no spec given. */
    std::string_view spec = str.substr(openBracket + 1, closeBracket - openBracket - 1);
    if (auto colon = spec.find(':'); colon != std::string_view::npos) {
        spec.remove_prefix(colon + 1);
    } else {
        spec = {};
    }

    if (!spec.empty()) {
        std::ios_base::fmtflags saved = oss.flags();
        char saved_fill = oss.fill();
        bool zero_pad = false;
        if (!spec.empty() && spec.front() == '0') {
            zero_pad = true;
            spec.remove_prefix(1);
        }
        int width = 0;
        while (!spec.empty() && std::isdigit(static_cast<unsigned char>(spec.front()))) {
            width = width * 10 + (spec.front() - '0');
            spec.remove_prefix(1);
        }
        bool hex = false, hex_upper = false;
        if (!spec.empty() && (spec.front() == 'x' || spec.front() == 'X')) {
            hex = true;
            hex_upper = spec.front() == 'X';
        }
        if (hex) oss << std::hex;
        if (hex_upper) oss << std::uppercase;
        if (zero_pad) oss << std::setfill('0');
        if (width > 0) oss << std::setw(width);
        oss << widen_for_stream(value);
        oss.flags(saved);
        oss.fill(saved_fill);
    } else {
        oss << widen_for_stream(value);
    }

    str = str.substr(closeBracket + 1);
}

template<typename... Targs>
std::string format(std::string_view str, Targs...args)
{
    std::ostringstream oss;
    (format_helper(oss, str, args),...);
    oss << str;
    return oss.str();
}

template <typename... Args>
using format_string_t = std::string_view;

class Logger {
public:
    /* Verbosity threshold. A message at level L is emitted only when
     * _level <= L, so Trace shows everything and Silent shows nothing.
     * Defaults to Debug; a caller that wants a clean stderr (e.g. sense's
     * live display) can quiet the library with set_level(Level::Warn). */
    enum class Level { Trace, Debug, Info, Warn, Error, Silent };
    void set_level(Level l) { _level = l; }
    Level level() const { return _level; }

    /* Diagnostics destination (default stderr). */
    void set_diag_stream(std::FILE* f) { _diag = f; }

    /* The machine event stream (JSON Lines, default stdout) — Event.h. */
    devourer::EventSink& events() { return _events; }

    template<typename... Args>
    void trace(format_string_t<Args...> fmt, Args &&...args) {
#if DEVOURER_LOG_MAX_LEVEL <= DEVOURER_LL_TRACE
        if (_level > Level::Trace) return;
        emit('T', format(fmt, args...));
#else
        (void)fmt; ((void)args, ...);
#endif
    }

    template<typename... Args>
    void debug(format_string_t<Args...> fmt, Args &&...args) {
#if DEVOURER_LOG_MAX_LEVEL <= DEVOURER_LL_DEBUG
        if (_level > Level::Debug) return;
        emit('D', format(fmt, args...));
#else
        (void)fmt; ((void)args, ...);
#endif
    }

    template<typename... Args>
    void info(format_string_t<Args...> fmt, Args &&...args) {
#if DEVOURER_LOG_MAX_LEVEL <= DEVOURER_LL_INFO
        if (_level > Level::Info) return;
        emit('I', format(fmt, args...));
#else
        (void)fmt; ((void)args, ...);
#endif
    }

    template<typename... Args>
    void warn(format_string_t<Args...> fmt, Args &&...args) {
#if DEVOURER_LOG_MAX_LEVEL <= DEVOURER_LL_WARN
        if (_level > Level::Warn) return;
        emit('W', format(fmt, args...));
#else
        (void)fmt; ((void)args, ...);
#endif
    }

    template<typename... Args>
    void error(format_string_t<Args...> fmt, Args &&...args) {
#if DEVOURER_LOG_MAX_LEVEL <= DEVOURER_LL_ERROR
        if (_level > Level::Error) return;
        emit('E', format(fmt, args...));
#else
        (void)fmt; ((void)args, ...);
#endif
    }

private:
    void emit(char lvl, const std::string& txt) {
#ifdef __ANDROID__
        static constexpr int prio[] = {ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG,
                                       ANDROID_LOG_INFO, ANDROID_LOG_WARN,
                                       ANDROID_LOG_ERROR};
        const char* p = "TDIWE";
        const char* found = std::strchr(p, lvl);
        __android_log_write(prio[found ? found - p : 2], "devourer",
                            txt.c_str());
#else
        /* One line, one fwrite (FILE* is stream-locked → per-line atomic
         * across threads), then flush so a pipe reader never stalls. */
        std::string line;
        line.reserve(txt.size() + 16);
        line += "devourer [";
        line += lvl;
        line += "] ";
        line += txt;
        line += '\n';
        std::fwrite(line.data(), 1, line.size(), _diag);
        std::fflush(_diag);
#endif
    }

    Level _level = Level::Debug;
    std::FILE* _diag = stderr;
    devourer::EventSink _events;
};

using Logger_t = std::shared_ptr<Logger>;

/* Hot-path trace/debug: the if-guard elides argument evaluation at runtime;
 * the #if removes the whole statement (arguments included) when compiled out
 * via DEVOURER_LOG_MAX_LEVEL. */
#if DEVOURER_LOG_MAX_LEVEL <= DEVOURER_LL_TRACE
#define DVR_TRACE(lg, ...)                                                     \
  do {                                                                         \
    if ((lg) && (lg)->level() <= Logger::Level::Trace)                         \
      (lg)->trace(__VA_ARGS__);                                                \
  } while (0)
#else
#define DVR_TRACE(lg, ...)                                                     \
  do {                                                                         \
  } while (0)
#endif

#if DEVOURER_LOG_MAX_LEVEL <= DEVOURER_LL_DEBUG
#define DVR_DEBUG(lg, ...)                                                     \
  do {                                                                         \
    if ((lg) && (lg)->level() <= Logger::Level::Debug)                         \
      (lg)->debug(__VA_ARGS__);                                                \
  } while (0)
#else
#define DVR_DEBUG(lg, ...)                                                     \
  do {                                                                         \
  } while (0)
#endif

#endif /* LOGGER_H */
