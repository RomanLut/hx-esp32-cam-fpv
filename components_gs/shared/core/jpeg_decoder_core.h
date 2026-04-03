#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace gs::core
{

class JpegDecoderCore
{
public:
    struct Config
    {
        size_t worker_count = 4;
        int pixel_format = 0;
        int bytes_per_pixel = 4;
        int tj_flags = 0;
    };

    struct DecodedFrame
    {
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        int stride = 0;
    };

    struct Stats
    {
        uint32_t broken_frames = 0;
        uint32_t input_submitted_count = 0;
        uint32_t dropped_input_count = 0;
        uint32_t decoded_count = 0;
        uint32_t decoded_total_ms = 0;
        uint32_t decoded_min_ms = 99;
        uint32_t decoded_max_ms = 0;
    };

    explicit JpegDecoderCore(const Config& config);
    ~JpegDecoderCore();

    void shutdown();
    void submitJpeg(const uint8_t* jpeg_data, size_t jpeg_size);
    bool waitPopLatestFrame(DecodedFrame& frame);
    Stats statsSnapshot() const;
    Stats consumeStats();

private:
    struct InputFrame
    {
        std::vector<uint8_t> jpeg;
    };

    void workerThreadProc();

    Config m_config;
    std::mutex m_input_mutex;
    std::condition_variable m_input_cv;
    std::deque<InputFrame> m_input_queue;

    std::mutex m_output_mutex;
    std::condition_variable m_output_cv;
    std::deque<DecodedFrame> m_output_queue;

    std::vector<std::thread> m_threads;
    std::atomic<bool> m_exit = false;

    std::atomic<uint32_t> m_broken_frames = 0;
    std::atomic<uint32_t> m_input_submitted_count = 0;
    std::atomic<uint32_t> m_dropped_input_count = 0;
    std::atomic<uint32_t> m_decoded_count = 0;
    std::atomic<uint32_t> m_decoded_total_ms = 0;
    std::atomic<uint32_t> m_decoded_min_ms = 99;
    std::atomic<uint32_t> m_decoded_max_ms = 0;
};

} // namespace gs::core
