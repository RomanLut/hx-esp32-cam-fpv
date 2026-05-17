#pragma once

#include <cstdint>
#include <optional>
#include <string>

extern void setupNonBlockingInput();
extern bool isRadxaZero3();
extern int formatGSRSSI(int8_t rssi);
extern bool runShellCommand(const std::string& command, std::string* output = nullptr);
extern std::optional<std::string> findExecutablePath(const std::string& executable_name);
extern std::string trimAsciiWhitespace(const std::string& value);
extern std::string shellQuote(const std::string& value);
