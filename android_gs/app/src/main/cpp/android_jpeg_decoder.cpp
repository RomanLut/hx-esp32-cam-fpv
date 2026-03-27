#include "android_jpeg_decoder.h"

#include <turbojpeg.h>

AndroidJpegDecoder::AndroidJpegDecoder(AndroidVideoRenderer& renderer)
    : m_renderer(renderer),
      m_core([&renderer] {
          gs::core::JpegDecoderCore::Config config;
          config.worker_count = 2;
          config.pixel_format = TJPF_RGBA;
          config.bytes_per_pixel = 4;
          config.tj_flags = TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE;
          return config;
      }()),
      m_output_thread(&AndroidJpegDecoder::outputThreadProc, this)
{
}

AndroidJpegDecoder::~AndroidJpegDecoder()
{
    m_core.shutdown();
    if (m_output_thread.joinable())
    {
        m_output_thread.join();
    }
}

void AndroidJpegDecoder::submitJpeg(const uint8_t* jpeg_data, size_t jpeg_size)
{
    m_core.submitJpeg(jpeg_data, jpeg_size);
}

uint64_t AndroidJpegDecoder::submittedFrameCount() const
{
    return m_submitted_frames.load();
}

AndroidJpegDecoder::DecodeStats AndroidJpegDecoder::statsSnapshot() const
{
    const gs::core::JpegDecoderCore::Stats core_stats = m_core.statsSnapshot();
    DecodeStats stats;
    stats.broken_frames = core_stats.broken_frames;
    stats.input_submitted_count = core_stats.input_submitted_count;
    stats.overwritten_pending_count = core_stats.dropped_input_count;
    stats.decoded_count = core_stats.decoded_count;
    stats.decoded_total_ms = core_stats.decoded_total_ms;
    stats.decoded_min_ms = core_stats.decoded_min_ms;
    stats.decoded_max_ms = core_stats.decoded_max_ms;
    return stats;
}

void AndroidJpegDecoder::outputThreadProc()
{
    gs::core::JpegDecoderCore::DecodedFrame frame;
    while (m_core.waitPopLatestFrame(frame))
    {
        m_renderer.submitFrame(std::move(frame.pixels), frame.width, frame.height, frame.stride);
        m_submitted_frames.fetch_add(1);
    }
}

AndroidJpegDecoder::DecodeStats AndroidJpegDecoder::consumeStats()
{
    const gs::core::JpegDecoderCore::Stats core_stats = m_core.consumeStats();
    DecodeStats stats;
    stats.broken_frames = core_stats.broken_frames;
    stats.input_submitted_count = core_stats.input_submitted_count;
    stats.overwritten_pending_count = core_stats.dropped_input_count;
    stats.decoded_count = core_stats.decoded_count;
    stats.decoded_total_ms = core_stats.decoded_total_ms;
    stats.decoded_min_ms = core_stats.decoded_min_ms;
    stats.decoded_max_ms = core_stats.decoded_max_ms;
    return stats;
}
