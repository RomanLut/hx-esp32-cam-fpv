#include "core/video_frame_assembler.h"

#include <cstring>

namespace gs::core
{

//===================================================================================
//===================================================================================
// Initializes the frame buffer pool with the given number of pre-allocated buffers.
VideoFrameAssembler::FrameBufferPoolState::FrameBufferPoolState(size_t count)
{
    free_buffers.reserve(count);
    for (size_t index = 0; index < count; ++index)
    {
        free_buffers.emplace_back(std::make_unique<FrameBuffer>());
    }
}

//===================================================================================
//===================================================================================
// Constructor. Creates the shared frame buffer pool with 3 buffers.
VideoFrameAssembler::VideoFrameAssembler()
    : m_frameBufferPool(std::make_shared<FrameBufferPoolState>(3))
{
}

//===================================================================================
//===================================================================================
// Takes a free buffer from the pool and returns it as a shared_ptr with a custom
// deleter that returns the buffer to the pool on release.
auto VideoFrameAssembler::acquireFrameBuffer() -> FrameBufferPtr
{
    auto pool = m_frameBufferPool;
    std::lock_guard<std::mutex> lock(pool->mutex);
    if (pool->free_buffers.empty())
    {
        return {};
    }

    FrameBuffer* buffer = pool->free_buffers.back().release();
    pool->free_buffers.pop_back();
    buffer->data.clear();

    return FrameBufferPtr(buffer, [pool](FrameBuffer* released_buffer)
    {
        released_buffer->data.clear();
        std::lock_guard<std::mutex> release_lock(pool->mutex);
        pool->free_buffers.emplace_back(released_buffer);
    });
}

//===================================================================================
//===================================================================================
// Returns the index of the frame currently being assembled.
uint32_t VideoFrameAssembler::currentFrameIndex() const
{
    return m_frameIndex;
}

//===================================================================================
//===================================================================================
// Returns accumulated stats and resets the internal counters.
VideoFrameAssembler::Stats VideoFrameAssembler::consumeStats()
{
    Stats stats;
    stats.discarded_frames = m_discardedFrames;
    m_discardedFrames = 0;
    return stats;
}

//===================================================================================
//===================================================================================
// Processes an incoming video packet, assembling it into the current frame.
// Detects lost or out-of-order frames, manages partial frame abandonment,
// and returns a completed frame when the last part arrives.
VideoFrameAssembler::Result VideoFrameAssembler::pushPacket(const Air2Ground_Video_Packet& packet,
                                                            const uint8_t* payload,
                                                            size_t payloadSize,
                                                            bool restoredByFec)
{
    Result result;

    if ((packet.frame_index + 200u < m_frameIndex) || (packet.frame_index > m_frameIndex))
    {
        m_currentFrame.reset();
        m_discardCurrentFrame = false;

        if (m_nextPartIndex != 0)
        {
            result.lostPartialFrame = true;
            result.lostPartialParts = m_nextPartIndex;
            if (packet.frame_index == m_frameIndex + 1)
            {
                result.abandonedOnNewFrameWhileWaitingNextPart = true;
            }
        }

        int df = packet.frame_index - m_frameIndex;
        if (df > 1)
        {
            result.lostWholeFrames = df - 1;
        }

        m_frameIndex = packet.frame_index;
        m_nextPartIndex = 0;
        m_restoredByFec = false;
    }

    if (packet.frame_index != m_frameIndex || packet.part_index != m_nextPartIndex)
    {
        return result;
    }

    if (m_discardCurrentFrame)
    {
        return result;
    }

    if (!m_currentFrame)
    {
        m_currentFrame = acquireFrameBuffer();
        if (!m_currentFrame)
        {
            m_discardCurrentFrame = true;
            ++m_discardedFrames;
            return result;
        }
    }

    m_restoredByFec |= restoredByFec;
    m_nextPartIndex++;

    size_t offset = m_currentFrame->data.size();
    m_currentFrame->data.resize(offset + payloadSize);
    memcpy(m_currentFrame->data.data() + offset, payload, payloadSize);

    if (packet.last_part == 0)
    {
        return result;
    }

    result.completedFrame = true;
    result.completedRestoredByFec = m_restoredByFec;
    result.completedPartIndex = packet.part_index;
    result.completedFrameIndex = packet.frame_index;
    result.frameData = std::move(m_currentFrame);

    m_nextPartIndex = 0;
    m_restoredByFec = false;
    m_discardCurrentFrame = false;

    return result;
}

}
