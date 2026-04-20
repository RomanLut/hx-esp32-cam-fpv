#include "gs_playback_manager.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#include "avi.h"

namespace
{

constexpr size_t kAviHeaderFpsOffset = 0x84;
constexpr size_t kAviHeaderFrameCountOffset = 0x30;

}

PlaybackManager* s_playbackManager = nullptr;

//===================================================================================
//===================================================================================
// Closes a C FILE handle owned by a unique_ptr.
void PlaybackManager::FileCloser::operator()(FILE* file) const
{
    if (file != nullptr)
    {
        std::fclose(file);
    }
}

//===================================================================================
//===================================================================================
// Moves playback speed one step toward slower/reverse playback.
void PlaybackManager::decreaseSpeed()
{
    adjustSpeed(-1);
}

//===================================================================================
//===================================================================================
// Moves playback speed one step toward faster/forward playback.
void PlaybackManager::increaseSpeed()
{
    adjustSpeed(1);
}

//===================================================================================
//===================================================================================
// Toggles between paused 0x playback and forward 1x playback.
void PlaybackManager::togglePaused()
{
    const int current_speed = speedMultiplier();
    m_speed_index.store(current_speed == 0 ? kPlaybackNormalSpeedIndex : kPlaybackStoppedSpeedIndex);
    PlaybackStatus status = this->status();
    status.speed_multiplier = speedMultiplier();
    if (status.active && !status.broken)
    {
        status.message = std::to_string(status.speed_multiplier) + "x " +
                         formatPlaybackTime(status.current_ms) + " / " + formatPlaybackTime(status.total_ms);
    }
    setStatus(status);
    onPlaybackStateChanged();
}

//===================================================================================
//===================================================================================
// Returns a snapshot of the current playback progress.
PlaybackStatus PlaybackManager::status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

//===================================================================================
//===================================================================================
// Parses the fixed GS AVI header and positions the file at the first MJPEG chunk.
bool PlaybackManager::readAviHeader(FILE* file, uint32_t& fps, uint32_t& total_frames)
{
    std::array<uint8_t, AVI_HEADER_LEN> header = {};
    if (std::fread(header.data(), 1, header.size(), file) != header.size())
    {
        return false;
    }

    if (std::memcmp(header.data(), "RIFF", 4) != 0 ||
        std::memcmp(header.data() + 8, "AVI ", 4) != 0 ||
        std::memcmp(header.data() + AVI_HEADER_LEN - 4, "movi", 4) != 0)
    {
        return false;
    }

    fps = readAviLe32(header.data() + kAviHeaderFpsOffset);
    if (fps == 0)
    {
        fps = header[kAviHeaderFpsOffset];
    }
    total_frames = readAviLe32(header.data() + kAviHeaderFrameCountOffset);
    if (total_frames == 0)
    {
        total_frames = readAviLe16(header.data() + kAviHeaderFrameCountOffset);
    }

    return total_frames > 0;
}

//===================================================================================
//===================================================================================
// Builds a random-access frame index for speed changes and reverse playback.
bool PlaybackManager::buildAviFrameIndex(FILE* file, uint32_t total_frames, std::vector<AviFrameIndex>& frame_index)
{
    frame_index.clear();
    frame_index.reserve(total_frames);

    for (uint32_t index = 0; index < total_frames; ++index)
    {
        const long chunk_offset = std::ftell(file);
        std::array<uint8_t, CHUNK_HDR> chunk_header = {};
        if (std::fread(chunk_header.data(), 1, chunk_header.size(), file) != chunk_header.size())
        {
            return false;
        }

        if (std::memcmp(chunk_header.data(), dcBuf, 4) != 0)
        {
            return false;
        }

        const uint32_t chunk_size = readAviLe32(chunk_header.data() + 4);
        if (chunk_size == 0 || chunk_size > 1024U * 1024U)
        {
            return false;
        }

        frame_index.push_back({chunk_offset, chunk_size});
        if (std::fseek(file, static_cast<long>(chunk_size), SEEK_CUR) != 0)
        {
            return false;
        }
    }

    return !frame_index.empty();
}

//===================================================================================
//===================================================================================
// Reads one padded 00dc MJPEG chunk from an indexed AVI frame position.
bool PlaybackManager::readAviFrameAt(FILE* file, const AviFrameIndex& frame_index, std::vector<uint8_t>& jpeg_frame)
{
    if (std::fseek(file, frame_index.chunk_offset, SEEK_SET) != 0)
    {
        return false;
    }

    std::array<uint8_t, CHUNK_HDR> chunk_header = {};
    if (std::fread(chunk_header.data(), 1, chunk_header.size(), file) != chunk_header.size())
    {
        return false;
    }

    if (std::memcmp(chunk_header.data(), dcBuf, 4) != 0)
    {
        return false;
    }

    uint32_t chunk_size = 0;
    std::memcpy(&chunk_size, chunk_header.data() + 4, sizeof(chunk_size));
    if (chunk_size == 0 || chunk_size != frame_index.chunk_size || chunk_size > 1024U * 1024U)
    {
        return false;
    }

    jpeg_frame.resize(chunk_size);
    if (std::fread(jpeg_frame.data(), 1, jpeg_frame.size(), file) != jpeg_frame.size())
    {
        return false;
    }

    while (!jpeg_frame.empty() && jpeg_frame.back() == 0)
    {
        jpeg_frame.pop_back();
    }
    return jpeg_frame.size() >= 4 &&
           jpeg_frame[0] == 0xFF &&
           jpeg_frame[1] == 0xD8;
}

//===================================================================================
//===================================================================================
// Reads AVI MJPEG frames and submits them to the platform decoder at the file FPS.
void PlaybackManager::runPlaybackFile(std::string path)
{
    std::unique_ptr<FILE, FileCloser> file(std::fopen(path.c_str(), "rb"));
    if (!file)
    {
        markBroken("open_failed");
        return;
    }

    uint32_t fps = 0;
    uint32_t total_frames = 0;
    if (!readAviHeader(file.get(), fps, total_frames))
    {
        markBroken("bad_header");
        return;
    }

    fps = std::clamp(fps == 0 ? kDefaultPlaybackFps : fps, kMinPlaybackFps, kMaxPlaybackFps);
    const uint32_t frame_ms = std::max<uint32_t>(1U, 1000U / fps);
    const auto frame_interval = std::chrono::microseconds(1000000U / fps);
    const uint32_t total_ms = total_frames * frame_ms;
    auto next_frame_tp = std::chrono::steady_clock::now();

    std::vector<AviFrameIndex> frame_index;
    if (!buildAviFrameIndex(file.get(), total_frames, frame_index))
    {
        markBroken("bad_index");
        return;
    }

    std::vector<uint8_t> jpeg_frame;
    int64_t frame_position = 0;
    while (!m_stop_requested.load() && frame_position >= 0 && frame_position < static_cast<int64_t>(frame_index.size()))
    {
        const int speed = speedMultiplier();
        const uint32_t current_frame = static_cast<uint32_t>(frame_position);
        if (!readAviFrameAt(file.get(), frame_index[current_frame], jpeg_frame))
        {
            markBroken("bad_frame");
            return;
        }

        submitPlaybackFrame(jpeg_frame, current_frame);

        PlaybackStatus status = {};
        status.active = true;
        status.current_frame = current_frame + 1U;
        status.total_frames = total_frames;
        status.current_ms = std::min(total_ms, (current_frame + 1U) * frame_ms);
        status.total_ms = total_ms;
        status.speed_multiplier = speed;
        int64_t next_frame_position = frame_position + speed;
        int visible_speed = speed;
        if (speed < 0 && next_frame_position < 0)
        {
            next_frame_position = 0;
            visible_speed = 0;
            m_speed_index.store(kPlaybackStoppedSpeedIndex);
        }
        else if (speed > 0 && next_frame_position >= static_cast<int64_t>(frame_index.size()))
        {
            next_frame_position = static_cast<int64_t>(frame_index.size()) - 1;
            visible_speed = 0;
            m_speed_index.store(kPlaybackStoppedSpeedIndex);
        }

        status.speed_multiplier = visible_speed;
        status.message = std::to_string(visible_speed) + "x " +
                         formatPlaybackTime(status.current_ms) + " / " + formatPlaybackTime(status.total_ms);
        setStatus(status);

        frame_position = next_frame_position;
        next_frame_tp += frame_interval;
        std::this_thread::sleep_until(next_frame_tp);
    }

    onPlaybackFinished();
}

//===================================================================================
//===================================================================================
// Returns the currently selected playback speed multiplier.
int PlaybackManager::speedMultiplier() const
{
    const int index = std::clamp(m_speed_index.load(), 0, static_cast<int>(kPlaybackSpeeds.size()) - 1);
    return kPlaybackSpeeds[index];
}

//===================================================================================
//===================================================================================
// Stores a playback status snapshot under lock.
void PlaybackManager::setStatus(const PlaybackStatus& status)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status = status;
}

//===================================================================================
//===================================================================================
// Marks the current playback as active but broken so the overlay shows the error text.
void PlaybackManager::markBroken(const std::string& message)
{
    PlaybackStatus status = {};
    status.active = true;
    status.broken = true;
    status.speed_multiplier = speedMultiplier();
    status.message = message;
    setStatus(status);
    onPlaybackStateChanged();
    logPlaybackFailure(message);
}

//===================================================================================
//===================================================================================
// Formats milliseconds as HH:MM:SS for the playback progress label.
std::string PlaybackManager::formatPlaybackTime(uint32_t milliseconds) const
{
    const uint32_t total_seconds = milliseconds / 1000U;
    const uint32_t hours = total_seconds / 3600U;
    const uint32_t minutes = (total_seconds / 60U) % 60U;
    const uint32_t seconds = total_seconds % 60U;

    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u", hours, minutes, seconds);
    return buffer;
}

//===================================================================================
//===================================================================================
// Adjusts playback speed within the supported reverse/forward speed table.
void PlaybackManager::adjustSpeed(int delta)
{
    int current = m_speed_index.load();
    while (true)
    {
        const int next = std::clamp(current + delta, 0, static_cast<int>(kPlaybackSpeeds.size()) - 1);
        if (m_speed_index.compare_exchange_weak(current, next))
        {
            PlaybackStatus status = this->status();
            status.speed_multiplier = kPlaybackSpeeds[next];
            if (status.active && !status.broken)
            {
                status.message = std::to_string(status.speed_multiplier) + "x " +
                                 formatPlaybackTime(status.current_ms) + " / " + formatPlaybackTime(status.total_ms);
            }
            setStatus(status);
            onPlaybackStateChanged();
            return;
        }
    }
}

//===================================================================================
//===================================================================================
// Lets platform managers react after the playback worker exits normally or by stop.
void PlaybackManager::onPlaybackFinished()
{
}

//===================================================================================
//===================================================================================
// Lets platform managers react after user-visible playback state changes.
void PlaybackManager::onPlaybackStateChanged()
{
}

//===================================================================================
//===================================================================================
// Lets platform managers report playback failures with platform-specific logging.
void PlaybackManager::logPlaybackFailure(const std::string& message)
{
    (void)message;
}
