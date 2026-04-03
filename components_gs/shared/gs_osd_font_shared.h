#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

const std::string& getDefaultOsdFontName();
const std::vector<std::string>& getBuiltinOsdFontNames();
std::string getSelectedOsdFontName();
void setSelectedOsdFontName(const std::string& font_name);
std::optional<std::string> findOsdFontNameByCrc(const std::vector<std::string>& font_names, uint32_t font_crc32);
