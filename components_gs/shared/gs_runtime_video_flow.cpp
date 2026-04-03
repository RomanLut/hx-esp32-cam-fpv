#include "gs_runtime_video_flow.h"

CompletedVideoFrameView getCompletedVideoFrameView(const ProcessedVideoEvent& event)
{
    CompletedVideoFrameView view;
    if (!event.frame_result.completedFrame || !event.frame_result.frameData)
    {
        return view;
    }

    view.has_frame = true;
    view.frame_index = event.frame_result.completedFrameIndex;
    view.frame_data = event.frame_result.frameData;
    view.data = view.frame_data->data.empty() ? nullptr : view.frame_data->data.data();
    view.size = view.frame_data->data.size();
    return view;
}

VideoDispatchDecision buildVideoDispatchDecision(const ProcessedVideoEvent& event, bool restored_by_fec)
{
    VideoDispatchDecision decision;
    decision.restored_video_part = restored_by_fec;
    decision.completed_frame = getCompletedVideoFrameView(event);
    return decision;
}

ProcessedVideoEvent processVideoSessionEvent(const gs::core::SessionEvent& event,
                                            gs::core::VideoFrameAssembler& assembler,
                                            gs::core::GsSessionCore& session,
                                            bool restored_by_fec,
                                            Clock::time_point now)
{
    ProcessedVideoEvent result;
    if (event.kind != gs::core::SessionEventKind::VideoPacket || event.video.packet == nullptr)
    {
        return result;
    }

    result.frame_index = event.video.packet->frame_index;
    result.part_index = event.video.packet->part_index;
    result.last_part = event.video.packet->last_part;
    result.payload_size = event.video.payload_size;

    result.frame_result = assembler.pushPacket(*event.video.packet,
                                               event.video.payload,
                                               event.video.payload_size,
                                               restored_by_fec);
    const AirStats& air_stats = session.lastAirStats();
    const uint8_t queue_usage = air_stats.wifi_queue_max;
    const uint8_t quality = air_stats.curr_quality;

    if (result.frame_result.lostPartialFrame)
    {
        session.onLostPartialFrame(result.frame_result.lostPartialParts, queue_usage);
    }

    if (result.frame_result.lostWholeFrames > 0)
    {
        session.onLostWholeFrames(result.frame_result.lostWholeFrames);
    }

    if (result.frame_result.completedFrame)
    {
        session.onCompletedFrame(result.frame_result.completedRestoredByFec,
                                 result.frame_result.completedPartIndex,
                                 quality,
                                 queue_usage,
                                 now);
    }

    return result;
}
