#pragma once

#include <cstddef>
#include <cstdint>

#include "../../components/common/Clock.h"
#include "core/gs_session_core.h"
#include "core/video_frame_assembler.h"

struct ProcessedVideoEvent
{
    gs::core::VideoFrameAssembler::Result frame_result = {};
    uint32_t frame_index = 0;
    uint8_t part_index = 0;
    uint8_t last_part = 0;
    size_t payload_size = 0;
};

struct CompletedVideoFrameView
{
    bool has_frame = false;
    uint32_t frame_index = 0;
    gs::core::VideoFrameAssembler::FrameBufferPtr frame_data = {};
    uint8_t* data = nullptr;
    size_t size = 0;
};

struct VideoDispatchDecision
{
    bool restored_video_part = false;
    CompletedVideoFrameView completed_frame = {};
};

CompletedVideoFrameView getCompletedVideoFrameView(const ProcessedVideoEvent& event);
VideoDispatchDecision buildVideoDispatchDecision(const ProcessedVideoEvent& event, bool restored_by_fec);

ProcessedVideoEvent processVideoSessionEvent(const gs::core::SessionEvent& event,
                                            gs::core::VideoFrameAssembler& assembler,
                                            gs::core::GsSessionCore& session,
                                            bool restored_by_fec,
                                            Clock::time_point now);
