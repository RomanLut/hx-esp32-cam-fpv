#include "linux_serial_telemetry.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "gs_runtime_config.h"
#include "gs_shared_state.h"
#include "utils/utils.h"

//===================================================================================
//===================================================================================
// Resolves the default Linux telemetry serial port for the current device.
std::string LinuxSerialTelemetry::resolveSerialPortName(const std::string& port_name)
{
    if (!port_name.empty())
    {
        return port_name;
    }
    return resolveAutoPortName();
}

//===================================================================================
//===================================================================================
// Picks the platform's default UART when "auto" is selected.
std::string LinuxSerialTelemetry::resolveAutoPortName()
{
    if (access("/dev/ttyUSB0", F_OK) == 0)
    {
        return "/dev/ttyUSB0";
    }
    return isRadxaZero3() ? "/dev/ttyS3" : "/dev/serial0";
}

//===================================================================================
//===================================================================================
// Closes the serial port if open.
LinuxSerialTelemetry::~LinuxSerialTelemetry()
{
    close();
}

//===================================================================================
//===================================================================================
void LinuxSerialTelemetry::close()
{
    if (m_fd != -1)
    {
        ::close(m_fd);
        m_fd = -1;
    }
    m_current_port.clear();
}

//===================================================================================
//===================================================================================
// Opens and configures the serial port at 115200 baud. Returns false if unavailable (non-fatal).
bool LinuxSerialTelemetry::init(const std::string& port_name)
{
    const std::string resolved_port_name = resolveSerialPortName(port_name);

    m_fd = open(resolved_port_name.c_str(), O_RDWR);
    if (m_fd == -1)
    {
        printf("Warning: Can not open serial port %s. Telemetry will not be available.\n", resolved_port_name.c_str());
        return false;
    }

    struct termios tty;
    if (tcgetattr(m_fd, &tty) != 0)
    {
        printf("Error %i from tcgetattr: %s\n", errno, strerror(errno));
        return false;
    }

    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_lflag &= ~ICANON;
    tty.c_lflag &= ~ECHO;
    tty.c_lflag &= ~ECHOE;
    tty.c_lflag &= ~ECHONL;
    tty.c_lflag &= ~ISIG;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;
    tty.c_cc[VTIME] = 0;
    tty.c_cc[VMIN] = 0;

    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);

    if (tcsetattr(m_fd, TCSANOW, &tty) != 0)
    {
        printf("Error %i from tcsetattr: %s\n", errno, strerror(errno));
        return false;
    }

    m_current_port = resolved_port_name;
    return true;
}

//===================================================================================
//===================================================================================
// Returns true if the serial port was successfully opened.
bool LinuxSerialTelemetry::isOpen() const
{
    return m_fd != -1;
}

//===================================================================================
//===================================================================================
// Non-blocking read. Returns number of bytes read, or -1 on error.
int LinuxSerialTelemetry::read(uint8_t* buf, size_t max_bytes)
{
    return static_cast<int>(::read(m_fd, buf, max_bytes));
}

//===================================================================================
//===================================================================================
// Writes outbound telemetry bytes to the serial port.
void LinuxSerialTelemetry::write(const uint8_t* data, size_t size)
{
    [[maybe_unused]] ssize_t res = ::write(m_fd, data, size);
}

//===================================================================================
//===================================================================================
namespace
{

// Reads a single-line file under /sys (e.g., idVendor / idProduct / product) and
// returns its trimmed contents, or an empty string on any error.
std::string readSysfsLine(const std::string& path)
{
    std::ifstream f(path);
    if (!f) return {};
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
    {
        line.pop_back();
    }
    return line;
}

// Returns the basename of /sys/class/tty/<name>'s parent device, used to look up
// USB descriptors. e.g. /sys/class/tty/ttyUSB0/device -> usb0/0-1/0-1:1.0
std::string sysfsDeviceDirForTty(const std::string& tty_basename)
{
    return "/sys/class/tty/" + tty_basename + "/device";
}

// Walks up from /sys/class/tty/<name>/device looking for the nearest ancestor
// containing idVendor + idProduct (the USB device node). Returns its path or "".
std::string sysfsUsbDeviceDirForTty(const std::string& tty_basename)
{
    std::string dev = sysfsDeviceDirForTty(tty_basename);
    char resolved[PATH_MAX];
    if (realpath(dev.c_str(), resolved) == nullptr) return {};
    std::string cur = resolved;
    for (int i = 0; i < 6; i++)
    {
        struct stat st;
        std::string idv = cur + "/idVendor";
        if (stat(idv.c_str(), &st) == 0) return cur;
        const auto slash = cur.find_last_of('/');
        if (slash == std::string::npos || slash == 0) return {};
        cur = cur.substr(0, slash);
    }
    return {};
}

void scanGlobInto(const char* dir, const char* prefix, std::set<std::string>& out)
{
    DIR* d = opendir(dir);
    if (!d) return;
    while (dirent* ent = readdir(d))
    {
        if (ent->d_name[0] == '.') continue;
        const std::string name = ent->d_name;
        if (name.compare(0, std::strlen(prefix), prefix) != 0) continue;
        out.insert(std::string(dir) + "/" + name);
    }
    closedir(d);
}

}  // namespace

//===================================================================================
//===================================================================================
// Linux platform implementation: enumerates serial devices under /dev.
std::vector<std::string> listAvailableTelemetryUarts()
{
    std::set<std::string> uarts;
    scanGlobInto("/dev", "ttyUSB", uarts);
    scanGlobInto("/dev", "ttyACM", uarts);
    scanGlobInto("/dev", "ttyS", uarts);
    scanGlobInto("/dev", "serial", uarts);
    return std::vector<std::string>(uarts.begin(), uarts.end());
}

//===================================================================================
//===================================================================================
// Enriches a /dev/tty path with the sysfs product name when available.
std::string getTelemetryUartDisplayLabel(const std::string& identifier)
{
    if (identifier.empty()) return identifier;
    const auto slash = identifier.find_last_of('/');
    const std::string tty = (slash == std::string::npos) ? identifier : identifier.substr(slash + 1);
    const std::string usb_dir = sysfsUsbDeviceDirForTty(tty);
    if (usb_dir.empty()) return identifier;

    const std::string product = readSysfsLine(usb_dir + "/product");
    if (product.empty()) return identifier;
    return identifier + " (" + product + ")";
}

//===================================================================================
//===================================================================================
// Closes / opens the underlying serial port to match s_groundstation_config.telemetryUart.
// Idempotent — safe to call from a periodic loop for hot-plug retries.
void applySelectedTelemetryUart()
{
    LinuxSerialTelemetry* lst = static_cast<LinuxSerialTelemetry*>(g_serialTelemetry);
    if (lst == nullptr) return;

    const std::string& selection = s_groundstation_config.telemetryUart;
    std::string desired_port;
    if (selection == "none")
    {
        desired_port.clear();
    }
    else if (selection == "auto")
    {
        desired_port = LinuxSerialTelemetry::resolveAutoPortName();
    }
    else
    {
        desired_port = selection;
    }

    // Close if the open port no longer matches the desired one.
    if (lst->isOpen() && lst->currentPort() != desired_port)
    {
        printf("Closing telemetry UART %s (selection changed)\n", lst->currentPort().c_str());
        lst->close();
    }

    if (!lst->isOpen() && !desired_port.empty())
    {
        if (access(desired_port.c_str(), F_OK) == 0)
        {
            lst->init(desired_port);
        }
    }
}
