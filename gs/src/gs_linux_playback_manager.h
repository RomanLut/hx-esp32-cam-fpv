#pragma once

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "gs_playback_manager.h"

//===================================================================================
//===================================================================================
// Plays local Linux GS AVI recordings through the normal JPEG decoder path.
class LinuxPlaybackManager final : public PlaybackManager
{
public:
    LinuxPlaybackManager();
    ~LinuxPlaybackManager() override;

    bool startPlayback(const RecordingEntry& entry) override;
    void stopPlayback() override;

private:
    void submitPlaybackFrame(const std::vector<uint8_t>& jpeg_frame, uint32_t current_frame) override;
    void logPlaybackFailure(const std::string& message) override;

    std::thread m_thread;
};

LinuxPlaybackManager& getLinuxPlaybackManager();
