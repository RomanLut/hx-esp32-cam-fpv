#include "gs_linux_recordings_storage.h"

#include <limits.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <string>

namespace
{

//===================================================================================
//===================================================================================
// Returns the directory that contains the running Linux GS executable.
std::string getExecutableDirectory()
{
    char path[PATH_MAX] = {};
    const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0)
    {
        return ".";
    }

    path[length] = '\0';
    std::string executable_path(path);
    const size_t separator = executable_path.find_last_of('/');
    if (separator == std::string::npos)
    {
        return ".";
    }
    if (separator == 0)
    {
        return "/";
    }
    return executable_path.substr(0, separator);
}

//===================================================================================
//===================================================================================
// Owns the Linux recordings storage instance for explicit shared binding.
LinuxRecordingsStorage s_linux_recordings_storage;

} // namespace

//===================================================================================
//===================================================================================
// Returns the shared Linux recordings storage instance for explicit binding.
RecordingsStorage& getLinuxRecordingsStorage()
{
    return s_linux_recordings_storage;
}

//===================================================================================
//===================================================================================
// Queries free space in the Linux GS recordings directory.
bool LinuxRecordingsStorage::queryGroundStorageStatus(GroundStorageStatus& status) const
{
    struct statvfs fs = {};
    const std::string directory = recordingDirectory();
    if (statvfs(directory.c_str(), &fs) != 0)
    {
        status = {};
        return false;
    }

    status.free_space_bytes = static_cast<uint64_t>(fs.f_bavail) * static_cast<uint64_t>(fs.f_frsize);
    status.total_space_bytes = static_cast<uint64_t>(fs.f_blocks) * static_cast<uint64_t>(fs.f_frsize);
    return true;
}

//===================================================================================
//===================================================================================
// Returns the Linux GS directory where recording files are written.
std::string LinuxRecordingsStorage::recordingDirectory() const
{
    return getExecutableDirectory();
}

//===================================================================================
//===================================================================================
// Opens a Linux recording file for shared recording output.
bool LinuxRecordingsStorage::openRecordingFile(const std::string& path)
{
    m_record_file = std::fopen(path.c_str(), "wb+");
    return m_record_file != nullptr;
}

//===================================================================================
//===================================================================================
// Writes raw recording bytes into the current Linux recording file.
bool LinuxRecordingsStorage::writeRecordingData(const void* data, size_t size)
{
    return m_record_file != nullptr && std::fwrite(data, size, 1, m_record_file) == 1;
}

//===================================================================================
//===================================================================================
// Repositions the current Linux recording file cursor.
bool LinuxRecordingsStorage::seekRecordingFile(long offset, int origin)
{
    return m_record_file != nullptr && std::fseek(m_record_file, offset, origin) == 0;
}

//===================================================================================
//===================================================================================
// Flushes buffered Linux recording data to disk.
void LinuxRecordingsStorage::flushRecordingFile()
{
    if (m_record_file != nullptr)
    {
        std::fflush(m_record_file);
    }
}

//===================================================================================
//===================================================================================
// Closes the active Linux recording file.
void LinuxRecordingsStorage::closeRecordingFile()
{
    if (m_record_file != nullptr)
    {
        std::fclose(m_record_file);
        m_record_file = nullptr;
    }
}
