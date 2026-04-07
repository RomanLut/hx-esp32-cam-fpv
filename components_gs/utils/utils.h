#pragma once

#include <cstdint>
#include <string>

extern void setupNonBlockingInput();
extern bool isRadxaZero3();
extern int formatGSRSSI(int8_t rssi);
extern bool runShellCommand(const std::string& command, std::string* output = nullptr);
