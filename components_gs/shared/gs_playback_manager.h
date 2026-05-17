#pragma once

#include <atomic>
#include <array>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "gs_recordings_storage.h"

//===================================================================================
//===================================================================================
// Captures the current ground recording playback state for menu and overlay rendering.
struct PlaybackStatus
{
    bool active = false;
    bool broken = false;
    uint32_t current_frame = 0;
    uint32_t total_frames = 0;
    uint32_t current_ms = 0;
    uint32_t total_ms = 0;
    int speed_multiplier = 1;
    std::string message;
    std::string source_path;
};

//===================================================================================
//===================================================================================
// Provides shared playback state handling for files selected from the Playback menu.
class PlaybackManager
{
public:
    virtual ~PlaybackManager() = default;

    virtual bool startPlayback(const RecordingEntry& entry) = 0;
    virtual void stopPlayback() = 0;
    virtual void decreaseSpeed();
    virtual void increaseSpeed();
    virtual void togglePaused();
    virtual void stepForward();
    virtual PlaybackStatus status() const;

protected:
    static constexpr uint32_t kDefaultPlaybackFps = 30;
    static constexpr uint32_t kMinPlaybackFps = 1;
    static constexpr uint32_t kMaxPlaybackFps = 120;
    static constexpr int kPlaybackStoppedSpeedIndex = 4;
    static constexpr int kPlaybackNormalSpeedIndex = 5;
    static constexpr std::array<int, 9> kPlaybackSpeeds = {-8, -4, -2, -1, 0, 1, 2, 4, 8};

    //===================================================================================
    //===================================================================================
    // Closes a C FILE handle owned by a unique_ptr.
    struct FileCloser
    {
        void operator()(FILE* file) const;
    };

    //===================================================================================
    //===================================================================================
    // Stores the byte position and padded chunk size for one AVI MJPEG frame.
    struct AviFrameIndex
    {
        long chunk_offset = 0;
        uint32_t chunk_size = 0;
    };

    bool readAviHeader(FILE* file, uint32_t& fps, uint32_t& total_frames);
    bool buildAviFrameIndex(FILE* file, uint32_t total_frames, std::vector<AviFrameIndex>& frame_index);
    bool readAviFrameAt(FILE* file, const AviFrameIndex& frame_index, std::vector<uint8_t>& jpeg_frame);
    void runPlaybackFile(std::string path);
    int speedMultiplier() const;
    void setStatus(const PlaybackStatus& status);
    void markBroken(const std::string& message);
    std::string formatPlaybackTime(uint32_t milliseconds) const;

    mutable std::mutex m_mutex;
    PlaybackStatus m_status = {};
    std::atomic<bool> m_stop_requested = false;
    std::atomic<int> m_speed_index = kPlaybackNormalSpeedIndex;
    std::atomic<int> m_step_frames = 0;
    std::string m_source_path;

private:
    void adjustSpeed(int delta);
    virtual void submitPlaybackFrame(const std::vector<uint8_t>& jpeg_frame, uint32_t current_frame) = 0;
    virtual void onPlaybackFinished();
    virtual void onPlaybackStateChanged();
    virtual void logPlaybackFailure(const std::string& message);
};

extern PlaybackManager* s_playbackManager;
