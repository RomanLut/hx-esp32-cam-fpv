#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "packets.h"

namespace gs::core
{

class VideoFrameAssembler
{
public:
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
        std::vector<uint8_t>* frameData = nullptr;
    };

    uint32_t currentFrameIndex() const;

    Result pushPacket(const Air2Ground_Video_Packet& packet,
                      const uint8_t* payload,
                      size_t payloadSize,
                      bool restoredByFec);

private:
    std::vector<uint8_t> m_currentFrame;
    std::vector<uint8_t> m_completedFrame;
    uint32_t m_frameIndex = 0;
    uint8_t m_nextPartIndex = 0;
    bool m_restoredByFec = false;
};

}
