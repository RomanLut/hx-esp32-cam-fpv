#pragma once

#include "gs_udp_broadcast.h"

//===================================================================================
//===================================================================================
class LinuxGSUDPBroadcast : public IGSUDPBroadcast
{
public:
    ~LinuxGSUDPBroadcast() override;

    bool init(const std::string& addr, int port) override;
    bool isOpen() const override;
    void sendVideoFrame(const uint8_t* data, size_t size) override;

private:
    int m_socket_fd = -1;
};
