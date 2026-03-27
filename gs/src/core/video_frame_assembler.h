#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "packets.h"

namespace gs::core
{

class VideoFrameAssembler
{
public:
    struct FrameBuffer
    {
        std::vector<uint8_t> data;
    };

    using FrameBufferPtr = std::shared_ptr<FrameBuffer>;

    struct Stats
    {
        uint32_t discarded_frames = 0;
    };

    struct Result
    {
        bool lostPartialFrame = false;
        uint8_t lostPartialParts = 0;
        int lostWholeFrames = 0;
        bool abandonedOnNewFrameWhileWaitingNextPart = false;

        bool completedFrame = false;
        bool completedRestoredByFec = false;
        uint8_t completedPartIndex = 0;
        uint32_t completedFrameIndex = 0;
        FrameBufferPtr frameData;
    };

    VideoFrameAssembler();

    uint32_t currentFrameIndex() const;

    Result pushPacket(const Air2Ground_Video_Packet& packet,
                      const uint8_t* payload,
                      size_t payloadSize,
                      bool restoredByFec);
    Stats consumeStats();

private:
    struct FrameBufferPoolState
    {
        explicit FrameBufferPoolState(size_t count);

        std::mutex mutex;
        std::vector<std::unique_ptr<FrameBuffer>> free_buffers;
    };

    FrameBufferPtr acquireFrameBuffer();

    std::shared_ptr<FrameBufferPoolState> m_frameBufferPool;
    FrameBufferPtr m_currentFrame;
    uint32_t m_frameIndex = 0;
    uint8_t m_nextPartIndex = 0;
    bool m_restoredByFec = false;
    bool m_discardCurrentFrame = false;
    uint32_t m_discardedFrames = 0;
};

}
