#include "gs_linux_playback_manager.h"

#include <memory>
#include <thread>

#include "Log.h"
#include "Video_Decoder.h"
#include "main.h"

//===================================================================================
//===================================================================================
// Constructs an idle Linux playback manager.
LinuxPlaybackManager::LinuxPlaybackManager() = default;

//===================================================================================
//===================================================================================
// Stops any active playback worker before the manager is destroyed.
LinuxPlaybackManager::~LinuxPlaybackManager()
{
    stopPlayback();
}

//===================================================================================
//===================================================================================
// Starts playback of the selected recording file on a background pacing thread.
bool LinuxPlaybackManager::startPlayback(const RecordingEntry& entry)
{
    stopPlayback();

    m_speed_index.store(kPlaybackNormalSpeedIndex);
    PlaybackStatus status = {};
    status.active = true;
    status.speed_multiplier = speedMultiplier();
    setStatus(status);

    m_stop_requested.store(false);
    // Playback shares the live decoder, so queued camera frames must be dropped before
    // the file frames are submitted. Receive and recording continue on the comms thread.
    s_decoder.invalidate_displayed_frame();
    m_thread = std::thread([this, path = entry.path]()
    {
        runPlaybackFile(path);
    });
    return true;
}

//===================================================================================
//===================================================================================
// Requests playback stop and waits until the worker has left the decoder path.
void LinuxPlaybackManager::stopPlayback()
{
    m_stop_requested.store(true);
    if (m_thread.joinable())
    {
        m_thread.join();
    }

    PlaybackStatus status = {};
    setStatus(status);
    s_decoder.invalidate_displayed_frame();
}

//===================================================================================
//===================================================================================
// Submits one playback JPEG frame through the normal Linux decoder path.
void LinuxPlaybackManager::submitPlaybackFrame(const std::vector<uint8_t>& jpeg_frame, uint32_t current_frame)
{
    auto frame_buffer = std::make_shared<gs::core::VideoFrameAssembler::FrameBuffer>();
    frame_buffer->data = jpeg_frame;
    s_decoder.decode_data(frame_buffer, current_frame);
}

//===================================================================================
//===================================================================================
// Reports playback failures through the shared Linux GS logger.
void LinuxPlaybackManager::logPlaybackFailure(const std::string& message)
{
    LOGW("Playback failed: {}", message);
}

//===================================================================================
//===================================================================================
// Returns the process-wide Linux playback manager instance.
LinuxPlaybackManager& getLinuxPlaybackManager()
{
    static LinuxPlaybackManager manager;
    return manager;
}
