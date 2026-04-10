#include "utils.h"

#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

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

//===================================================================================
//===================================================================================
// Finds an executable by searching the current PATH first, then common Linux system directories.
std::optional<std::string> findExecutablePath(const std::string& executable_name)
{
    if (executable_name.empty())
    {
        return std::nullopt;
    }

    std::vector<std::string> search_dirs;
    std::set<std::string> seen_dirs;

    const char* path_env = std::getenv("PATH");
    if (path_env != nullptr)
    {
        std::stringstream ss(path_env);
        std::string path_entry;
        while (std::getline(ss, path_entry, ':'))
        {
            if (!path_entry.empty() && seen_dirs.insert(path_entry).second)
            {
                search_dirs.push_back(path_entry);
            }
        }
    }

    static constexpr const char* kFallbackDirs[] = {
        "/usr/local/sbin",
        "/usr/local/bin",
        "/usr/sbin",
        "/usr/bin",
        "/sbin",
        "/bin"
    };
    for (const char* dir : kFallbackDirs)
    {
        if (seen_dirs.insert(dir).second)
        {
            search_dirs.emplace_back(dir);
        }
    }

    for (const std::string& dir : search_dirs)
    {
        const std::string candidate = dir + "/" + executable_name;
        if (access(candidate.c_str(), X_OK) == 0)
        {
            return candidate;
        }
    }

    return std::nullopt;
}

//===================================================================================
//===================================================================================
// Trims leading and trailing ASCII whitespace from a string.
std::string trimAsciiWhitespace(const std::string& value)
{
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0)
    {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
    {
        --end;
    }

    return value.substr(start, end - start);
}

//===================================================================================
//===================================================================================
// Escapes a string so it can be safely embedded as a single shell argument.
std::string shellQuote(const std::string& value)
{
    std::string quoted = "'";
    for (const char ch : value)
    {
        if (ch == '\'')
        {
            quoted += "'\\''";
        }
        else
        {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
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
