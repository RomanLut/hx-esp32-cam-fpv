#pragma once

#include <string>

#include "core/transport_base.h"

//===================================================================================
//===================================================================================
// Implements a named stub transport that keeps runtime wiring alive without packet I/O.
class GSStubTransport final : public gs::core::TransportBase
{
public:
    explicit GSStubTransport(std::string transport_name);
    bool init(const gs::core::RXDescriptor& rx_descriptor,
              const gs::core::TXDescriptor& tx_descriptor) override;
    void process() override;
    void send(const void* data, size_t size, bool flush) override;
    bool receive(void* data, size_t& size, bool& restoredByFEC) override;

private:
    std::string m_transport_name;
    bool m_init_logged = false;
};
