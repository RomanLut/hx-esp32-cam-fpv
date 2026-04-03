#include "gs_linux_recordings_storage.h"

#include <sys/statvfs.h>

namespace
{

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
// Queries Linux filesystem free space for ground recordings.
bool LinuxRecordingsStorage::queryGroundStorageStatus(GroundStorageStatus& status) const
{
    struct statvfs fs = {};
    if (statvfs(".", &fs) != 0)
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
// Returns the Linux directory where recording files are written.
std::string LinuxRecordingsStorage::recordingDirectory() const
{
    return ".";
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
