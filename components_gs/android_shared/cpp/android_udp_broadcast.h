#pragma once

#include "gs_udp_broadcast.h"

class AndroidGSUDPBroadcast : public IGSUDPBroadcast
{
public:
    bool init(const std::string& /*addr*/, int /*port*/) override { return false; }
    bool isOpen() const override { return false; }
    void sendVideoFrame(const uint8_t* /*data*/, size_t /*size*/) override {}
};
