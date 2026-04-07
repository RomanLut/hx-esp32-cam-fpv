#include "utils.h"

#include <array>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <memory>

#include "Log.h"

static bool _isRadxaZeroChecked = false;
static bool _isRadxaZero = false;

namespace
{

//===================================================================================
//===================================================================================
// Closes a FILE handle returned by popen().
struct PipeCloser
{
    void operator()(FILE* pipe) const
    {
        if (pipe != nullptr)
        {
            pclose(pipe);
        }
    }
};

}

//===================================================================================
//===================================================================================
// Configures stdin to non-blocking mode so the main loop can poll for input
// without stalling.
void setupNonBlockingInput()
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

//===================================================================================
//===================================================================================
// Runs a shell command, optionally capturing stdout, and reports failures.
bool runShellCommand(const std::string& command, std::string* output)
{
    if (output == nullptr)
    {
        const int result = std::system(command.c_str());
        if (result != 0)
        {
            LOGW("Shell command failed ({}): {}", result, command);
            return false;
        }

        return true;
    }

    std::array<char, 128> buffer = {};
    std::string result;
    std::unique_ptr<FILE, PipeCloser> pipe(popen(command.c_str(), "r"));
    if (!pipe)
    {
        LOGW("popen() failed for command: {}", command);
        return false;
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr)
    {
        result += buffer.data();
    }

    const int status = pclose(pipe.release());
    if (status != 0)
    {
        LOGW("Shell command failed ({}): {}", status, command);
        return false;
    }

    *output = std::move(result);
    return true;
}

//======================================================
//======================================================
bool isRadxaZero3()
{
    if (_isRadxaZeroChecked)
    {
        return _isRadxaZero;
    }
    _isRadxaZeroChecked = true;

    std::ifstream compatibleFile("/proc/device-tree/compatible");

    // Check /proc/device-tree/compatible for Radxa Zero 3W
    if (compatibleFile.is_open())
    {
        std::ostringstream content;
        content << compatibleFile.rdbuf();
        if (content.str().find("radxa,zero3") != std::string::npos)
        {
            compatibleFile.close();
            _isRadxaZero = true;
            return true;
        }
        compatibleFile.close();
    }

    _isRadxaZero = false;
    return false;
}

//======================================================
//======================================================
int formatGSRSSI(int8_t rssi)
{
    if (rssi == 0)
    {
        return 0;
    }
    return std::max(-99, static_cast<int>(rssi));
}
