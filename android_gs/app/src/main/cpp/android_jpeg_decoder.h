#pragma once

#include "android_video_renderer.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include <limits>

class AndroidJpegDecoder
{
public:
    struct DecodeStats
    {
        uint32_t broken_frames = 0;
        uint32_t input_submitted_count = 0;
        uint32_t overwritten_pending_count = 0;
        uint32_t decoded_count = 0;
        uint32_t decoded_total_ms = 0;
        uint32_t decoded_min_ms = 99;
        uint32_t decoded_max_ms = 0;
    };

    explicit AndroidJpegDecoder(AndroidVideoRenderer& renderer);
    ~AndroidJpegDecoder();

    void submitJpeg(const uint8_t* jpeg_data, size_t jpeg_size);
    uint64_t submittedFrameCount() const;
    DecodeStats statsSnapshot() const;
    DecodeStats consumeStats();

private:
    void run();

    AndroidVideoRenderer& m_renderer;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    bool m_exit = false;

    std::vector<uint8_t> m_next_jpeg;
    bool m_has_next_jpeg = false;
    std::atomic<uint32_t> m_input_submitted_count = 0;
    std::atomic<uint32_t> m_overwritten_pending_count = 0;
    std::atomic<uint64_t> m_submitted_frames = 0;
    std::atomic<uint32_t> m_broken_frames = 0;
    std::atomic<uint32_t> m_decoded_count = 0;
    std::atomic<uint32_t> m_decoded_total_ms = 0;
    std::atomic<uint32_t> m_decoded_min_ms = 99;
    std::atomic<uint32_t> m_decoded_max_ms = 0;
};
