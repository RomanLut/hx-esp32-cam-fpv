#pragma once

#include "android_jni_shared.h"
#include "gs_video_renderer.h"
#include "core/video_frame_assembler.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

//===================================================================================
//===================================================================================
// Decodes JPEG frames received from the video assembler using the Android Bitmap API
// via JNI, running a single worker thread and submitting decoded frames to the
// video renderer in the pipeline format selected from GS postprocessing settings.
class AndroidBitmapJpegDecoder
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

    explicit AndroidBitmapJpegDecoder(GsVideoRenderer& renderer);
    ~AndroidBitmapJpegDecoder();

    void submitJpeg(gs::core::VideoFrameAssembler::FrameBufferPtr jpeg_buffer, uint32_t frame_id);
    void clearPending();
    uint64_t submittedFrameCount() const;
    DecodeStats statsSnapshot() const;
    DecodeStats consumeStats();

private:
    struct InputFrame
    {
        gs::core::VideoFrameAssembler::FrameBufferPtr jpeg_buffer;
        uint32_t frame_id = 0;
    };

    void workerThreadProc();

    GsVideoRenderer& m_renderer;
    std::mutex m_input_mutex;
    std::condition_variable m_input_cv;
    std::deque<InputFrame> m_input_queue;
    std::thread m_thread;
    std::atomic<bool> m_exit = false;
    std::atomic<uint64_t> m_submitted_frames = 0;
    std::atomic<uint32_t> m_broken_frames = 0;
    std::atomic<uint32_t> m_input_submitted_count = 0;
    std::atomic<uint32_t> m_dropped_input_count = 0;
    std::atomic<uint32_t> m_decoded_count = 0;
    std::atomic<uint32_t> m_decoded_total_ms = 0;
    std::atomic<uint32_t> m_decoded_min_ms = 99;
    std::atomic<uint32_t> m_decoded_max_ms = 0;
};
