#include "android_jpeg_decoder.h"

#include <android/log.h>
#include <turbojpeg.h>

#include <algorithm>
#include <chrono>

namespace
{
constexpr const char* kLogTag = "AndroidGSJpeg";
}

AndroidJpegDecoder::AndroidJpegDecoder(AndroidVideoRenderer& renderer)
    : m_renderer(renderer), m_thread(&AndroidJpegDecoder::run, this)
{
}

AndroidJpegDecoder::~AndroidJpegDecoder()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_exit = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void AndroidJpegDecoder::submitJpeg(const uint8_t* jpeg_data, size_t jpeg_size)
{
    if (jpeg_data == nullptr || jpeg_size == 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending_jpeg.assign(jpeg_data, jpeg_data + jpeg_size);
    m_has_pending_jpeg = true;
    m_cv.notify_all();
}

uint64_t AndroidJpegDecoder::submittedFrameCount() const
{
    return m_submitted_frames.load();
}

AndroidJpegDecoder::DecodeStats AndroidJpegDecoder::statsSnapshot() const
{
    DecodeStats stats;
    stats.broken_frames = m_broken_frames.load();
    stats.decoded_count = m_decoded_count.load();
    stats.decoded_total_ms = m_decoded_total_ms.load();
    stats.decoded_min_ms = m_decoded_min_ms.load();
    stats.decoded_max_ms = m_decoded_max_ms.load();
    return stats;
}

void AndroidJpegDecoder::run()
{
    tjhandle tj_instance = tjInitDecompress();
    if (tj_instance == nullptr)
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "tjInitDecompress failed: %s", tjGetErrorStr());
        return;
    }

    std::vector<uint8_t> jpeg_data;
    std::vector<uint8_t> rgba;

    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_exit || m_has_pending_jpeg; });
            if (m_exit)
            {
                break;
            }
            jpeg_data.swap(m_pending_jpeg);
            m_pending_jpeg.clear();
            m_has_pending_jpeg = false;
        }

        const auto decode_begin = std::chrono::steady_clock::now();
        int width = 0;
        int height = 0;
        int subsamp = 0;
        int colorspace = 0;
        if (tjDecompressHeader3(tj_instance,
                                jpeg_data.data(),
                                static_cast<unsigned long>(jpeg_data.size()),
                                &width,
                                &height,
                                &subsamp,
                                &colorspace) != 0)
        {
            __android_log_print(ANDROID_LOG_WARN, kLogTag, "tjDecompressHeader3 failed: %s", tjGetErrorStr());
            m_broken_frames.fetch_add(1);
            continue;
        }
        if (width <= 0 || height <= 0)
        {
            m_broken_frames.fetch_add(1);
            continue;
        }

        const int stride = width * 4;
        const size_t required_size = static_cast<size_t>(stride) * static_cast<size_t>(height);
        rgba.resize(required_size);

        if (tjDecompress2(tj_instance,
                          jpeg_data.data(),
                          static_cast<unsigned long>(jpeg_data.size()),
                          rgba.data(),
                          width,
                          stride,
                          height,
                          TJPF_RGBA,
                          TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE) != 0)
        {
            __android_log_print(ANDROID_LOG_WARN, kLogTag, "tjDecompress2 failed: %s", tjGetErrorStr());
            m_broken_frames.fetch_add(1);
            continue;
        }

        const uint32_t duration_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - decode_begin).count());
        m_renderer.submitFrame(rgba.data(), rgba.size(), width, height, stride);
        m_submitted_frames.fetch_add(1);
        m_decoded_count.fetch_add(1);
        m_decoded_total_ms.fetch_add(duration_ms);
        uint32_t current_min = m_decoded_min_ms.load();
        while (duration_ms < current_min && !m_decoded_min_ms.compare_exchange_weak(current_min, duration_ms))
        {
        }
        uint32_t current_max = m_decoded_max_ms.load();
        while (duration_ms > current_max && !m_decoded_max_ms.compare_exchange_weak(current_max, duration_ms))
        {
        }
    }

    tjDestroy(tj_instance);
}
