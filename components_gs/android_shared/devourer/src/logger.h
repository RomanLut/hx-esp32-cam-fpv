#ifndef LOGGER_H
#define LOGGER_H

#include <memory>
#include <string>

#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

#define ushort uint16_t
#define DEVOURER_LOG_TAG "devourer"

#ifdef __ANDROID__
    #include <android/log.h>

    #define DEVOURER_LOGV(...) __android_log_write(ANDROID_LOG_VERBOSE, DEVOURER_LOG_TAG, __VA_ARGS__)
    #define DEVOURER_LOGD(...) __android_log_write(ANDROID_LOG_DEBUG, DEVOURER_LOG_TAG, __VA_ARGS__)
    #define DEVOURER_LOGI(...) __android_log_write(ANDROID_LOG_INFO, DEVOURER_LOG_TAG, __VA_ARGS__)
    #define DEVOURER_LOGW(...) __android_log_write(ANDROID_LOG_WARN, DEVOURER_LOG_TAG, __VA_ARGS__)
    #define DEVOURER_LOGE(...) __android_log_write(ANDROID_LOG_ERROR, DEVOURER_LOG_TAG, __VA_ARGS__)
#else
    #include <cstdio>

    #define DEVOURER_LOGV(...)            \
    printf("<%s>", DEVOURER_LOG_TAG); \
    printf(__VA_ARGS__);                \
    printf("\n")
    #define DEVOURER_LOGD(...)            \
    printf("<%s>", DEVOURER_LOG_TAG); \
    printf(__VA_ARGS__);                \
    printf("\n")
    #define DEVOURER_LOGI(...)            \
    printf("<%s>", DEVOURER_LOG_TAG); \
    printf(__VA_ARGS__);                \
    printf("\n")
    #define DEVOURER_LOGW(...)                   \
    printf("<%s>", DEVOURER_LOG_TAG); \
    printf(__VA_ARGS__);                       \
    printf("\n")
    #define DEVOURER_LOGE(...)                \
    printf("<%s>", DEVOURER_LOG_TAG); \
    printf(__VA_ARGS__);                    \
    printf("\n")
#endif

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
        oss << value;
        oss.flags(saved);
        oss.fill(saved_fill);
    } else {
        oss << value;
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
    template<typename... Args>
    void debug(format_string_t<Args...> fmt, Args &&...args) {
#if !defined(NDEBUG)
        std::string txt = format(fmt, args...);
        DEVOURER_LOGD(txt.c_str());
#endif
    }

    template<typename... Args>
    void info(format_string_t<Args...> fmt, Args &&...args) {
        std::string txt = format(fmt, args...);
        DEVOURER_LOGI(txt.c_str());
    }

    template<typename... Args>
    void warn(format_string_t<Args...> fmt, Args &&...args) {
        std::string txt = format(fmt, args...);
        DEVOURER_LOGW(txt.c_str());
    }

    template<typename... Args>
    void error(format_string_t<Args...> fmt, Args &&...args) {
        std::string txt = format(fmt, args...);
        DEVOURER_LOGE(txt.c_str());
    }
};

using Logger_t = std::shared_ptr<Logger>;

#endif /* LOGGER_H */
