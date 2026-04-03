#include "android_recordings_storage.h"

#include <sys/statvfs.h>

#include "gs_shared_runtime.h"
#include "settings_storage.h"

namespace
{

//===================================================================================
//===================================================================================
// Owns the Android recordings storage instance for explicit shared binding.
AndroidRecordingsStorage s_android_recordings_storage;

//===================================================================================
//===================================================================================
// Returns the parent directory of the configured Android settings path.
std::string getAndroidRecordingsDirectory()
{
    const std::string settings_path = s_settingsStorage.path();
    const size_t separator = settings_path.find_last_of("/\\");
    if (separator == std::string::npos)
    {
        return ".";
    }

    const std::string directory = settings_path.substr(0, separator);
    return directory.empty() ? "." : directory;
}

} // namespace

//===================================================================================
//===================================================================================
// Returns the shared Android recordings storage instance for explicit binding.
RecordingsStorage& getAndroidRecordingsStorage()
{
    return s_android_recordings_storage;
}

//===================================================================================
//===================================================================================
// Queries Android app storage free space for ground recordings.
bool AndroidRecordingsStorage::queryGroundStorageStatus(GroundStorageStatus& status) const
{
    struct statvfs fs = {};
    const std::string directory = getAndroidRecordingsDirectory();
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
// Returns the Android directory where recording files are written.
std::string AndroidRecordingsStorage::recordingDirectory() const
{
    return getAndroidRecordingsDirectory();
}

//===================================================================================
//===================================================================================
// Opens an Android recording file for shared recording output.
bool AndroidRecordingsStorage::openRecordingFile(const std::string& path)
{
    m_record_file = std::fopen(path.c_str(), "wb+");
    return m_record_file != nullptr;
}

//===================================================================================
//===================================================================================
// Writes raw recording bytes into the current Android recording file.
bool AndroidRecordingsStorage::writeRecordingData(const void* data, size_t size)
{
    return m_record_file != nullptr && std::fwrite(data, size, 1, m_record_file) == 1;
}

//===================================================================================
//===================================================================================
// Repositions the current Android recording file cursor.
bool AndroidRecordingsStorage::seekRecordingFile(long offset, int origin)
{
    return m_record_file != nullptr && std::fseek(m_record_file, offset, origin) == 0;
}

//===================================================================================
//===================================================================================
// Flushes buffered Android recording data to disk.
void AndroidRecordingsStorage::flushRecordingFile()
{
    if (m_record_file != nullptr)
    {
        std::fflush(m_record_file);
    }
}

//===================================================================================
//===================================================================================
// Closes the active Android recording file.
void AndroidRecordingsStorage::closeRecordingFile()
{
    if (m_record_file != nullptr)
    {
        std::fclose(m_record_file);
        m_record_file = nullptr;
    }
}
