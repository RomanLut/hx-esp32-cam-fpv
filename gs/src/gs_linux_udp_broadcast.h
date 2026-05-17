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
    static int udp_socket_init(const std::string& addr, int port);
    static void send_data_to_udp(int socketfd, uint8_t* buf, int len);

    int m_socket_fd = -1;
};
