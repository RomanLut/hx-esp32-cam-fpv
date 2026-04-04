#include "utils.h"

#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>

static bool _isRadxaZeroChecked = false;
static bool _isRadxaZero = false;

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
