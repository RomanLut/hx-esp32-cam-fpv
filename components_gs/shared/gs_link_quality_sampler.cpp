#include "gs_link_quality_sampler.h"

#include <algorithm>
#include <chrono>

#include "gs_top_overlay_shared.h"

namespace gs::imgui
{
namespace
{
constexpr Clock::duration kLinkQualitySampleDuration = std::chrono::milliseconds(300);
constexpr Clock::duration kLinkQualityStaleDuration = std::chrono::seconds(2);
}

//===================================================================================
//===================================================================================
// Updates the video and RC link-quality samples from the latest runtime data.
void LinkQualitySampler::update(const TopOverlayData& input)
{
    updateVideoQuality(input);
    updateRcQuality(input);
}

//===================================================================================
//===================================================================================
// Returns the most recently sampled video link quality.
float LinkQualitySampler::videoQuality() const
{
    return m_video_quality;
}

//===================================================================================
//===================================================================================
// Returns the most recently sampled RC link quality.
float LinkQualitySampler::rcQuality() const
{
    return m_rc_quality;
}

//===================================================================================
//===================================================================================
// Samples received video packets over completed FEC blocks.
void LinkQualitySampler::updateVideoQuality(const TopOverlayData& input)
{
    const uint8_t fec_k = input.video_fec_k > 0 ? input.video_fec_k : static_cast<uint8_t>(FEC_K);
    const uint8_t fec_n = input.video_fec_n >= fec_k ? input.video_fec_n : static_cast<uint8_t>(FEC_N);

    if (!m_video_initialized || fec_k != m_video_fec_k || fec_n != m_video_fec_n)
    {
        resetVideoWindow(input, fec_k, fec_n);
        m_video_initialized = true;
        return;
    }

    // The periodic GS stats rollover can briefly expose zeros before the decoder
    // republishes its cumulative counters. Ignoring that transient avoids treating
    // it as a stream restart or as 65536 newly received packets.
    if (input.video_received_packet_count == 0 &&
        input.video_last_packet_index == 0 &&
        m_last_observed_received > 0)
    {
        if (input.now - m_last_video_data_tp >= kLinkQualityStaleDuration)
        {
            m_video_quality = 0.0f;
        }
        return;
    }

    const uint32_t current_block_index = input.video_last_packet_index / fec_n;
    if (current_block_index + 64U < m_max_video_block_index)
    {
        // A large FEC block-number back-jump is a new stream using the same FEC
        // settings, so its counters must not be compared with the previous stream.
        resetVideoWindow(input, fec_k, fec_n);
        m_video_quality = 0.0f;
        return;
    }

    m_max_video_block_index = std::max(m_max_video_block_index, current_block_index);
    if (input.video_received_packet_count != m_last_observed_received)
    {
        // The decoder increments this unique counter before rejecting packets for
        // an already-ready block, so late packets dropped there remain included.
        m_last_observed_received = input.video_received_packet_count;
        m_last_video_data_tp = input.now;
    }

    if (input.now - m_last_video_data_tp >= kLinkQualityStaleDuration)
    {
        m_video_quality = 0.0f;
    }

    if (input.now - m_last_video_sample_tp < kLinkQualitySampleDuration)
    {
        return;
    }

    const uint32_t completed_blocks = m_max_video_block_index - m_window_start_block_index;
    if (completed_blocks == 0)
    {
        return;
    }

    const uint32_t expected_packets = completed_blocks * static_cast<uint32_t>(fec_n);
    const uint16_t received_packets = static_cast<uint16_t>(
        input.video_received_packet_count - m_window_start_received);
    m_video_quality = expected_packets > 0
        ? std::clamp(static_cast<float>(received_packets) / static_cast<float>(expected_packets), 0.0f, 1.0f)
        : 0.0f;
    m_window_start_received = input.video_received_packet_count;
    m_window_start_block_index = m_max_video_block_index;
    m_last_video_sample_tp = input.now;
}

//===================================================================================
//===================================================================================
// Samples ground-to-air packet delivery for the RC link-quality gauge.
void LinkQualitySampler::updateRcQuality(const TopOverlayData& input)
{
    if (!input.air_stats_valid)
    {
        m_rc_quality = 0.0f;
        return;
    }
    if (input.now - m_last_rc_sample_tp < kLinkQualitySampleDuration)
    {
        return;
    }

    // Match the AirPacketLossRatio stats calculation. The shared gauge renderer
    // squares this delivery ratio, yielding (100 - AirPacketLossRatio)^2.
    if (input.gs_out_packet_rate == 0)
    {
        m_rc_quality = 1.0f;
    }
    else
    {
        const int lost_packets = std::max(
            0,
            static_cast<int>(input.gs_out_packet_rate) - static_cast<int>(input.air_in_packet_rate));
        const float loss_ratio = static_cast<float>(lost_packets) /
            static_cast<float>(input.gs_out_packet_rate);
        m_rc_quality = std::clamp(1.0f - loss_ratio, 0.0f, 1.0f);
    }
    m_last_rc_sample_tp = input.now;
}

//===================================================================================
//===================================================================================
// Resets video sampling when FEC settings or the incoming stream changes.
void LinkQualitySampler::resetVideoWindow(const TopOverlayData& input, uint8_t fec_k, uint8_t fec_n)
{
    m_window_start_received = input.video_received_packet_count;
    m_last_observed_received = input.video_received_packet_count;
    m_window_start_block_index = input.video_last_packet_index / fec_n;
    m_max_video_block_index = m_window_start_block_index;
    m_video_fec_k = fec_k;
    m_video_fec_n = fec_n;
    m_last_video_sample_tp = input.now;
    m_last_video_data_tp = input.now;
}
}
