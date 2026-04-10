#include "android_apfpv_transport.h"
#include "../../../../../components/common/Clock.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "gs_runtime_state.h"

namespace
{

constexpr const char* kAndroidApfpvLogTag = "AndroidAPFPVTransport";

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
// Performs no background work because the UDP client is driven by the transport thread.
void AndroidAPFPVTransport::process()
{
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
void AndroidAPFPVTransport::updateUdpStats(uint64_t packets_received, float throughput_mbps, float video_fps)
{
    m_udp_packets_received = packets_received;
    m_udp_throughput_mbps = throughput_mbps;
    m_udp_video_fps = video_fps;
    if (packets_received > 0)
    {
        setLinkState(LinkState::None);
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
// Returns the latest measured UDP throughput for the Android APFPV backend.
float AndroidAPFPVTransport::udpThroughputMbps() const
{
    return m_udp_throughput_mbps;
}

//===================================================================================
//===================================================================================
// Returns the latest measured video FPS for the Android APFPV backend.
float AndroidAPFPVTransport::udpVideoFps() const
{
    return m_udp_video_fps;
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
    updateUdpStats(0, 0.0f, 0.0f);
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
                    sendto(socket_fd,
                           packet.data(),
                           packet.size(),
                           0,
                           addrinfo_result->ai_addr,
                           static_cast<socklen_t>(addrinfo_result->ai_addrlen));
                }
            }
            next_control_send = now + std::chrono::milliseconds(250);
        }

        ssize_t received = recvfrom(socket_fd, rx_buffer.data(), rx_buffer.size(), 0, nullptr, nullptr);
        if (received > 0)
        {
            packets_received++;
            bytes_window += static_cast<uint64_t>(received);
            if (packets_received == 1)
            {
                __android_log_print(ANDROID_LOG_INFO, kAndroidApfpvLogTag, "udp_first_packet size=%zd", received);
            }
            if (callbacks.processTransportPacket)
            {
                callbacks.processTransportPacket(rx_buffer.data(), static_cast<size_t>(received));
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
            const float video_fps = static_cast<float>((frame_count_now - frame_count_start) * 1000.0) /
                                    static_cast<float>(elapsed_ms);
            updateUdpStats(packets_received, throughput_mbps, video_fps);

            bytes_window = 0;
            frame_count_start = frame_count_now;
            stats_window_start = stats_now;
        }
    }

    close(socket_fd);
    freeaddrinfo(addrinfo_result);
    setUdpRunning(false);
    if (!udpLastError().empty())
    {
        __android_log_print(ANDROID_LOG_WARN, kAndroidApfpvLogTag, "udp_stop error=%s", udpLastError().c_str());
    }
}
