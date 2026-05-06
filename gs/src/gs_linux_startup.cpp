#include "gs_linux_startup.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../components/common/Clock.h"
#include "util.h"
#include "utils.h"

//===================================================================================
//===================================================================================
// Returns the single-instance pid file path: prefer XDG_RUNTIME_DIR so a root-run gs
// cannot leave an unwritable /tmp pid that blocks a normal-user verify or dev launch.
static std::string gsLinuxInstancePidPath()
{
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if(xdg != nullptr && xdg[0] != '\0')
    {
        std::string base(xdg);
        if(base.back() == '/')
        {
            return base + "esp32_cam_fpv_gs.pid";
        }
        return base + "/esp32_cam_fpv_gs.pid";
    }
    // Non-interactive shells (e.g. wsl.exe bash -lc) often omit XDG_RUNTIME_DIR; use /run/user/<uid> when present.
    const std::string run_user = std::string("/run/user/") + std::to_string(static_cast<long long>(getuid()));
    struct stat st;
    if(stat(run_user.c_str(), &st) == 0 && S_ISDIR(st.st_mode) != 0 && access(run_user.c_str(), W_OK) == 0)
    {
        return run_user + "/esp32_cam_fpv_gs.pid";
    }
    return std::string("/tmp/esp32_cam_fpv_gs.pid");
}

//===================================================================================
//===================================================================================
// Removes PID file from previous application run
void cleanupLinuxSingleInstancePidFile()
{
    unlink(gsLinuxInstancePidPath().c_str());
}

//===================================================================================
//===================================================================================
// Reads PID of running GS instance from file
static pid_t readRunningInstancePid()
{
    const std::string path = gsLinuxInstancePidPath();
    std::ifstream pid_file(path);
    pid_t pid = 0;
    pid_file >> pid;
    return pid;
}

//===================================================================================
//===================================================================================
// Waits for process with given PID to exit within specified timeout
static bool waitForProcessExit(pid_t pid, std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;

    while (Clock::now() < deadline)
    {
        if (kill(pid, 0) != 0)
        {
            return errno == ESRCH;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return kill(pid, 0) != 0 && errno == ESRCH;
}

//===================================================================================
//===================================================================================
// Ensures only one GS instance is running on the system
bool ensureLinuxSingleInstance()
{
    const pid_t current_pid = getpid();
    const pid_t existing_pid = readRunningInstancePid();

    if (existing_pid > 0 && existing_pid != current_pid)
    {
        if (kill(existing_pid, 0) == 0 || errno == EPERM)
        {
            printf("Existing gs instance detected with pid %d. Sending SIGTERM...\n", (int)existing_pid);
            kill(existing_pid, SIGTERM);

            if (!waitForProcessExit(existing_pid, std::chrono::seconds(5)))
            {
                printf("Existing gs instance did not exit after SIGTERM.\n");
                return false;
            }
        }
        else if (errno == ESRCH)
        {
            cleanupLinuxSingleInstancePidFile();
        }
    }

    const std::string pid_path = gsLinuxInstancePidPath();
    std::ofstream pid_file(pid_path, std::ios::trunc);
    if (!pid_file.is_open())
    {
        printf("Can not open pid file %s\n", pid_path.c_str());
        return false;
    }

    pid_file << current_pid;
    pid_file.close();
    return true;
}

//===================================================================================
//===================================================================================
// Parses airmon-ng output and returns list of supported wifi interfaces
static std::vector<std::string> getInterfacesWithChipset(const std::string& output)
{
    std::istringstream iss(output);
    std::string line;
    std::vector<std::string> interfaces;

    while (std::getline(iss, line))
    {
        std::istringstream line_stream(line);
        std::string phy, interface, driver, chipset;

        line_stream >> phy >> interface >> driver;
        std::getline(line_stream, chipset);

        std::string driver_lower = toLowerCopy(driver);
        std::string chipset_lower = toLowerCopy(chipset);

        if ((chipset_lower.find("8812") != std::string::npos) ||
            (chipset_lower.find("9271") != std::string::npos) ||
            (driver_lower.find("rtl88") != std::string::npos) ||
            (driver_lower.find("ath9k") != std::string::npos))
        {
            interfaces.push_back(interface);
        }
    }

    return interfaces;
}

//===================================================================================
//===================================================================================
// Scans system for available RX wifi interfaces
void findLinuxRXInterfacesEx(gs::core::RXDescriptor& rx_descriptor)
{
    std::string output;
    if (!runShellCommand("sudo airmon-ng", &output))
    {
        throw std::runtime_error("Failed to query wifi interfaces with airmon-ng");
    }
    rx_descriptor.interfaces = getInterfacesWithChipset(output);
}

//===================================================================================
//===================================================================================
// Scans for RX wifi interfaces with retries and initialization wait
bool findLinuxRXInterfaces(gs::core::RXDescriptor& rx_descriptor)
{
    rx_descriptor.interfaces.clear();

    for (int i = 10; i >= 0; i--)
    {
        findLinuxRXInterfacesEx(rx_descriptor);
        if (rx_descriptor.interfaces.size() != 0) break;
        printf("Waiting for wifi interface... %d\n", i);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (rx_descriptor.interfaces.size() == 0) return false;

    printf("Found RX interface: %s\n", rx_descriptor.interfaces[0].c_str());

    for (int i = 3; i >= 0; i--)
    {
        rx_descriptor.interfaces.clear();
        findLinuxRXInterfacesEx(rx_descriptor);
        if (rx_descriptor.interfaces.size() == 2) break;
        printf("Waiting for 2nd wifi interface... %d\n", i);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (rx_descriptor.interfaces.size() > 1)
    {
        printf("Found RX interface: %s\n", rx_descriptor.interfaces[1].c_str());
    }
    else
    {
        printf("Second RX interface not found.\n");
    }

    return rx_descriptor.interfaces.size() != 0;
}

//===================================================================================
//===================================================================================
// Generates unique device identifier based on machine-id
uint16_t generateLinuxDeviceId()
{
    std::ifstream file("/etc/machine-id");
    std::string machine_id;

    if (file.is_open())
    {
        std::getline(file, machine_id);
        file.close();
    }

    uint16_t device_id = 0;
    if (!machine_id.empty())
    {
        for (size_t i = 0; i < machine_id.size(); ++i)
        {
            device_id ^= (uint16_t)machine_id[i] << (i % 8);
        }
    }
    else
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint16_t> dist(0, 0xFFFF);
        device_id = dist(gen);
    }

    return device_id;
}
