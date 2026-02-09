#include "util.h"
#ifndef ESP_PLATFORM
#include <fstream>
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

std::string trimHexPrefix(const std::string& value)
{
  if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0)
  {
    return value.substr(2);
  }
  return value;
}

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

std::string getInterfaceSummary(const std::string& iface)
{
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
  return summary;
}
#endif
