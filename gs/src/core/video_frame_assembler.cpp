#include "core/video_frame_assembler.h"

#include <cstring>

namespace gs::core
{

uint32_t VideoFrameAssembler::currentFrameIndex() const
{
    return m_frameIndex;
}

VideoFrameAssembler::Result VideoFrameAssembler::pushPacket(const Air2Ground_Video_Packet& packet,
                                                            const uint8_t* payload,
                                                            size_t payloadSize,
                                                            bool restoredByFec)
{
    Result result;

    if ((packet.frame_index + 200u < m_frameIndex) || (packet.frame_index > m_frameIndex))
    {
        m_currentFrame.clear();

        if (m_nextPartIndex != 0)
        {
            result.lostPartialFrame = true;
            result.lostPartialParts = m_nextPartIndex;
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

    m_restoredByFec |= restoredByFec;
    m_nextPartIndex++;

    size_t offset = m_currentFrame.size();
    m_currentFrame.resize(offset + payloadSize);
    memcpy(m_currentFrame.data() + offset, payload, payloadSize);

    if (packet.last_part == 0)
    {
        return result;
    }

    m_completedFrame = std::move(m_currentFrame);
    m_currentFrame.clear();

    result.completedFrame = true;
    result.completedRestoredByFec = m_restoredByFec;
    result.completedPartIndex = packet.part_index;
    result.completedFrameIndex = packet.frame_index;
    result.frameData = &m_completedFrame;

    m_nextPartIndex = 0;
    m_restoredByFec = false;

    return result;
}

}
