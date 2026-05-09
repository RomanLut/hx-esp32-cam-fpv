#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/transport_base.h"

//===================================================================================
//===================================================================================
// Implements the Android APFPV transport adapter used by the native bridge.
// Packet flow:
// - The UDP backend thread receives APFPV transport packets from the camera and enqueues
//   them into a bounded native packet queue as fast as possible.
// - A dedicated native packet-processing worker drains that queue in order, then calls
//   the shared `processTransportPacket` callback for each queued transport packet.
// - That callback updates shared transport counters, pushes the packet into the shared
//   RX FEC block decoder, and drains any fully decoded transport payloads right away.
// - Every decoded payload is passed through the shared GS session/event pipeline, which
//   parses telemetry/config/video packets and feeds completed JPEG frame buffers into
//   the Android JPEG decoder workers as soon as a full video frame is assembled.
// - The Android JPEG decoder worker threads decode those JPEG frame buffers using the
//   selected GS pipeline mode (RGB565 or RGB888) and submit them to `GsVideoRenderer`.
// - The renderer thread uploads the latest decoded frame into the GL texture and then
//   displays that ready texture during the next render pass together with overlay/menu UI.
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
    void activate() override;
    void deactivate() override;
    bool requestImmediateReconnect() override;
    bool supportsMenuSearchOrConnect() const override;
    bool supportsTxPowerControl() const override { return false; }
    bool supportsApfpvInterfaceSelection() const override { return false; }
    bool supportsNetworkInterfaceStatus() const override { return false; }
    void process() override;
    void reset_rx_state() override;
    void beginMenuSearchOrConnect() override;
    bool advanceMenuSearchOrConnect() override;
    void cancelMenuSearchOrConnect() override;
    void send(const void* data, size_t size, bool flush) override;
    bool receive(void* data, size_t& size, bool& restoredByFEC) override;
    size_t get_data_rate() const override;
    int get_input_dBm() const override;
    void setInputDbm(int input_dbm);
    void configureUdpEndpoint(std::string peer_host, int peer_port, int local_port);
    const std::string& udpPeerHost() const;
    int udpPeerPort() const;
    int udpLocalPort() const;
    void updateUdpStats(uint64_t packets_received,
                        float throughput_mbps,
                        int received_completed_frames,
                        int restored_completed_frames);
    uint64_t udpPacketsReceived() const;
    bool hasSeenUdpPackets() const;
    float udpThroughputMbps() const;
    int receivedCompletedFrames() const;
    int restoredCompletedFrames() const;
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
    bool isMenuSearchActive() const;
    bool consumeReconnectRequest();
    void syncCameraState(size_t discovered_camera_count, bool has_active_camera,
                         std::string connecting_ssid);
    std::string getTransportMessage() const override;

private:
    void runUdpClientLoop(UdpLoopCallbacks callbacks);

    std::vector<uint8_t> m_last_sent_packet;
    int m_input_dbm = 0;
    uint64_t m_udp_packets_received = 0;
    std::atomic<bool> m_udp_packets_seen = false;
    float m_udp_throughput_mbps = 0.0f;
    int m_received_completed_frames = 0;
    int m_restored_completed_frames = 0;
    std::string m_udp_peer_host = "192.168.4.1";
    int m_udp_peer_port = 5600;
    int m_udp_local_port = 5600;
    std::string m_udp_last_error;
    std::atomic<bool> m_udp_stop_requested = false;
    std::atomic<bool> m_udp_running = false;
    std::atomic<bool> m_reconnect_requested = false;
    std::atomic<bool> m_menu_search_active = false;
    std::atomic<bool> m_menu_search_done = false;
    std::atomic<bool> m_has_active_camera = false;
    uint32_t m_udp_stats_log_count = 0;
    mutable std::mutex m_udp_thread_mutex;
    std::mutex m_udp_lifecycle_mutex;
    std::thread m_udp_thread;
    mutable std::mutex m_connecting_ssid_mutex;
    std::string m_connecting_ssid;
};
