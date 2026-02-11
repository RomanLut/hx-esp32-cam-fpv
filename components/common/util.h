#pragma once
#include <string>

template<typename T>
T clamp(T value, T min, T max) 
{
    return (value < min) ? min : (value > max) ? max : value;
}
extern int smallestPowerOfTwo(int value, int minValue);

#ifndef ESP_PLATFORM
std::string readTextFileFirstLine(const std::string& path);
std::string trimHexPrefix(const std::string& value);
std::string readSymlinkBasename(const std::string& path);
std::string getInterfaceSummary(const std::string& iface);
#endif
