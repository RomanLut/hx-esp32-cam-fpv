#include "linux_apfpv_transport.h"

#include <set>

#include "Log.h"
#include "fmt/core.h"

namespace
{

//===================================================================================
//===================================================================================
// Builds a unique Linux interface list from the RX and TX transport descriptors.
std::vector<std::string> buildApfpvInterfaces(const gs::core::RXDescriptor& rx_descriptor,
                                              const gs::core::TXDescriptor& tx_descriptor)
{
    std::vector<std::string> interfaces;
    std::set<std::string> seen_interfaces;

    for (const std::string& interface : rx_descriptor.interfaces)
    {
        if (!interface.empty() && seen_interfaces.insert(interface).second)
        {
            interfaces.push_back(interface);
        }
    }

    if (!tx_descriptor.interface.empty() && seen_interfaces.insert(tx_descriptor.interface).second)
    {
        interfaces.push_back(tx_descriptor.interface);
    }

    return interfaces;
}

//===================================================================================
//===================================================================================
// Switches Linux Wi-Fi interfaces back to managed mode for APFPV camera connections.
void setManagedMode(const std::vector<std::string>& interfaces)
{
    for (const std::string& interface : interfaces)
    {
        LOGI("Setting managed mode on {}", interface);
        system(fmt::format("sudo ip link set {} down", interface).c_str());
        system(fmt::format("sudo iw dev {} set type managed", interface).c_str());
        system(fmt::format("sudo ip link set {} up", interface).c_str());
    }
}

}

//===================================================================================
//===================================================================================
// Stores descriptors and reports that Linux APFPV currently uses a stub packet backend.
bool LinuxApfpvTransport::init(const gs::core::RXDescriptor& rx_descriptor,
                               const gs::core::TXDescriptor& tx_descriptor)
{
    storeDescriptors(rx_descriptor, tx_descriptor);
    if (!m_init_logged)
    {
        LOGW("Linux APFPV transport is currently a stub implementation");
        m_init_logged = true;
    }
    return true;
}

//===================================================================================
//===================================================================================
// Activates APFPV mode by switching the configured Linux interfaces back to managed mode.
void LinuxApfpvTransport::activate()
{
    setManagedMode(buildApfpvInterfaces(m_rx_descriptor, m_tx_descriptor));
}

//===================================================================================
//===================================================================================
// Performs no background work because Linux APFPV has no packet backend yet.
void LinuxApfpvTransport::process()
{
}

//===================================================================================
//===================================================================================
// Drops outgoing payloads while Linux APFPV remains a stub transport.
void LinuxApfpvTransport::send(const void* /* data */, size_t /* size */, bool /* flush */)
{
}

//===================================================================================
//===================================================================================
// Reports that the Linux APFPV stub transport never has packets available.
bool LinuxApfpvTransport::receive(void* /* data */, size_t& /* size */, bool& /* restoredByFEC */)
{
    return false;
}
