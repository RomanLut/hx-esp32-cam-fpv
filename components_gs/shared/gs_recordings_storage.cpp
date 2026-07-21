#include "gs_recordings_storage.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>

#include "Log.h"
#include "avi.h"
#include "gs_runtime_core.h"
#include "gs_runtime_state.h"
#include "gs_shared_runtime.h"
#include "gs_shared_state.h"
#include "jpeg_parser.h"
#include "packets.h"

namespace
{

constexpr uint32_t kMaxRepeatedMissingFrames = 40;

//===================================================================================
//===================================================================================
// Counts skipped frame indexes between two increasing completed-frame numbers,
// clamped to the repeat cap. Lower numbers are treated as stream resets.
uint32_t countMissingFramesToRepeat(uint32_t current_frame_index, uint32_t previous_frame_index)
{
    if (current_frame_index <= previous_frame_index)
    {
        return 0;
    }

    const uint32_t skipped_frames = current_frame_index - previous_frame_index - 1u;
    return std::min(skipped_frames, kMaxRepeatedMissingFrames);
}

//===================================================================================
//===================================================================================
// Returns the configured FPS from the explicitly negotiated hardware and sensor mode.
uint8_t getRecordingFps(const Ground2Air_Config_Packet& config, const TVMode& mode)
{
    if (s_isOV5640)
    {
        return config.camera.ov5640HighFPS ? mode.highFPS5640 : mode.FPS5640;
    }
    if (s_isOV3660)
    {
        return config.camera.ov3660HighFPS ? mode.highFPS3660 : mode.FPS3660;
    }
    return config.camera.ov2640HighFPS ? mode.highFPS2640 : mode.FPS2640;
}

}

//===================================================================================
//===================================================================================
// Refreshes cached ground storage statistics from the platform-specific backend.
void RecordingsStorage::refreshGroundStorageStatus()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    GroundStorageStatus status = {};
    if (queryGroundStorageStatus(status))
    {
        m_ground_storage_status = status;
    }
    else
    {
        m_ground_storage_status = {};
    }
}

//===================================================================================
//===================================================================================
// Returns the latest cached ground storage status.
GroundStorageStatus RecordingsStorage::groundStorageStatus() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ground_storage_status;
}

//===================================================================================
//===================================================================================
// Reports whether ground video recording is currently active.
bool RecordingsStorage::isRecording() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_is_recording;
}

//===================================================================================
//===================================================================================
// Toggles ground recording using the shared AVI container workflow.
void RecordingsStorage::toggleRecording(int width, int height, const char* reason)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_is_recording)
    {
        stopRecordingLocked(reason);
        return;
    }

    startRecordingLocked(width, height, reason);
}

//===================================================================================
//===================================================================================
// Appends a completed video frame to the current recording and repeats the previous
// frame for short frame-index gaps before writing the newly arrived frame.
void RecordingsStorage::writeVideoFrame(const uint8_t* frame_data, size_t frame_size, uint32_t frame_index)
{
    if (frame_data == nullptr || frame_size == 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_is_recording)
    {
        return;
    }

#ifdef WRITE_RAW_MJPEG_STREAM
    const uint32_t missing_frames = m_has_previous_video_frame
        ? countMissingFramesToRepeat(frame_index, m_previous_video_frame_index)
        : 0;
    for (uint32_t repeat_index = 0; repeat_index < missing_frames; ++repeat_index)
    {
        if (!writeRecordingData(m_previous_video_frame.data(), m_previous_video_frame.size()))
        {
            stopRecordingLocked("raw_repeat_write_failed");
            return;
        }
    }

    if (!writeRecordingData(frame_data, frame_size))
    {
        stopRecordingLocked("raw_write_failed");
        return;
    }

    m_previous_video_frame.assign(frame_data, frame_data + frame_size);
    m_previous_video_frame_index = frame_index;
    m_has_previous_video_frame = true;
#else
    int width = 0;
    int height = 0;
    if (!getJPEGDimensions(const_cast<uint8_t*>(frame_data), width, height, 2048))
    {
        LOGI("Received frame - unknown size!");
        return;
    }

    const Ground2Air_Config_Packet config_snapshot = s_runtimeCore.session.copyConfigPacket();
    const uint32_t missing_frames = m_has_previous_video_frame
        ? countMissingFramesToRepeat(frame_index, m_previous_video_frame_index)
        : 0;
    // Repeating happens before applying the newly arrived frame's mode. If the
    // new frame changes resolution/FPS, the gap is filled in the old AVI segment.
    for (uint32_t repeat_index = 0; repeat_index < missing_frames; ++repeat_index)
    {
        if (!writeAviFrameLocked(m_previous_video_frame.data(),
                                 m_previous_video_frame.size(),
                                 m_previous_video_frame_width,
                                 m_previous_video_frame_height))
        {
            stopRecordingLocked("avi_repeat_write_failed");
            return;
        }
    }

    if (!updateRecordingModeLocked(width, height, config_snapshot))
    {
        return;
    }

    if (!writeAviFrameLocked(frame_data, frame_size, width, height))
    {
        stopRecordingLocked("avi_write_failed");
        return;
    }

    // Periodic fsync to bound the FUSE writeback queue on Android MediaStore-backed
    // FDs. Without this, ::write() returns success but the kernel can silently drop
    // pages once writeback pressure builds, leaving truncated files at stop.
    if ((m_avi_frame_count % 30U) == 0U)
    {
        flushRecordingFile();
    }

    m_previous_video_frame.assign(frame_data, frame_data + frame_size);
    m_previous_video_frame_index = frame_index;
    m_previous_video_frame_width = width;
    m_previous_video_frame_height = height;
    m_has_previous_video_frame = true;

    if ((m_avi_frame_count == (DVR_MAX_FRAMES - 1)) || (moviSize > 50 * 1024 * 1024))
    {
        stopRecordingLocked("auto_split_stop");
        startRecordingLocked(width, height, "auto_split_start");
    }
#endif
}

//===================================================================================
//===================================================================================
// Starts a new recording file using the current shared runtime configuration.
bool RecordingsStorage::startRecordingLocked(int width, int height, const char* reason)
{
    GroundStorageStatus status = {};
    if (queryGroundStorageStatus(status))
    {
        m_ground_storage_status = status;
    }

    if (m_ground_storage_status.free_space_bytes < kGSMinFreeSpaceBytes)
    {
        m_is_recording = false;
        return false;
    }

    const std::string path = buildRecordingPath(
#ifdef WRITE_RAW_MJPEG_STREAM
        "mjpeg"
#else
        "avi"
#endif
    );

#ifndef WRITE_RAW_MJPEG_STREAM
    const Ground2Air_Config_Packet config_snapshot = s_runtimeCore.session.copyConfigPacket();
    prepAviIndex();
    m_avi_frame_count = 0;

    const TVMode* mode_table = getVModeTable(s_isEsp32);
    const TVMode* mode = &mode_table[std::clamp(static_cast<int>(config_snapshot.camera.resolution), 0, static_cast<int>(Resolution::COUNT) - 1)];
    if (width != 0 && height != 0)
    {
        for (size_t index = 0; index < static_cast<size_t>(Resolution::COUNT); ++index)
        {
            if (mode_table[index].width == width && mode_table[index].height == height)
            {
                mode = &mode_table[index];
                break;
            }
        }
    }

    m_avi_fps = getRecordingFps(config_snapshot, *mode);

    m_avi_frame_width = static_cast<uint16_t>(width);
    m_avi_frame_height = static_cast<uint16_t>(height);
    m_avi_ov2640_high_fps = config_snapshot.camera.ov2640HighFPS;
    m_avi_ov3660_high_fps = config_snapshot.camera.ov3660HighFPS;
    m_avi_ov5640_high_fps = config_snapshot.camera.ov5640HighFPS;
#endif

    if (!openRecordingFile(path))
    {
        m_is_recording = false;
        return false;
    }

    m_is_recording = true;
    m_has_previous_video_frame = false;
    m_previous_video_frame_index = 0;
    m_previous_video_frame_width = 0;
    m_previous_video_frame_height = 0;
    m_previous_video_frame.clear();

#ifndef WRITE_RAW_MJPEG_STREAM
    LOGI("{}x{} {}fps", m_avi_frame_width, m_avi_frame_height, m_avi_fps);
    if (!writeRecordingData(aviHeader, AVI_HEADER_LEN))
    {
        closeRecordingFile();
        m_is_recording = false;
        return false;
    }
#endif

    LOGI("start record:{} reason:{}", path, reason ? reason : "unknown");
    if (queryGroundStorageStatus(status))
    {
        m_ground_storage_status = status;
    }
    return true;
}

//===================================================================================
//===================================================================================
// Finalizes and closes the current recording file.
void RecordingsStorage::stopRecordingLocked(const char* reason)
{
    if (!m_is_recording)
    {
        return;
    }

#ifndef WRITE_RAW_MJPEG_STREAM
    finalizeAviIndex(static_cast<uint16_t>(m_avi_frame_count));

    constexpr size_t kWriteBlockSize = 8192;
    std::array<uint8_t, kWriteBlockSize> write_block = {};
    while (true)
    {
        const size_t size = writeAviIndex(write_block.data(), write_block.size());
        if (size == 0)
        {
            break;
        }
        if (!writeRecordingData(write_block.data(), size))
        {
            break;
        }
    }

    buildAviHdr(m_avi_fps, m_avi_frame_width, m_avi_frame_height, static_cast<uint16_t>(m_avi_frame_count));
    // Force frames+idx out to storage before rewinding to overwrite the header.
    // On Android the recording FD is FUSE-backed; writeback errors only surface
    // here, and we want them visible before we trust the file is finalized.
    flushRecordingFile();
    seekRecordingFile(0, SEEK_SET);
    writeRecordingData(aviHeader, AVI_HEADER_LEN);
#endif

    flushRecordingFile();
    closeRecordingFile();
    m_is_recording = false;
    m_has_previous_video_frame = false;
    m_previous_video_frame_index = 0;
    m_previous_video_frame_width = 0;
    m_previous_video_frame_height = 0;
    m_previous_video_frame.clear();
    LOGI("stop record: reason:{}", reason ? reason : "unknown");
    GroundStorageStatus status = {};
    if (queryGroundStorageStatus(status))
    {
        m_ground_storage_status = status;
    }
}

//===================================================================================
//===================================================================================
// Restarts recording when video dimensions or FPS mode change at runtime.
bool RecordingsStorage::updateRecordingModeLocked(int width, int height, const Ground2Air_Config_Packet& config_snapshot)
{
    if ((width == m_avi_frame_width) &&
        (height == m_avi_frame_height) &&
        (m_avi_ov2640_high_fps == config_snapshot.camera.ov2640HighFPS) &&
        (m_avi_ov3660_high_fps == config_snapshot.camera.ov3660HighFPS) &&
        (m_avi_ov5640_high_fps == config_snapshot.camera.ov5640HighFPS))
    {
        return true;
    }

    if (m_avi_frame_count == 0 && (m_avi_frame_width == 0 || m_avi_frame_height == 0))
    {
        const TVMode* mode_table = getVModeTable(s_isEsp32);
        const TVMode* mode = &mode_table[std::clamp(static_cast<int>(config_snapshot.camera.resolution), 0, static_cast<int>(Resolution::COUNT) - 1)];
        for (size_t index = 0; index < static_cast<size_t>(Resolution::COUNT); ++index)
        {
            if (mode_table[index].width == width && mode_table[index].height == height)
            {
                mode = &mode_table[index];
                break;
            }
        }

        m_avi_fps = getRecordingFps(config_snapshot, *mode);

        // Manual recording can start before the first JPEG has revealed dimensions.
        // Keep the header-only file open and finalize its real dimensions later
        // instead of creating an immediate "(1)" duplicate.
        m_avi_frame_width = static_cast<uint16_t>(width);
        m_avi_frame_height = static_cast<uint16_t>(height);
        m_avi_ov2640_high_fps = config_snapshot.camera.ov2640HighFPS;
        m_avi_ov3660_high_fps = config_snapshot.camera.ov3660HighFPS;
        m_avi_ov5640_high_fps = config_snapshot.camera.ov5640HighFPS;
        return true;
    }

    stopRecordingLocked("auto_restart_resolution_change_stop");
    return startRecordingLocked(width, height, "auto_restart_resolution_change_start");
}

//===================================================================================
//===================================================================================
// Writes one MJPEG frame chunk into the current AVI recording.
bool RecordingsStorage::writeAviFrameLocked(const uint8_t* frame_data, size_t frame_size, int /*width*/, int /*height*/)
{
    const uint32_t jpeg_size = static_cast<uint32_t>(frame_size);
    const uint32_t filler = (4U - (jpeg_size & 0x3U)) & 0x3U;
    const uint32_t padded_size = jpeg_size + filler;

    std::array<uint8_t, 8> header = {};
    std::memcpy(header.data(), dcBuf, 4);
    std::memcpy(header.data() + 4, &padded_size, sizeof(padded_size));

    if (!writeRecordingData(header.data(), header.size()))
    {
        return false;
    }
    if (!writeRecordingData(frame_data, frame_size))
    {
        return false;
    }

    std::array<uint8_t, 4> padding = {};
    if (filler != 0 && !writeRecordingData(padding.data(), static_cast<size_t>(filler)))
    {
        return false;
    }

    buildAviIdx(padded_size);
    ++m_avi_frame_count;
    return true;
}

//===================================================================================
//===================================================================================
// Builds a timestamped recording path inside the platform-selected recordings directory.
std::string RecordingsStorage::buildRecordingPath(const char* extension) const
{
    std::time_t timestamp = std::time(nullptr);
    char filename[] = "yyyy-mm-dd-hh-mm-ss.avi";
    std::strftime(filename, sizeof(filename), "%Y-%m-%d-%H-%M-%S", std::localtime(&timestamp));

    std::string path = recordingDirectory();
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
    {
        path += '/';
    }
    path += filename;
    path += '.';
    path += extension;
    return path;
}

//===================================================================================
//===================================================================================
// Returns a sorted list of recording files found in the recordings directory.
std::vector<RecordingEntry> RecordingsStorage::listRecordings() const
{
    std::vector<RecordingEntry> entries;
    const std::string dir = recordingsListDirectory();

    DIR* dp = opendir(dir.c_str());
    if (!dp)
    {
        return entries;
    }

    struct dirent* de;
    while ((de = readdir(dp)) != nullptr)
    {
        const std::string filename = de->d_name;
        const size_t dot = filename.rfind('.');
        if (dot == std::string::npos)
        {
            continue;
        }

        const std::string ext = filename.substr(dot + 1);
        if (ext != "avi" && ext != "mjpeg")
        {
            continue;
        }

        std::string filepath = dir;
        if (!filepath.empty() && filepath.back() != '/' && filepath.back() != '\\')
        {
            filepath += '/';
        }
        filepath += filename;

        struct stat st = {};
        if (stat(filepath.c_str(), &st) != 0)
        {
            continue;
        }

        RecordingEntry entry;
        entry.name = filename.substr(0, dot);
        entry.extension = ext;
        entry.path = filepath;
        entry.size_kb = static_cast<size_t>(st.st_size / 1024);
        entries.push_back(std::move(entry));
    }
    closedir(dp);

    std::sort(entries.begin(), entries.end(), [](const RecordingEntry& a, const RecordingEntry& b) {
        return a.name > b.name;
    });

    return entries;
}

//===================================================================================
//===================================================================================
// Deletes a recording file from disk and refreshes ground storage statistics.
bool RecordingsStorage::deleteRecording(const std::string& path)
{
    if (path.empty())
    {
        return false;
    }

    if (std::remove(path.c_str()) != 0)
    {
        LOGW("delete recording failed: {}", path);
        return false;
    }

    LOGI("deleted recording: {}", path);
    refreshGroundStorageStatus();
    return true;
}
