#pragma once

#include "ISerialTelemetry.h"

//===================================================================================
//===================================================================================
class LinuxSerialTelemetry : public ISerialTelemetry
{
public:
    ~LinuxSerialTelemetry() override;

    bool init(const std::string& port_name) override;
    bool isOpen() const override;
    int read(uint8_t* buf, size_t max_bytes) override;
    void write(const uint8_t* data, size_t size) override;

private:
    static std::string resolveSerialPortName(const std::string& port_name);

    int m_fd = -1;
};
