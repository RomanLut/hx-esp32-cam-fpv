#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

//===================================================================================
//===================================================================================
// Abstracts platform-specific serial port access for mavlink telemetry.
class ISerialTelemetry
{
public:
    virtual ~ISerialTelemetry() = default;

    // Open and configure the serial port. Returns false if unavailable (non-fatal).
    virtual bool init(const std::string& port_name) = 0;

    virtual bool isOpen() const = 0;

    // Non-blocking read. Returns number of bytes read, or -1 on error.
    virtual int read(uint8_t* buf, size_t max_bytes) = 0;

    // Write outbound telemetry bytes.
    virtual void write(const uint8_t* data, size_t size) = 0;
};

extern ISerialTelemetry* g_serialTelemetry;
