#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "packets.h"

//===================================================================================
//===================================================================================
// Describes ground storage capacity and free space for recordings.
struct GroundStorageStatus
{
    uint64_t free_space_bytes = 0;
    uint64_t total_space_bytes = 0;
};

//===================================================================================
//===================================================================================
// Provides shared recording logic with platform-specific storage operations.
class RecordingsStorage
{
public:
    virtual ~RecordingsStorage() = default;

    void refreshGroundStorageStatus();
    GroundStorageStatus groundStorageStatus() const;
    bool isRecording() const;
    void toggleRecording(int width, int height, const char* reason);
    void writeVideoFrame(const uint8_t* frame_data, size_t frame_size);

protected:
    virtual bool queryGroundStorageStatus(GroundStorageStatus& status) const = 0;
    virtual std::string recordingDirectory() const = 0;
    virtual bool openRecordingFile(const std::string& path) = 0;
    virtual bool writeRecordingData(const void* data, size_t size) = 0;
    virtual bool seekRecordingFile(long offset, int origin) = 0;
    virtual void flushRecordingFile() = 0;
    virtual void closeRecordingFile() = 0;

private:
    bool startRecordingLocked(int width, int height, const char* reason);
    void stopRecordingLocked(const char* reason);
    bool updateRecordingModeLocked(int width, int height, const Ground2Air_Config_Packet& config_snapshot);
    bool writeAviFrameLocked(const uint8_t* frame_data, size_t frame_size, int width, int height);
    std::string buildRecordingPath(const char* extension) const;

    mutable std::mutex m_mutex;
    GroundStorageStatus m_ground_storage_status = {};
    bool m_is_recording = false;
    uint8_t m_avi_fps = 0;
    uint16_t m_avi_frame_width = 0;
    uint16_t m_avi_frame_height = 0;
    uint32_t m_avi_frame_count = 0;
    bool m_avi_ov2640_high_fps = false;
    bool m_avi_ov5640_high_fps = false;
};

extern RecordingsStorage* s_recordingsStorage;
