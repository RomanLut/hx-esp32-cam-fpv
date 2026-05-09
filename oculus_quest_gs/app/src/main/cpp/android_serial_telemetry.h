#pragma once

#include "ISerialTelemetry.h"

class AndroidSerialTelemetry : public ISerialTelemetry
{
public:
    bool init(const std::string& /*port_name*/) override { return false; }
    bool isOpen() const override { return false; }
    int read(uint8_t* /*buf*/, size_t /*max_bytes*/) override { return 0; }
    void write(const uint8_t* /*data*/, size_t /*size*/) override {}
};
