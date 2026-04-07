#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "core/transport_base.h"

//===================================================================================
//===================================================================================
// Implements the current Android APFPV transport adapter used by the native bridge.
class AndroidAPFPVTransport final : public gs::core::TransportBase
{
public:
    //===================================================================================
    //===================================================================================
    // Provides the runtime callbacks required by the APFPV UDP backend loop.
    struct UdpLoopCallbacks
    {
        std::function<std::vector<std::vector<uint8_t>>()> buildControlPackets;
        std::function<void(const uint8_t* data, size_t size)> processTransportPacket;
        std::function<uint64_t()> submittedFrameCount;
    };

    bool init(const gs::core::RXDescriptor& rx_descriptor,
              const gs::core::TXDescriptor& tx_descriptor) override;
    void process() override;
    void send(const void* data, size_t size, bool flush) override;
    bool receive(void* data, size_t& size, bool& restoredByFEC) override;
    int get_input_dBm() const override;
    void setInputDbm(int input_dbm);
    void configureUdpEndpoint(std::string peer_host, int peer_port, int local_port);
    const std::string& udpPeerHost() const;
    int udpPeerPort() const;
    int udpLocalPort() const;
    void updateUdpStats(uint64_t packets_received, float throughput_mbps, float video_fps);
    uint64_t udpPacketsReceived() const;
    float udpThroughputMbps() const;
    float udpVideoFps() const;
    void clearUdpError();
    void setUdpError(std::string error);
    const std::string& udpLastError() const;
    void setUdpRunning(bool running);
    bool isUdpRunning() const;
    void requestUdpStop(bool stop_requested);
    bool udpStopRequested() const;
    bool hasJoinableUdpThread() const;
    void joinUdpThread();
    void setUdpThread(std::thread thread);
    bool startUdpClient(UdpLoopCallbacks callbacks);
    void stopUdpClient();

private:
    void runUdpClientLoop(UdpLoopCallbacks callbacks);

    std::vector<uint8_t> m_last_sent_packet;
    int m_input_dbm = 0;
    uint64_t m_udp_packets_received = 0;
    float m_udp_throughput_mbps = 0.0f;
    float m_udp_video_fps = 0.0f;
    std::string m_udp_peer_host = "192.168.4.1";
    int m_udp_peer_port = 5600;
    int m_udp_local_port = 5600;
    std::string m_udp_last_error;
    std::atomic<bool> m_udp_stop_requested = false;
    bool m_udp_running = false;
    std::thread m_udp_thread;
};
