#pragma once

#include "android_video_renderer.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class AndroidJpegDecoder
{
public:
    explicit AndroidJpegDecoder(AndroidVideoRenderer& renderer);
    ~AndroidJpegDecoder();

    void submitJpeg(const uint8_t* jpeg_data, size_t jpeg_size);
    uint64_t submittedFrameCount() const;

private:
    void run();

    AndroidVideoRenderer& m_renderer;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    bool m_exit = false;

    std::vector<uint8_t> m_pending_jpeg;
    bool m_has_pending_jpeg = false;
    std::atomic<uint64_t> m_submitted_frames = 0;
};
