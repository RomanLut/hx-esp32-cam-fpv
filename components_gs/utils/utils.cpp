#include "utils.h"

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>

static bool _isRadxaZeroChecked = false;
static bool _isRadxaZero = false;

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