#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#ifdef ANDROID
#include <android/log.h>
#endif

#include "fmt/format.h"

enum class LogLevel : uint8_t
{
    DBG,		//used to debug info. Disabled in Release
    INFO,		//used to print usefull info both in Debug and Release
    WARNING,	//used to print warnings that will not crash, both Debug and Release
    ERR			//used for messages that will probably crash or seriously affect the game. Both Debug and Release
};

template<class Fmt, typename... Params>
void logf(LogLevel level, char const* file, int line, Fmt const& fmt, Params&&... params)
{
    const char* levelStr = "";
#ifdef ANDROID
    int android_priority = ANDROID_LOG_INFO;
#endif
    switch (level)
    {
        case LogLevel::DBG:
            levelStr = "D";
#ifdef ANDROID
            android_priority = ANDROID_LOG_DEBUG;
#endif
            break;
        case LogLevel::INFO:
            levelStr = "I";
#ifdef ANDROID
            android_priority = ANDROID_LOG_INFO;
#endif
            break;
        case LogLevel::WARNING:
            levelStr = "W";
#ifdef ANDROID
            android_priority = ANDROID_LOG_WARN;
#endif
            break;
        case LogLevel::ERR:
            levelStr = "E";
#ifdef ANDROID
            android_priority = ANDROID_LOG_ERROR;
#endif
            break;
    }

    const std::string message = fmt::format(fmt, std::forward<Params>(params)...);

#ifdef ANDROID
    __android_log_print(android_priority, "esp32-cam-fpv", "(%s) %s:%d: %s", levelStr, file, line, message.c_str());
#else
    std::printf("(%s) %s:%d: %s\n", levelStr, file, line, message.c_str());
    std::fflush(stdout);
#endif
}

#define LOGD(fmt, ...) logf(LogLevel::DBG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) logf(LogLevel::INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) logf(LogLevel::WARNING, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) logf(LogLevel::ERR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
