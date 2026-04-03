#pragma once

#include <cstdio>

#include "gs_recordings_storage.h"

//===================================================================================
//===================================================================================
// Implements Linux-specific filesystem access for shared recording logic.
class LinuxRecordingsStorage final : public RecordingsStorage
{
protected:
    bool queryGroundStorageStatus(GroundStorageStatus& status) const override;
    std::string recordingDirectory() const override;
    bool openRecordingFile(const std::string& path) override;
    bool writeRecordingData(const void* data, size_t size) override;
    bool seekRecordingFile(long offset, int origin) override;
    void flushRecordingFile() override;
    void closeRecordingFile() override;

private:
    FILE* m_record_file = nullptr;
};

//===================================================================================
//===================================================================================
// Returns the shared Linux recordings storage instance for explicit binding.
RecordingsStorage& getLinuxRecordingsStorage();
