#include "gs_stub_transport.h"

//===================================================================================
//===================================================================================
// Creates a stub transport with a descriptive transport name for logging.
GSStubTransport::GSStubTransport(std::string transport_name)
    : m_transport_name(std::move(transport_name))
{
}

//===================================================================================
//===================================================================================
// Stores descriptors and reports that the stub transport is active.
bool GSStubTransport::init(const gs::core::RXDescriptor& rx_descriptor,
                           const gs::core::TXDescriptor& tx_descriptor)
{
    storeDescriptors(rx_descriptor, tx_descriptor);
    if (!m_init_logged)
    {
        LOGW("{} transport is currently a stub implementation", m_transport_name);
        m_init_logged = true;
    }
    return true;
}

//===================================================================================
//===================================================================================
// Performs no background work because the stub transport has no real backend.
void GSStubTransport::process()
{
}

//===================================================================================
//===================================================================================
// Drops outgoing payloads while the transport remains a stub.
void GSStubTransport::send(const void* /* data */, size_t /* size */, bool /* flush */)
{
}

//===================================================================================
//===================================================================================
// Reports that the stub transport never has packets available.
bool GSStubTransport::receive(void* /* data */, size_t& /* size */, bool& /* restoredByFEC */)
{
    return false;
}
