#pragma once

#include <cstdint>
#include <mutex>

extern std::mutex s_gs_stats_mutex;

struct GSStats
{
    uint16_t outPacketCounter = 0;
    uint16_t inPacketCounterAll[2] = {0,0};  //all received radio packets per interface for the last second
    uint16_t inPacketCounter[2] = {0,0};  //keep stack of max 2 interfaces max

    uint32_t lastPacketIndex = 0;           //gs_stats: last recevied packet index, last_gs_stats: last packet index received for the period of last_gs_stats
    uint32_t statsPacketIndex = 0;          //packets index when inUniquePacketCounter is stated counting
    uint16_t inDublicatedPacketCounter = 0;
    uint16_t inUniquePacketCounter = 0;

    uint32_t FECSuccPacketIndexCounter = 0;
    uint32_t FECBlocksCounter = 0;

    int8_t rssiDbm[2] = {0,0};  //negative value of RSSI
    int8_t noiseFloorDbm = 0; //negative value

    uint8_t brokenFrames = 0;  //JPEG decoding errors

    int pingMinMS = 0;
    int pingMaxMS = 0;

    int RCPeriodMax = -1;  //ms

    int decodedJpegCount = 0;
    int decodedJpegTimeTotalMS = 0;

    int decodedJpegTimeMinMS = 99;
    int decodedJpegTimeMaxMS = 0;
    int textureUploadCount = 0;
    int textureUploadTimeTotalMS = 0;
    int textureUploadTimeMinMS = 99;
    int textureUploadTimeMaxMS = 0;
    int gpuWaitLastFrameMS = 0;
    int gpuWaitMaxMS = 0;
    int stabilizationCount = 0;
    int stabilizationTimeMinMS = 0;
    int stabilizationTimeMaxMS = 0;
    int discardedFramesAssemblerPoolOverflow = 0;
    int discardedFramesDecoderInput = 0;
    int discardedFramesDecodedOutput = 0;
    int restoredTransportPackets = 0;
    int restoredVideoParts = 0;
    int receivedCompletedFrames = 0;
    int restoredCompletedFrames = 0;
};

struct GSStatsSync
{
    uint16_t outPacketCounter = 0;
    uint16_t inPacketCounterAll[2] = {0,0};  //all received radio packets per interface for the last second
    uint16_t inPacketCounter[2] = {0,0};  //keep stack of max 2 interfaces max
};

//===================================================================================
//===================================================================================
// Returns whether one GS stats window indicates heavy on-channel interference.
inline bool shouldShowInterferenceChip(const GSStats& gs_stats)
{
    const int valid_packets =
        static_cast<int>(gs_stats.inPacketCounter[0]) +
        static_cast<int>(gs_stats.inPacketCounter[1]);
    const int all_packets =
        static_cast<int>(gs_stats.inPacketCounterAll[0]) +
        static_cast<int>(gs_stats.inPacketCounterAll[1]);

    return valid_packets > 100 && all_packets > 0 && valid_packets * 10 < all_packets * 7;
}

extern GSStats& s_gs_stats;
extern GSStats& s_last_gs_stats;
