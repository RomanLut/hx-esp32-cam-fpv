#pragma once

#include "core/transport_base.h"

//===================================================================================
//===================================================================================
// Configures Linux Wi-Fi interfaces for APFPV mode while keeping packet I/O as a stub.
class LinuxApfpvTransport final : public gs::core::TransportBase
{
public:
    bool init(const gs::core::RXDescriptor& rx_descriptor,
              const gs::core::TXDescriptor& tx_descriptor) override;
    void activate() override;
    void process() override;
    void send(const void* data, size_t size, bool flush) override;
    bool receive(void* data, size_t& size, bool& restoredByFEC) override;

private:
    bool m_init_logged = false;
};
