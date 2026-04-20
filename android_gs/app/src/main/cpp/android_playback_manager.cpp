#include "android_playback_manager.h"

#include <memory>
#include <thread>

#include "Log.h"

//===================================================================================
//===================================================================================
// Stores the Android decoder reference used for playback.
AndroidPlaybackManager::AndroidPlaybackManager(AndroidBitmapJpegDecoder& jpeg_decoder)
    : m_jpeg_decoder(jpeg_decoder)
{
}

//===================================================================================
//===================================================================================
// Stops any active playback worker before the manager is destroyed.
AndroidPlaybackManager::~AndroidPlaybackManager()
{
    stopPlayback();
}

//===================================================================================
//===================================================================================
// Starts playback of the selected recording file on a background pacing thread.
bool AndroidPlaybackManager::startPlayback(const RecordingEntry& entry)
{
    LOGI("Starting playback name={} path={}", entry.name, entry.path);
    stopPlayback();

    m_speed_index.store(kPlaybackNormalSpeedIndex);
    PlaybackStatus status = {};
    status.active = true;
    status.speed_multiplier = speedMultiplier();
    setStatus(status);

    m_stop_requested.store(false);
    // Playback shares the Android decoder queue with live video. Clear pending camera
    // frames before submitting file frames.
    m_jpeg_decoder.clearPending();
    m_thread = std::thread([this, path = entry.path]()
    {
        runPlaybackFile(path);
    });
    return true;
}

//===================================================================================
//===================================================================================
// Requests playback stop and clears pending playback decode input.
void AndroidPlaybackManager::stopPlayback()
{
    m_stop_requested.store(true);
    if (m_thread.joinable())
    {
        m_thread.join();
    }

    PlaybackStatus status = {};
    setStatus(status);
    m_jpeg_decoder.clearPending();
}

//===================================================================================
//===================================================================================
// Submits one playback JPEG frame through the Android decoder queue.
void AndroidPlaybackManager::submitPlaybackFrame(const std::vector<uint8_t>& jpeg_frame, uint32_t current_frame)
{
    auto frame_buffer = std::make_shared<gs::core::VideoFrameAssembler::FrameBuffer>();
    frame_buffer->data = jpeg_frame;
    m_jpeg_decoder.submitJpeg(frame_buffer, current_frame);
}

//===================================================================================
//===================================================================================
// Reports playback failures through Android logging.
void AndroidPlaybackManager::logPlaybackFailure(const std::string& message)
{
    LOGW("Playback failed: {}", message);
}
