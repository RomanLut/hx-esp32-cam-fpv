#include "android_apfpv_transport.h"
#include "../../../../../components/common/Clock.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "fec.h"

namespace
{

constexpr const char* kAndroidApfpvLogTag = "AndroidAPFPVTransport";
constexpr int kAndroidApfpvReceiveBufferSizeBytes = 256 * 1024;
constexpr size_t kAndroidApfpvMaxQueuedPackets = 2048;

}

//===================================================================================
//===================================================================================
// Stores transport descriptors for the Android APFPV bridge transport.
bool AndroidAPFPVTransport::init(const gs::core::RXDescriptor& rx_descriptor,
                                 const gs::core::TXDescriptor& tx_descriptor)
{
    storeDescriptors(rx_descriptor, tx_descriptor);
    return true;
}

//===================================================================================
//===================================================================================
// Activates the Android APFPV transport and requests preferred-camera reconnect flow.
void AndroidAPFPVTransport::activate()
{
    m_menu_search_active.store(false);
    m_menu_search_done.store(false);
    m_reconnect_requested.store(true);
}

//===================================================================================
//===================================================================================
// Deactivates the Android APFPV transport and clears transient APFPV menu state.
void AndroidAPFPVTransport::deactivate()
{
    m_menu_search_active.store(false);
    m_menu_search_done.store(false);
    m_reconnect_requested.store(false);
    m_has_active_camera.store(false);
    {
        std::lock_guard<std::mutex> lock(m_connecting_ssid_mutex);
        m_connecting_ssid.clear();
    }
    clearApfpvCameraRuntimeState();
}

//===================================================================================
//===================================================================================
// Requests an immediate reconnect to the currently preferred APFPV camera on Android.
bool AndroidAPFPVTransport::requestImmediateReconnect()
{
    m_menu_search_active.store(false);
    m_menu_search_done.store(false);
    m_reconnect_requested.store(true);
    clearApfpvActiveCamera();
    return true;
}

//===================================================================================
//===================================================================================
// Reports that Android APFPV supports the shared menu-driven search/connect flow.
bool AndroidAPFPVTransport::supportsMenuSearchOrConnect() const
{
    return true;
}

//===================================================================================
//===================================================================================
// Performs no background work because the UDP client is driven by the transport thread.
void AndroidAPFPVTransport::process()
{
}

//===================================================================================
//===================================================================================
// Resets Android APFPV receive/runtime state after a transport or Wi-Fi reconnect.
void AndroidAPFPVTransport::reset_rx_state()
{
    m_menu_search_done.store(false);
}

//===================================================================================
//===================================================================================
// Starts the shared APFPV menu search flow and clears stale discovered-camera state.
void AndroidAPFPVTransport::beginMenuSearchOrConnect()
{
    m_menu_search_active.store(true);
    m_menu_search_done.store(false);
    m_reconnect_requested.store(false);
    updateApfpvDiscoveredCameras({});
    clearApfpvActiveCamera();
}

//===================================================================================
//===================================================================================
// Advances the shared Android APFPV menu search flow until the controller marks it done.
bool AndroidAPFPVTransport::advanceMenuSearchOrConnect()
{
    return m_menu_search_done.load();
}

//===================================================================================
//===================================================================================
// Cancels the shared Android APFPV menu search flow without changing saved preference.
void AndroidAPFPVTransport::cancelMenuSearchOrConnect()
{
    m_menu_search_active.store(false);
    m_menu_search_done.store(false);
}

//===================================================================================
//===================================================================================
// Caches the last outgoing packet emitted through the Android APFPV bridge.
void AndroidAPFPVTransport::send(const void* data, size_t size, bool /* flush */)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    m_last_sent_packet.assign(bytes, bytes + size);
}

//===================================================================================
//===================================================================================
// Reports no packets because RX delivery is injected externally by the UDP backend loop.
bool AndroidAPFPVTransport::receive(void* /* data */, size_t& /* size */, bool& /* restoredByFEC */)
{
    return false;
}

//===================================================================================
//===================================================================================
// Returns the latest measured APFPV transport throughput in bytes per second.
size_t AndroidAPFPVTransport::get_data_rate() const
{
    return static_cast<size_t>(m_udp_throughput_mbps * 1000.0f * 1000.0f / 8.0f);
}

//===================================================================================
//===================================================================================
// Returns the latest input RSSI estimate injected by the runtime packet path.
int AndroidAPFPVTransport::get_input_dBm() const
{
    return m_input_dbm;
}

//===================================================================================
//===================================================================================
// Updates the latest input RSSI estimate from the UDP receive path.
void AndroidAPFPVTransport::setInputDbm(int input_dbm)
{
    m_input_dbm = input_dbm;
}

//===================================================================================
//===================================================================================
// Stores the UDP endpoint configuration used by the Android APFPV backend.
void AndroidAPFPVTransport::configureUdpEndpoint(std::string peer_host, int peer_port, int local_port)
{
    m_udp_peer_host = std::move(peer_host);
    m_udp_peer_port = peer_port;
    m_udp_local_port = local_port;
}

//===================================================================================
//===================================================================================
// Returns the configured UDP peer hostname for the Android APFPV backend.
const std::string& AndroidAPFPVTransport::udpPeerHost() const
{
    return m_udp_peer_host;
}

//===================================================================================
//===================================================================================
// Returns the configured UDP peer port for the Android APFPV backend.
int AndroidAPFPVTransport::udpPeerPort() const
{
    return m_udp_peer_port;
}

//===================================================================================
//===================================================================================
// Returns the configured UDP local port for the Android APFPV backend.
int AndroidAPFPVTransport::udpLocalPort() const
{
    return m_udp_local_port;
}

//===================================================================================
//===================================================================================
// Updates the latest UDP receive counters and derived APFPV transport stats.
void AndroidAPFPVTransport::updateUdpStats(uint64_t packets_received,
                                           float throughput_mbps,
                                           int received_completed_frames,
                                           int restored_completed_frames)
{
    m_udp_packets_received = packets_received;
    m_udp_throughput_mbps = throughput_mbps;
    m_received_completed_frames = received_completed_frames;
    m_restored_completed_frames = restored_completed_frames;
    if (packets_received > 0)
    {
        m_udp_packets_seen.store(true);
    }
    m_udp_stats_log_count++;
    if ((m_udp_stats_log_count % 4U) == 1U)
    {
        __android_log_print(ANDROID_LOG_INFO,
                            kAndroidApfpvLogTag,
                            "udp_stats peer=%s:%d packets=%llu mbps=%.2f frames=%d restored=%d error=%s",
                            m_udp_peer_host.c_str(),
                            m_udp_peer_port,
                            static_cast<unsigned long long>(m_udp_packets_received),
                            static_cast<double>(m_udp_throughput_mbps),
                            m_received_completed_frames,
                            m_restored_completed_frames,
                            m_udp_last_error.empty() ? "-" : m_udp_last_error.c_str());
    }
}

//===================================================================================
//===================================================================================
// Returns the latest UDP packet receive counter for the Android APFPV backend.
uint64_t AndroidAPFPVTransport::udpPacketsReceived() const
{
    return m_udp_packets_received;
}

//===================================================================================
//===================================================================================
// Returns whether the Android APFPV backend has already received any UDP packets.
bool AndroidAPFPVTransport::hasSeenUdpPackets() const
{
    return m_udp_packets_seen.load();
}

//===================================================================================
//===================================================================================
// Returns the latest measured UDP throughput for the Android APFPV backend.
float AndroidAPFPVTransport::udpThroughputMbps() const
{
    return m_udp_throughput_mbps;
}

//===================================================================================
//===================================================================================
// Returns the latest measured video FPS for the Android APFPV backend.
int AndroidAPFPVTransport::receivedCompletedFrames() const
{
    return m_received_completed_frames;
}

//===================================================================================
//===================================================================================
// Returns the APFPV frame count restored through transport redundancy in the last window.
int AndroidAPFPVTransport::restoredCompletedFrames() const
{
    return m_restored_completed_frames;
}

//===================================================================================
//===================================================================================
// Clears the last recorded UDP backend error string.
void AndroidAPFPVTransport::clearUdpError()
{
    m_udp_last_error.clear();
}

//===================================================================================
//===================================================================================
// Stores the last recorded UDP backend error string.
void AndroidAPFPVTransport::setUdpError(std::string error)
{
    m_udp_last_error = std::move(error);
}

//===================================================================================
//===================================================================================
// Returns the last recorded UDP backend error string.
const std::string& AndroidAPFPVTransport::udpLastError() const
{
    return m_udp_last_error;
}

//===================================================================================
//===================================================================================
// Marks the Android APFPV UDP backend as running or stopped.
void AndroidAPFPVTransport::setUdpRunning(bool running)
{
    m_udp_running.store(running);
}

//===================================================================================
//===================================================================================
// Returns whether the Android APFPV UDP backend is currently running.
bool AndroidAPFPVTransport::isUdpRunning() const
{
    return m_udp_running.load();
}

//===================================================================================
//===================================================================================
// Updates the stop flag observed by the Android APFPV UDP backend thread.
void AndroidAPFPVTransport::requestUdpStop(bool stop_requested)
{
    m_udp_stop_requested.store(stop_requested);
}

//===================================================================================
//===================================================================================
// Returns whether the Android APFPV UDP backend thread was asked to stop.
bool AndroidAPFPVTransport::udpStopRequested() const
{
    return m_udp_stop_requested.load();
}

//===================================================================================
//===================================================================================
// Returns true when the Android APFPV transport currently owns a joinable UDP thread.
bool AndroidAPFPVTransport::hasJoinableUdpThread() const
{
    std::lock_guard<std::mutex> lock(m_udp_thread_mutex);
    return m_udp_thread.joinable();
}

//===================================================================================
//===================================================================================
// Joins the current Android APFPV UDP thread if it is still joinable.
void AndroidAPFPVTransport::joinUdpThread()
{
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(m_udp_thread_mutex);
        if (!m_udp_thread.joinable())
        {
            return;
        }

        thread = std::move(m_udp_thread);
    }

    if (thread.joinable())
    {
        thread.join();
    }
}

//===================================================================================
//===================================================================================
// Replaces the owned Android APFPV UDP thread with a newly started backend thread.
void AndroidAPFPVTransport::setUdpThread(std::thread thread)
{
    std::lock_guard<std::mutex> lock(m_udp_thread_mutex);
    m_udp_thread = std::move(thread);
}

//===================================================================================
//===================================================================================
// Starts the Android APFPV UDP backend thread with the provided runtime callbacks.
bool AndroidAPFPVTransport::startUdpClient(UdpLoopCallbacks callbacks)
{
    std::lock_guard<std::mutex> lifecycle_lock(m_udp_lifecycle_mutex);

    if (isUdpRunning())
    {
        __android_log_print(ANDROID_LOG_WARN, kAndroidApfpvLogTag, "startUdpClient ignored: already running");
        return false;
    }

    if (hasJoinableUdpThread())
    {
        __android_log_print(ANDROID_LOG_INFO, kAndroidApfpvLogTag, "startUdpClient joining stale thread before restart");
        requestUdpStop(true);
        joinUdpThread();
        setUdpRunning(false);
    }

    requestUdpStop(false);
    setUdpThread(std::thread([this, callbacks = std::move(callbacks)]() mutable
    {
        runUdpClientLoop(std::move(callbacks));
    }));
    return true;
}

//===================================================================================
//===================================================================================
// Stops and joins the Android APFPV UDP backend thread.
void AndroidAPFPVTransport::stopUdpClient()
{
    std::lock_guard<std::mutex> lifecycle_lock(m_udp_lifecycle_mutex);

    __android_log_print(ANDROID_LOG_INFO,
                        kAndroidApfpvLogTag,
                        "stopUdpClient running=%d joinable=%d",
                        isUdpRunning() ? 1 : 0,
                        hasJoinableUdpThread() ? 1 : 0);
    requestUdpStop(true);
    joinUdpThread();
    setUdpRunning(false);
}

//===================================================================================
//===================================================================================
// Returns whether the shared Android APFPV menu search flow is currently active.
bool AndroidAPFPVTransport::isMenuSearchActive() const
{
    return m_menu_search_active.load();
}

//===================================================================================
//===================================================================================
// Returns and clears one queued Android APFPV reconnect request for the Wi-Fi controller.
bool AndroidAPFPVTransport::consumeReconnectRequest()
{
    return m_reconnect_requested.exchange(false);
}

//===================================================================================
//===================================================================================
// Updates shared Android APFPV search state from the latest Wi-Fi discovery snapshot.
//===================================================================================
//===================================================================================
// Returns a user-facing connection progress string while the APFPV link is establishing.
// Derives the message entirely from the state passed via syncCameraState.
std::string AndroidAPFPVTransport::getTransportMessage() const
{
    {
        std::lock_guard<std::mutex> lock(m_connecting_ssid_mutex);
        if (!m_connecting_ssid.empty())
        {
            return "Connecting to WiFi network " + m_connecting_ssid + "...";
        }
    }
    if (m_has_active_camera.load() && !m_udp_packets_seen.load())
    {
        return "Connecting to stream...";
    }
    return {};
}

//===================================================================================
//===================================================================================
void AndroidAPFPVTransport::syncCameraState(size_t discovered_camera_count, bool has_active_camera,
                                            std::string connecting_ssid)
{
    m_has_active_camera.store(has_active_camera);
    {
        std::lock_guard<std::mutex> lock(m_connecting_ssid_mutex);
        m_connecting_ssid = std::move(connecting_ssid);
    }

    if (has_active_camera)
    {
        m_reconnect_requested.store(false);
        if (m_menu_search_active.exchange(false))
        {
            m_menu_search_done.store(true);
        }
        return;
    }

    if (m_menu_search_active.load() && discovered_camera_count >= 2)
    {
        m_menu_search_active.store(false);
        m_menu_search_done.store(true);
    }
}

//===================================================================================
//===================================================================================
// Runs the Android APFPV UDP backend loop for control TX, media RX, and transport stats.
void AndroidAPFPVTransport::runUdpClientLoop(UdpLoopCallbacks callbacks)
{
    using ClockType = Clock;

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* addrinfo_result = nullptr;
    const std::string peer_port_string = std::to_string(m_udp_peer_port);
    if (getaddrinfo(m_udp_peer_host.c_str(), peer_port_string.c_str(), &hints, &addrinfo_result) != 0 ||
        addrinfo_result == nullptr)
    {
        setUdpRunning(false);
        setUdpError("peer resolve failed");
        __android_log_print(ANDROID_LOG_ERROR, kAndroidApfpvLogTag, "udp_start failed: %s", udpLastError().c_str());
        return;
    }

    const int socket_fd = socket(addrinfo_result->ai_family, addrinfo_result->ai_socktype, addrinfo_result->ai_protocol);
    if (socket_fd < 0)
    {
        freeaddrinfo(addrinfo_result);
        setUdpRunning(false);
        setUdpError(std::string("socket create failed: ") + std::strerror(errno));
        __android_log_print(ANDROID_LOG_ERROR, kAndroidApfpvLogTag, "udp_start failed: %s", udpLastError().c_str());
        return;
    }

    int reuse_opt = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_opt, sizeof(reuse_opt));
#ifdef SO_REUSEPORT
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, &reuse_opt, sizeof(reuse_opt));
#endif

    // Android socket defaults vary by device/kernel. Pin the receive buffer so short
    // packet-processing stalls have more room before the kernel starts dropping bursts.
    int receive_buffer_size = kAndroidApfpvReceiveBufferSizeBytes;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer_size, sizeof(receive_buffer_size)) != 0)
    {
        __android_log_print(ANDROID_LOG_WARN,
                            kAndroidApfpvLogTag,
                            "setsockopt SO_RCVBUF=%d failed: %s",
                            receive_buffer_size,
                            std::strerror(errno));
    }
    else
    {
        int effective_receive_buffer_size = 0;
        socklen_t effective_receive_buffer_size_len = sizeof(effective_receive_buffer_size);
        if (getsockopt(socket_fd,
                       SOL_SOCKET,
                       SO_RCVBUF,
                       &effective_receive_buffer_size,
                       &effective_receive_buffer_size_len) == 0)
        {
            __android_log_print(ANDROID_LOG_INFO,
                                kAndroidApfpvLogTag,
                                "SO_RCVBUF requested=%d effective=%d",
                                receive_buffer_size,
                                effective_receive_buffer_size);
        }
    }

    sockaddr_in local_address = {};
    local_address.sin_family = AF_INET;
    local_address.sin_port = htons(static_cast<uint16_t>(m_udp_local_port));
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, reinterpret_cast<const sockaddr*>(&local_address), sizeof(local_address)) != 0)
    {
        close(socket_fd);
        freeaddrinfo(addrinfo_result);
        setUdpRunning(false);
        setUdpError(std::string("bind failed: ") + std::strerror(errno));
        __android_log_print(ANDROID_LOG_ERROR, kAndroidApfpvLogTag, "udp_start failed: %s", udpLastError().c_str());
        return;
    }

    timeval timeout = {};
    timeout.tv_usec = 250000;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    setUdpRunning(true);
    clearUdpError();
    m_udp_packets_seen.store(false);
    updateUdpStats(0, 0.0f, 0, 0);
    __android_log_print(ANDROID_LOG_INFO,
                        kAndroidApfpvLogTag,
                        "udp_start ok peer=%s:%d local=%d",
                        m_udp_peer_host.c_str(),
                        m_udp_peer_port,
                        m_udp_local_port);

    std::array<uint8_t, 2048> rx_buffer = {};
    uint64_t packets_received = 0;
    uint64_t bytes_window = 0;
    uint64_t frame_count_start = callbacks.submittedFrameCount ? callbacks.submittedFrameCount() : 0;
    auto stats_window_start = ClockType::now();
    auto next_control_send = ClockType::now();
    std::mutex queued_packet_mutex;
    std::condition_variable queued_packet_cv;
    std::deque<std::vector<uint8_t>> queued_packets;
    std::atomic<bool> packet_worker_exit = false;
    std::atomic<uint32_t> dropped_queued_packets = 0;

    // The UDP receive thread should stay close to recvfrom() so Android scheduler jitter
    // does not immediately turn into kernel-buffer loss. A separate worker keeps the
    // shared FEC/session processing off the socket-consumption critical path.
    std::thread packet_worker_thread([&]()
    {
        while (true)
        {
            std::vector<uint8_t> queued_packet;
            {
                std::unique_lock<std::mutex> lock(queued_packet_mutex);
                queued_packet_cv.wait(lock, [&]()
                {
                    return packet_worker_exit.load() || !queued_packets.empty();
                });
                if (packet_worker_exit.load() && queued_packets.empty())
                {
                    break;
                }
                if (queued_packets.empty())
                {
                    continue;
                }

                queued_packet = std::move(queued_packets.front());
                queued_packets.pop_front();
            }

            if (!queued_packet.empty() && callbacks.processTransportPacket)
            {
                callbacks.processTransportPacket(queued_packet.data(), queued_packet.size());
            }
        }
    });

    while (!udpStopRequested())
    {
        const auto now = ClockType::now();
        if (now >= next_control_send && callbacks.buildControlPackets)
        {
            const std::vector<std::vector<uint8_t>> control_packets = callbacks.buildControlPackets();
            for (const auto& packet : control_packets)
            {
                if (!packet.empty())
                {
                    const ssize_t sent = sendto(socket_fd,
                                                packet.data(),
                                                packet.size(),
                                                0,
                                                addrinfo_result->ai_addr,
                                                static_cast<socklen_t>(addrinfo_result->ai_addrlen));
                    (void)sent;
                }
            }
            next_control_send = now + std::chrono::milliseconds(250);
        }

        ssize_t received = recvfrom(socket_fd, rx_buffer.data(), rx_buffer.size(), 0, nullptr, nullptr);
        if (received > 0)
        {
            packets_received++;
            bytes_window += static_cast<uint64_t>(received);
            m_udp_packets_seen.store(true);
            if (packets_received == 1)
            {
                __android_log_print(ANDROID_LOG_INFO, kAndroidApfpvLogTag, "udp_first_packet size=%zd", received);
            }

            {
                bool dropped_packet = false;
                {
                    std::lock_guard<std::mutex> lock(queued_packet_mutex);
                    if (queued_packets.size() >= kAndroidApfpvMaxQueuedPackets)
                    {
                        dropped_packet = true;
                        dropped_queued_packets.fetch_add(1);
                    }
                    else
                    {
                        queued_packets.emplace_back(rx_buffer.begin(), rx_buffer.begin() + received);
                    }
                }

                if (!dropped_packet)
                {
                    queued_packet_cv.notify_one();
                }
            }
        }
        else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            setUdpError(std::string("recv failed: ") + std::strerror(errno));
            __android_log_print(ANDROID_LOG_ERROR, kAndroidApfpvLogTag, "udp_recv failed: %s", udpLastError().c_str());
            break;
        }

        const auto stats_now = ClockType::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(stats_now - stats_window_start).count();
        if (elapsed_ms >= 1000)
        {
            const uint64_t frame_count_now = callbacks.submittedFrameCount ? callbacks.submittedFrameCount() : frame_count_start;
            const float throughput_mbps = static_cast<float>(bytes_window * 8.0) /
                                          static_cast<float>(elapsed_ms * 1000.0);
            const int received_completed_frames = static_cast<int>(
                ((frame_count_now - frame_count_start) * 1000ULL) /
                static_cast<uint64_t>(elapsed_ms));
            updateUdpStats(packets_received, throughput_mbps, received_completed_frames, 0);

            bytes_window = 0;
            frame_count_start = frame_count_now;
            stats_window_start = stats_now;
        }
    }

    packet_worker_exit.store(true);
    queued_packet_cv.notify_all();
    if (packet_worker_thread.joinable())
    {
        packet_worker_thread.join();
    }

    close(socket_fd);
    freeaddrinfo(addrinfo_result);
    setUdpRunning(false);
    if (!udpLastError().empty())
    {
        __android_log_print(ANDROID_LOG_WARN, kAndroidApfpvLogTag, "udp_stop error=%s", udpLastError().c_str());
    }
}
