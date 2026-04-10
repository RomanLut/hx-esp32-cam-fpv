#include "util.h"
#ifndef ESP_PLATFORM
#include <algorithm>
#include <cctype>
#include <fstream>
#include <chrono>
#include <map>
#include <mutex>
#include <unistd.h>
#endif

//==============================================================
//==============================================================
int smallestPowerOfTwo(int value, int minValue)
{
  if (value < minValue)
  {
    return minValue; 
  }

  if (value < 2)
  {
    return 2;
  }

  int powerOfTwo = 1;
  while (powerOfTwo < value)
  {
    powerOfTwo <<= 1;
  }

  return powerOfTwo;
}

#ifndef ESP_PLATFORM
//===================================================================================
//===================================================================================
// Returns lowercased copy of input string
std::string toLowerCopy(const std::string& value)
{
  std::string lowerValue = value;
  std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(), [](unsigned char ch)
  {
    return (char)std::tolower(ch);
  });
  return lowerValue;
}

//===================================================================================
//===================================================================================
// Reads first line from text file
std::string readTextFileFirstLine(const std::string& path)
{
  std::ifstream file(path);
  if (!file.is_open())
  {
    return "";
  }
  std::string line;
  std::getline(file, line);
  return line;
}

//===================================================================================
//===================================================================================
// Removes hexadecimal prefix from string value
std::string trimHexPrefix(const std::string& value)
{
  if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0)
  {
    return value.substr(2);
  }
  return value;
}

//===================================================================================
//===================================================================================
// Reads symlink target and returns its basename
std::string readSymlinkBasename(const std::string& path)
{
  char target[512] = {0};
  ssize_t n = readlink(path.c_str(), target, sizeof(target) - 1);
  if (n <= 0)
  {
    return "";
  }
  target[n] = 0;
  std::string s(target);
  size_t p = s.find_last_of('/');
  return p == std::string::npos ? s : s.substr(p + 1);
}

//===================================================================================
//===================================================================================
// Builds short summary string for network interface details
std::string getInterfaceSummary(const std::string& iface)
{
  struct CachedInterfaceSummary
  {
    std::string summary;
    std::chrono::steady_clock::time_point refresh_tp = std::chrono::steady_clock::now();
  };

  static std::mutex s_interface_summary_cache_mutex;
  static std::map<std::string, CachedInterfaceSummary> s_interface_summary_cache;
  constexpr auto kInterfaceSummaryCacheTtl = std::chrono::seconds(3);

  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(s_interface_summary_cache_mutex);
    const auto it = s_interface_summary_cache.find(iface);
    if (it != s_interface_summary_cache.end() &&
        now - it->second.refresh_tp < kInterfaceSummaryCacheTtl)
    {
      return it->second.summary;
    }
  }

  std::string base = std::string("/sys/class/net/") + iface;
  std::string phy = readSymlinkBasename(base + "/phy80211");
  std::string driver = readSymlinkBasename(base + "/device/driver");
  std::string vendor = trimHexPrefix(readTextFileFirstLine(base + "/device/vendor"));
  std::string device = trimHexPrefix(readTextFileFirstLine(base + "/device/device"));

  std::string summary = iface + ": ";
  if (!phy.empty())
  {
    summary += phy + " ";
  }
  if (!driver.empty())
  {
    summary += driver + " ";
  }
  if (!vendor.empty() && !device.empty())
  {
    summary += vendor + ":" + device;
  }
  if (summary == iface + ": ")
  {
    summary += "adapter info unavailable";
  }

  {
    std::lock_guard<std::mutex> lock(s_interface_summary_cache_mutex);
    s_interface_summary_cache[iface] = {summary, now};
  }

  return summary;
}
#endif
