#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "gs_wifi_scan_transport.h"

struct pcap;
typedef struct pcap pcap_t;

//===================================================================================
//===================================================================================
// Linux implementation of the WiFi channel scan transport.
//
// Opens a pcap handle on the first RX interface in monitor mode.  A dedicated
// background thread drains the pcap handle continuously so that every 802.11 packet
// on the current channel is counted regardless of the receive() call cadence. The
// shutdown path explicitly breaks pcap dispatch before joining the thread. Monitor
// mode and pcap are configured once per Scan activation; channel hops only retune the
// live interface because repeated link resets wedge rtl88x2eu reception.
class LinuxWifiScanTransport final : public GSWifiScanTransport
{
public:
    ~LinuxWifiScanTransport() override;

    bool init(const gs::core::RXDescriptor& rx_descriptor,
              const gs::core::TXDescriptor& tx_descriptor) override;
    void deactivate() override;

protected:
    void setMonitorChannel(int channel) override;

private:
    bool openMonitorPcap(const std::string& interface);
    void closeMonitorPcap();
    void stopCaptureThread();
    void captureThreadFunc();
    static void packetCallback(u_char* user, const struct pcap_pkthdr* h, const u_char* bytes);

    pcap_t*               m_pcap               = nullptr;
    std::atomic<int>      m_channelFreqMHz     = {0}; // frequency of the current monitor channel
    std::atomic<bool>     m_captureStop        = {false};
    std::thread           m_captureThread;
    std::string           m_interface;
};
