#include "linux_serial_telemetry.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

LinuxSerialTelemetry::~LinuxSerialTelemetry()
{
    if (m_fd != -1)
    {
        ::close(m_fd);
    }
}

bool LinuxSerialTelemetry::init(const std::string& port_name)
{
    m_fd = open(port_name.c_str(), O_RDWR);
    if (m_fd == -1)
    {
        printf("Warning: Can not open serial port %s. Telemetry will not be available.\n", port_name.c_str());
        return false;
    }

    struct termios tty;
    if (tcgetattr(m_fd, &tty) != 0)
    {
        printf("Error %i from tcgetattr: %s\n", errno, strerror(errno));
        return false;
    }

    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_lflag &= ~ICANON;
    tty.c_lflag &= ~ECHO;
    tty.c_lflag &= ~ECHOE;
    tty.c_lflag &= ~ECHONL;
    tty.c_lflag &= ~ISIG;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;
    tty.c_cc[VTIME] = 0;
    tty.c_cc[VMIN] = 0;

    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);

    if (tcsetattr(m_fd, TCSANOW, &tty) != 0)
    {
        printf("Error %i from tcsetattr: %s\n", errno, strerror(errno));
        return false;
    }

    return true;
}

bool LinuxSerialTelemetry::isOpen() const
{
    return m_fd != -1;
}

int LinuxSerialTelemetry::read(uint8_t* buf, size_t max_bytes)
{
    return static_cast<int>(::read(m_fd, buf, max_bytes));
}

void LinuxSerialTelemetry::write(const uint8_t* data, size_t size)
{
    ::write(m_fd, data, size);
}
