#pragma once

#include "gs_recordings_storage.h"

//===================================================================================
//===================================================================================
// Implements Android-specific filesystem access for shared recording logic.
class AndroidRecordingsStorage final : public RecordingsStorage
{
protected:
    bool queryGroundStorageStatus(GroundStorageStatus& status) const override;
    std::string recordingDirectory() const override;
    std::string recordingsListDirectory() const override;
    bool openRecordingFile(const std::string& path) override;
    bool writeRecordingData(const void* data, size_t size) override;
    bool seekRecordingFile(long offset, int origin) override;
    void flushRecordingFile() override;
    void closeRecordingFile() override;

private:
    // MediaStore-backed FDs run through Android's FUSE shim; stdio buffering hid
    // silent partial writes there, so the recorder uses raw POSIX I/O directly.
    int m_record_fd = -1;
};

//===================================================================================
//===================================================================================
// Returns the shared Android recordings storage instance for explicit binding.
RecordingsStorage& getAndroidRecordingsStorage();

//===================================================================================
//===================================================================================
// Sets the directory where Android recordings are written.
void setAndroidRecordingsDirectory(const std::string& directory);
