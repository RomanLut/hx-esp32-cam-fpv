#pragma once

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "android_bitmap_jpeg_decoder.h"
#include "gs_playback_manager.h"

//===================================================================================
//===================================================================================
// Plays Android GS AVI recordings through the Android JPEG decoder.
class AndroidPlaybackManager final : public PlaybackManager
{
public:
    explicit AndroidPlaybackManager(AndroidBitmapJpegDecoder& jpeg_decoder);
    ~AndroidPlaybackManager() override;

    bool startPlayback(const RecordingEntry& entry) override;
    void stopPlayback() override;

private:
    void submitPlaybackFrame(const std::vector<uint8_t>& jpeg_frame, uint32_t current_frame) override;
    void logPlaybackFailure(const std::string& message) override;

    AndroidBitmapJpegDecoder& m_jpeg_decoder;
    std::thread m_thread;
};
