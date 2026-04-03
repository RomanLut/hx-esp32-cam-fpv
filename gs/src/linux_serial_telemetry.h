#pragma once

#include "ISerialTelemetry.h"

class LinuxSerialTelemetry : public ISerialTelemetry
{
public:
    ~LinuxSerialTelemetry() override;

    bool init(const std::string& port_name) override;
    bool isOpen() const override;
    int read(uint8_t* buf, size_t max_bytes) override;
    void write(const uint8_t* data, size_t size) override;

private:
    int m_fd = -1;
};
