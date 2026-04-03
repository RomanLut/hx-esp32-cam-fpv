#include "jpeg_decoder_core.h"

#include <algorithm>
#include <chrono>
#include <cstring>

extern "C"
{
#include <turbojpeg.h>
}

namespace gs::core
{

namespace
{

constexpr uint32_t kDefaultMinDecodeMs = 99;

}

JpegDecoderCore::JpegDecoderCore(const Config& config)
    : m_config(config)
{
    if (m_config.worker_count == 0)
    {
        m_config.worker_count = 1;
    }

    for (size_t index = 0; index < m_config.worker_count; ++index)
    {
        m_threads.emplace_back(&JpegDecoderCore::workerThreadProc, this);
    }
}

JpegDecoderCore::~JpegDecoderCore()
{
    shutdown();

    for (std::thread& thread : m_threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
}

void JpegDecoderCore::shutdown()
{
    m_exit.store(true);
    m_input_cv.notify_all();
    m_output_cv.notify_all();
}

void JpegDecoderCore::submitJpeg(const uint8_t* jpeg_data, size_t jpeg_size)
{
    if (jpeg_data == nullptr || jpeg_size == 0)
    {
        return;
    }

    InputFrame input;
    input.jpeg.assign(jpeg_data, jpeg_data + jpeg_size);

    {
        std::lock_guard<std::mutex> lock(m_input_mutex);
        m_input_submitted_count.fetch_add(1);
        if (!m_input_queue.empty())
        {
            m_dropped_input_count.fetch_add(static_cast<uint32_t>(m_input_queue.size()));
            m_input_queue.clear();
        }
        m_input_queue.push_back(std::move(input));
    }
    m_input_cv.notify_one();
}

bool JpegDecoderCore::waitPopLatestFrame(DecodedFrame& frame)
{
    std::unique_lock<std::mutex> lock(m_output_mutex);
    m_output_cv.wait(lock, [this] { return m_exit.load() || !m_output_queue.empty(); });
    if (m_output_queue.empty())
    {
        return false;
    }

    frame = std::move(m_output_queue.back());
    m_output_queue.clear();
    return true;
}

JpegDecoderCore::Stats JpegDecoderCore::statsSnapshot() const
{
    Stats stats;
    stats.broken_frames = m_broken_frames.load();
    stats.input_submitted_count = m_input_submitted_count.load();
    stats.dropped_input_count = m_dropped_input_count.load();
    stats.decoded_count = m_decoded_count.load();
    stats.decoded_total_ms = m_decoded_total_ms.load();
    stats.decoded_min_ms = m_decoded_min_ms.load();
    stats.decoded_max_ms = m_decoded_max_ms.load();
    return stats;
}

JpegDecoderCore::Stats JpegDecoderCore::consumeStats()
{
    Stats stats;
    stats.broken_frames = m_broken_frames.exchange(0);
    stats.input_submitted_count = m_input_submitted_count.exchange(0);
    stats.dropped_input_count = m_dropped_input_count.exchange(0);
    stats.decoded_count = m_decoded_count.exchange(0);
    stats.decoded_total_ms = m_decoded_total_ms.exchange(0);
    stats.decoded_min_ms = m_decoded_min_ms.exchange(kDefaultMinDecodeMs);
    stats.decoded_max_ms = m_decoded_max_ms.exchange(0);
    if (stats.decoded_count == 0)
    {
        stats.decoded_min_ms = kDefaultMinDecodeMs;
    }
    return stats;
}

void JpegDecoderCore::workerThreadProc()
{
    tjhandle tj_instance = tjInitDecompress();
    if (tj_instance == nullptr)
    {
        m_broken_frames.fetch_add(1);
        return;
    }

    InputFrame input;
    DecodedFrame frame;

    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(m_input_mutex);
            m_input_cv.wait(lock, [this] { return m_exit.load() || !m_input_queue.empty(); });
            if (m_exit.load())
            {
                break;
            }
            input = std::move(m_input_queue.back());
            m_input_queue.clear();
        }

        const auto decode_begin = std::chrono::steady_clock::now();
        int width = 0;
        int height = 0;
        int subsamp = 0;
        int colorspace = 0;
        if (tjDecompressHeader3(tj_instance,
                                input.jpeg.data(),
                                static_cast<unsigned long>(input.jpeg.size()),
                                &width,
                                &height,
                                &subsamp,
                                &colorspace) != 0)
        {
            m_broken_frames.fetch_add(1);
            continue;
        }

        if (width <= 0 || height <= 0)
        {
            m_broken_frames.fetch_add(1);
            continue;
        }

        frame.width = width;
        frame.height = height;
        frame.stride = width * m_config.bytes_per_pixel;
        frame.pixels.resize(static_cast<size_t>(frame.stride) * static_cast<size_t>(frame.height));

        if (tjDecompress2(tj_instance,
                          input.jpeg.data(),
                          static_cast<unsigned long>(input.jpeg.size()),
                          frame.pixels.data(),
                          frame.width,
                          frame.stride,
                          frame.height,
                          m_config.pixel_format,
                          m_config.tj_flags) != 0)
        {
            m_broken_frames.fetch_add(1);
            continue;
        }

        const uint32_t duration_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - decode_begin).count());
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

        {
            std::lock_guard<std::mutex> lock(m_output_mutex);
            m_output_queue.push_back(std::move(frame));
        }
        m_output_cv.notify_one();
    }

    tjDestroy(tj_instance);
}

} // namespace gs::core
