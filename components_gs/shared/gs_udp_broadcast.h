#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

//===================================================================================
//===================================================================================
// Abstracts platform-specific UDP broadcast of video frame data.
class IGSUDPBroadcast
{
public:
    virtual ~IGSUDPBroadcast() = default;

    // Initialize the UDP socket. Returns false if disabled or unavailable (non-fatal).
    virtual bool init(const std::string& addr, int port) = 0;

    virtual bool isOpen() const = 0;

    // Send video frame data over UDP.
    virtual void sendVideoFrame(const uint8_t* data, size_t size) = 0;
};

extern IGSUDPBroadcast* g_gsUDPBroadcast;
