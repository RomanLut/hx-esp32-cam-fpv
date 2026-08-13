#pragma once

#include <cstdint>

#include "../../components/common/Clock.h"
#include "packets.h"

namespace gs::imgui
{
struct TopOverlayData;

//===================================================================================
//===================================================================================
// Samples GS FEC progress and ground-to-air packet delivery for link-quality gauges.
class LinkQualitySampler
{
public:
    void update(const TopOverlayData& input);
    float videoQuality() const;
    float rcQuality() const;

private:
    void updateVideoQuality(const TopOverlayData& input);
    void updateRcQuality(const TopOverlayData& input);
    void resetVideoWindow(const TopOverlayData& input, uint8_t fec_k, uint8_t fec_n);

    bool m_video_initialized = false;
    uint8_t m_video_fec_k = FEC_K;
    uint8_t m_video_fec_n = FEC_N;
    uint32_t m_window_start_received = 0;
    uint32_t m_last_observed_received = 0;
    uint32_t m_window_start_block_index = 0;
    uint32_t m_max_video_block_index = 0;
    Clock::time_point m_last_video_sample_tp = {};
    Clock::time_point m_last_video_data_tp = {};
    Clock::time_point m_last_rc_sample_tp = {};
    float m_video_quality = 0.0f;
    float m_rc_quality = 0.0f;
};
}
