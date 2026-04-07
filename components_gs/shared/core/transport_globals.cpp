#include "core/transport_manager.h"
#include "core/transport_base.h"

namespace
{

//===================================================================================
//===================================================================================
// Provides a harmless fallback transport before a platform manager is installed.
class NullTransport final : public gs::core::TransportBase
{
public:
    //===================================================================================
    //===================================================================================
    // Accepts descriptor initialization without creating any transport resources.
    bool init(const gs::core::RXDescriptor& rx_descriptor,
              const gs::core::TXDescriptor& tx_descriptor) override
    {
        storeDescriptors(rx_descriptor, tx_descriptor);
        return true;
    }

    //===================================================================================
    //===================================================================================
    // Performs no background work for the fallback transport.
    void process() override {}

    //===================================================================================
    //===================================================================================
    // Drops outgoing bytes while no platform transport manager is available.
    void send(const void* /* data */, size_t /* size */, bool /* flush */) override {}

    //===================================================================================
    //===================================================================================
    // Reports that no packets are available from the fallback transport.
    bool receive(void* /* data */, size_t& /* size */, bool& /* restoredByFEC */) override
    {
        return false;
    }
};

NullTransport s_null_transport;

}

gs::core::ITransportManager* s_transportManager = nullptr;
gs::core::ITransport* s_transport = &s_null_transport;
std::mutex s_transport_mutex;
