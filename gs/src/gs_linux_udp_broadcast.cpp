#include "gs_linux_udp_broadcast.h"

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "Log.h"

//===================================================================================
//===================================================================================
int LinuxGSUDPBroadcast::udp_socket_init(const std::string& addr, int port)
{
    struct sockaddr_in saddr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) throw std::runtime_error(std::string("Error opening socket: ") + strerror(errno));

    bzero((char *) &saddr, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = inet_addr(addr.c_str());
    saddr.sin_port = htons((unsigned short)port);

    if (connect(fd, (struct sockaddr *) &saddr, sizeof(saddr)) < 0)
    {
        throw std::runtime_error(std::string("Connect error: ") + strerror(errno));
    }

    LOGI("UDP server start! fd:{}", fd);
    return fd;
}

//===================================================================================
//===================================================================================
void LinuxGSUDPBroadcast::send_data_to_udp(int socketfd, uint8_t* buf, int len)
{
    while (len > 1024)
    {
        send(socketfd, buf, 1024, MSG_DONTWAIT);
        buf += 1024;
        len -= 1024;
    }

    if (send(socketfd, buf, len, MSG_DONTWAIT) < 0)
    {
        LOGE("error when sending!");
    }
}

//===================================================================================
//===================================================================================
// Closes the UDP socket if open.
LinuxGSUDPBroadcast::~LinuxGSUDPBroadcast()
{
    if (m_socket_fd > 0)
    {
        ::close(m_socket_fd);
        m_socket_fd = -1;
    }
}

//===================================================================================
//===================================================================================
// Initializes the UDP socket to the given address and port. Returns false if unavailable (non-fatal).
bool LinuxGSUDPBroadcast::init(const std::string& addr, int port)
{
    m_socket_fd = udp_socket_init(addr, port);
    return m_socket_fd > 0;
}

//===================================================================================
//===================================================================================
// Returns true if the UDP socket was successfully opened.
bool LinuxGSUDPBroadcast::isOpen() const
{
    return m_socket_fd > 0;
}

//===================================================================================
//===================================================================================
// Sends video frame data over UDP.
void LinuxGSUDPBroadcast::sendVideoFrame(const uint8_t* data, size_t size)
{
    send_data_to_udp(m_socket_fd, const_cast<uint8_t*>(data), static_cast<int>(size));
}
