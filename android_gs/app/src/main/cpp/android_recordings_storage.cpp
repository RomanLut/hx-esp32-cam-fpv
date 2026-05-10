#include "android_recordings_storage.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "Log.h"
#include "gs_shared_runtime.h"
#include "settings_storage.h"

// Implemented in native_bridge.cpp — creates a MediaStore file and returns its FD.
int createRecordingFdFromNative(const std::string& path);

namespace
{

//===================================================================================
//===================================================================================
// Owns the Android recordings storage instance for explicit shared binding.
AndroidRecordingsStorage s_android_recordings_storage;

//===================================================================================
//===================================================================================
// Holds the externally configured recordings directory, or empty to use the
// settings file directory as a fallback.
std::string s_recordings_directory;

//===================================================================================
//===================================================================================
// Returns the parent directory of the configured Android settings path.
std::string getAndroidRecordingsDirectory()
{
    if (!s_recordings_directory.empty())
    {
        return s_recordings_directory;
    }

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
// Sets the directory where Android recordings are written.
void setAndroidRecordingsDirectory(const std::string& directory)
{
    s_recordings_directory = directory;
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
// Returns the Android directory where recording files can be listed.
std::string AndroidRecordingsStorage::recordingsListDirectory() const
{
    std::string dir = getAndroidRecordingsDirectory();
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
    {
        dir += '/';
    }
    dir += "Movies/esp32-cam-fpv";
    return dir;
}

//===================================================================================
//===================================================================================
// Opens an Android recording file via MediaStore and keeps the raw FD.
bool AndroidRecordingsStorage::openRecordingFile(const std::string& path)
{
    m_record_fd = createRecordingFdFromNative(path);
    return m_record_fd >= 0;
}

//===================================================================================
//===================================================================================
// Writes raw recording bytes into the current Android recording file. Writes go
// directly via ::write() (no stdio buffering) and short writes are retried so a
// FUSE-shim partial write surfaces as a real failure instead of being hidden.
bool AndroidRecordingsStorage::writeRecordingData(const void* data, size_t size)
{
    if (m_record_fd < 0)
    {
        return false;
    }
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    while (remaining > 0)
    {
        const ssize_t written = ::write(m_record_fd, cursor, remaining);
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOGW("recording write failed errno={} ({})", errno, std::strerror(errno));
            return false;
        }
        if (written == 0)
        {
            LOGW("recording write returned 0 (FUSE drop?)");
            return false;
        }
        cursor += written;
        remaining -= static_cast<size_t>(written);
    }
    return true;
}

//===================================================================================
//===================================================================================
// Repositions the current Android recording file cursor.
bool AndroidRecordingsStorage::seekRecordingFile(long offset, int origin)
{
    return m_record_fd >= 0 &&
           ::lseek(m_record_fd, static_cast<off_t>(offset), origin) != static_cast<off_t>(-1);
}

//===================================================================================
//===================================================================================
// Forces buffered kernel/FUSE writeback for the recording. Errors here surface
// silent FUSE writeback failures that ::write() returned success for.
void AndroidRecordingsStorage::flushRecordingFile()
{
    if (m_record_fd >= 0 && ::fsync(m_record_fd) != 0)
    {
        LOGW("recording fsync failed errno={} ({})", errno, std::strerror(errno));
    }
}

//===================================================================================
//===================================================================================
// Closes the active Android recording file.
void AndroidRecordingsStorage::closeRecordingFile()
{
    if (m_record_fd >= 0)
    {
        ::close(m_record_fd);
        m_record_fd = -1;
    }
}
