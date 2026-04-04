#include "gs_linux_udp_broadcast.h"

#include <unistd.h>

#include "gs_linux_socket.h"

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
