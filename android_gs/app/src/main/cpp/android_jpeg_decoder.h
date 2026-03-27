#pragma once

#include "android_video_renderer.h"
#include "core/jpeg_decoder_core.h"

#include <atomic>
#include <cstdint>
#include <thread>

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
    void outputThreadProc();

    AndroidVideoRenderer& m_renderer;
    gs::core::JpegDecoderCore m_core;
    std::thread m_output_thread;
    std::atomic<uint64_t> m_submitted_frames = 0;
};
