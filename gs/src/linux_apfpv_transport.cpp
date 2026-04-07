#include "linux_apfpv_transport.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <set>
#include <sys/socket.h>
#include <unistd.h>

#include "Log.h"
#include "fmt/core.h"
#include "gs_runtime_core.h"
#include "gs_runtime_state.h"
#include "gs_stats.h"

namespace
{

constexpr uint16_t kApfpvUdpPort = 5600;
constexpr const char* kApfpvPeerIp = "192.168.4.1";
constexpr int kApfpvSocketTimeoutUs = 250000;
constexpr int kApfpvSocketSendBufferSize = 8 * 1024;

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
// Runs a shell command for APFPV interface setup and logs failures without aborting activation.
void runSetupCommand(const std::string& command)
{
    const int result = system(command.c_str());
    if (result != 0)
    {
        LOGW("Linux APFPV setup command failed ({}): {}", result, command);
    }
}

//===================================================================================
//===================================================================================
// Switches Linux Wi-Fi interfaces back to managed mode for APFPV camera connections.
void setManagedMode(const std::vector<std::string>& interfaces)
{
    for (const std::string& interface : interfaces)
    {
        LOGI("Setting managed mode on {}", interface);
        runSetupCommand(fmt::format("sudo ip link set {} down", interface));
        runSetupCommand(fmt::format("sudo iw dev {} set type managed", interface));
        runSetupCommand(fmt::format("sudo ip link set {} up", interface));
    }
}

}

//===================================================================================
//===================================================================================
// Initializes the Linux APFPV transport runtime state and FEC helpers.
LinuxApfpvTransport::LinuxApfpvTransport()
{
    m_tx_fec = fec_new(2, 3);

    FecBlockDecoder::Descriptor decoder_descriptor = {};
    decoder_descriptor.coding_k = FEC_K;
    decoder_descriptor.coding_n = FEC_N;
    decoder_descriptor.mtu = AIR2GROUND_MAX_MTU;
    decoder_descriptor.reset_duration = std::chrono::milliseconds(0);
    decoder_descriptor.restart_backjump_blocks = 64;
    decoder_descriptor.max_block_queue_size = 3;
    decoder_descriptor.duplicate_window = 100;
    decoder_descriptor.interface_count = 1;
    m_rx_decoder.init(decoder_descriptor);
}

//===================================================================================
//===================================================================================
// Stops the Linux APFPV backend thread and releases FEC resources.
LinuxApfpvTransport::~LinuxApfpvTransport()
{
    stopBackend();

    if (m_tx_fec != nullptr)
    {
        fec_free(m_tx_fec);
        m_tx_fec = nullptr;
    }
}

//===================================================================================
//===================================================================================
// Stores descriptors and prepares the Linux APFPV UDP backend for activation.
bool LinuxApfpvTransport::init(const gs::core::RXDescriptor& rx_descriptor,
                               const gs::core::TXDescriptor& tx_descriptor)
{
    storeDescriptors(rx_descriptor, tx_descriptor);
    ensureRxDecoderConfig();
    reset_rx_state();
    return true;
}

//===================================================================================
//===================================================================================
// Activates APFPV mode by switching the configured Linux interfaces to managed mode.
void LinuxApfpvTransport::activate()
{
    setManagedMode(buildApfpvInterfaces(m_rx_descriptor, m_tx_descriptor));
    startBackend();
}

//===================================================================================
//===================================================================================
// Processes the APFPV decoder state and refreshes derived throughput statistics.
void LinuxApfpvTransport::process()
{
    ensureRxDecoderConfig();
    m_rx_decoder.process(Clock::now());
    syncRxDecoderStats();

    const Clock::time_point now = Clock::now();
    if (now - m_data_stats_last_tp >= std::chrono::seconds(1))
    {
        const float seconds = std::chrono::duration<float>(now - m_data_stats_last_tp).count();
        if (seconds > 0.0f)
        {
            m_data_stats_rate = static_cast<size_t>(static_cast<float>(m_data_stats_data_accumulated) / seconds);
        }
        else
        {
            m_data_stats_rate = 0;
        }
        m_data_stats_data_accumulated = 0;
        m_data_stats_last_tp = now;
    }
}

//===================================================================================
//===================================================================================
// Resets APFPV decoder, counters, and partially assembled transmit block state.
void LinuxApfpvTransport::reset_rx_state()
{
    m_rx_decoder.reset(Clock::now());
    m_has_first_tx_payload = false;
    m_next_tx_block_index = 1;
    m_data_stats_rate = 0;
    m_data_stats_data_accumulated = 0;
    m_last_rx_decoded_bytes_total = 0;
    m_data_stats_last_tp = Clock::now();
    syncRxDecoderStats();
}

//===================================================================================
//===================================================================================
// Encodes one APFPV control payload into transport packets and sends them over UDP.
void LinuxApfpvTransport::send(const void* data, size_t size, bool /* flush */)
{
    if (data == nullptr || size == 0)
    {
        return;
    }

    std::array<uint8_t, GROUND2AIR_MAX_MTU> current_payload = {};
    const size_t copy_size = std::min(size, current_payload.size());
    std::memcpy(current_payload.data(), data, copy_size);

    if (!m_has_first_tx_payload)
    {
        m_first_tx_payload = current_payload;
        m_has_first_tx_payload = true;
        sendTransportPacket(current_payload.data(), current_payload.size(), 0);
        return;
    }

    sendTransportPacket(current_payload.data(), current_payload.size(), 1);

    if (m_tx_fec != nullptr)
    {
        std::array<uint8_t, GROUND2AIR_MAX_MTU> fec_payload = {};
        const gf* src_ptrs[2] = {
            m_first_tx_payload.data(),
            current_payload.data()
        };
        gf* fec_ptrs[1] = {
            fec_payload.data()
        };
        fec_encode(m_tx_fec,
                   src_ptrs,
                   fec_ptrs,
                   fec_block_nums() + 2,
                   1,
                   current_payload.size());
        sendTransportPacket(fec_payload.data(), fec_payload.size(), 2);
    }

    m_has_first_tx_payload = false;
    m_next_tx_block_index++;
}

//===================================================================================
//===================================================================================
// Returns the next decoded APFPV session payload if the FEC decoder has one ready.
bool LinuxApfpvTransport::receive(void* data, size_t& size, bool& restoredByFEC)
{
    return m_rx_decoder.receive(data, size, restoredByFEC);
}

//===================================================================================
//===================================================================================
// Returns the latest decoded APFPV throughput estimate in bytes per second.
size_t LinuxApfpvTransport::get_data_rate() const
{
    return m_data_stats_rate;
}

//===================================================================================
//===================================================================================
// Returns the latest APFPV RSSI estimate, which is unavailable for Linux UDP transport.
int LinuxApfpvTransport::get_input_dBm() const
{
    return m_input_dbm;
}

//===================================================================================
//===================================================================================
// Reconfigures the APFPV receive decoder when the negotiated transport settings change.
void LinuxApfpvTransport::ensureRxDecoderConfig()
{
    const uint8_t config_k = s_runtimeCore.config_packet.dataChannel.fec_codec_k;
    const uint8_t config_n = s_runtimeCore.config_packet.dataChannel.fec_codec_n;
    const uint16_t config_mtu = s_runtimeCore.config_packet.dataChannel.fec_codec_mtu;

    const uint8_t effective_k = config_k > 0 ? config_k : static_cast<uint8_t>(FEC_K);
    const uint8_t effective_n = config_n > 0 ? config_n : static_cast<uint8_t>(FEC_N);
    const uint16_t effective_mtu = config_mtu > 0 ? config_mtu : static_cast<uint16_t>(AIR2GROUND_MAX_MTU);

    const FecBlockDecoder::Stats stats_before = m_rx_decoder.getStats();
    if (s_runtimeCore.rx_decoder_k == effective_k &&
        s_runtimeCore.rx_decoder_n == effective_n &&
        s_runtimeCore.rx_decoder_mtu == effective_mtu)
    {
        return;
    }

    FecBlockDecoder::Descriptor decoder_descriptor = {};
    decoder_descriptor.coding_k = effective_k;
    decoder_descriptor.coding_n = effective_n;
    decoder_descriptor.mtu = effective_mtu;
    decoder_descriptor.reset_duration = std::chrono::milliseconds(0);
    decoder_descriptor.restart_backjump_blocks = 64;
    decoder_descriptor.max_block_queue_size = 3;
    decoder_descriptor.duplicate_window = 100;
    decoder_descriptor.interface_count = 1;

    s_runtimeCore.rx_decoder_k = effective_k;
    s_runtimeCore.rx_decoder_n = effective_n;
    s_runtimeCore.rx_decoder_mtu = effective_mtu;
    m_rx_decoder.init(decoder_descriptor);
    m_last_rx_decoded_bytes_total = stats_before.decoded_bytes_total;
    syncRxDecoderStats();
}

//===================================================================================
//===================================================================================
// Copies APFPV decoder stats into the shared GS counters used by the Linux overlay.
void LinuxApfpvTransport::syncRxDecoderStats()
{
    const FecBlockDecoder::Stats stats = m_rx_decoder.getStats();
    {
        std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
        s_gs_stats.lastPacketIndex = stats.last_packet_index;
        s_gs_stats.inUniquePacketCounter = static_cast<uint16_t>(stats.unique_packet_count);
        s_gs_stats.inDublicatedPacketCounter = static_cast<uint16_t>(stats.duplicate_packet_count);
        s_gs_stats.FECBlocksCounter = stats.fec_blocks_count;
        s_gs_stats.FECSuccPacketIndexCounter = stats.fec_success_packet_index_sum;
    }

    if (stats.decoded_bytes_total >= m_last_rx_decoded_bytes_total)
    {
        m_data_stats_data_accumulated +=
            static_cast<size_t>(stats.decoded_bytes_total - m_last_rx_decoded_bytes_total);
    }
    m_last_rx_decoded_bytes_total = stats.decoded_bytes_total;
}

//===================================================================================
//===================================================================================
// Starts the Linux APFPV UDP receive backend thread if it is not already running.
bool LinuxApfpvTransport::startBackend()
{
    stopBackend();

    m_exit_requested = false;
    if (!openSocket())
    {
        return false;
    }

    m_rx_thread = std::thread([this]()
    {
        rxThreadProc();
    });
    m_backend_running = true;
    return true;
}

//===================================================================================
//===================================================================================
// Stops the Linux APFPV UDP backend thread and closes its socket.
void LinuxApfpvTransport::stopBackend()
{
    m_exit_requested = true;
    closeSocket();

    if (m_rx_thread.joinable())
    {
        m_rx_thread.join();
    }

    m_backend_running = false;
}

//===================================================================================
//===================================================================================
// Receives APFPV UDP transport packets, filters them, and pushes them into the FEC decoder.
void LinuxApfpvTransport::rxThreadProc()
{
    std::array<uint8_t, sizeof(Packet_Header) + AIR2GROUND_MAX_MTU> buffer = {};

    while (!m_exit_requested.load())
    {
        sockaddr_in from_addr = {};
        socklen_t from_len = sizeof(from_addr);
        int socket_fd = -1;
        {
            std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
            socket_fd = m_socket_fd;
        }

        if (socket_fd < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        const int len = recvfrom(socket_fd,
                                 reinterpret_cast<char*>(buffer.data()),
                                 static_cast<int>(buffer.size()),
                                 0,
                                 reinterpret_cast<sockaddr*>(&from_addr),
                                 &from_len);
        if (len <= 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                continue;
            }

            if (!m_exit_requested.load())
            {
                LOGW("Linux APFPV recvfrom failed: {}", std::strerror(errno));
            }
            continue;
        }

        const PacketFilter::PacketFilterResult filter_result =
            m_packet_filter.filter_packet(buffer.data(), static_cast<size_t>(len), GROUND2AIR_MAX_MTU);
        if (filter_result != PacketFilter::PacketFilterResult::Pass)
        {
            if (filter_result == PacketFilter::PacketFilterResult::WrongVersion)
            {
                s_incompatibleFirmwareTime = Clock::now();
            }
            continue;
        }

        if (static_cast<size_t>(len) >= sizeof(Packet_Header))
        {
            const auto* header = reinterpret_cast<const Packet_Header*>(buffer.data());
            {
                std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
                s_gs_stats.inPacketCounter[0]++;
            }
            s_runtimeCore.transport_packets_seen++;
            s_runtimeCore.transport_packets_passed_filter++;
            s_runtimeCore.last_transport_block = header->block_index;
            s_runtimeCore.last_transport_packet_index = header->packet_index;
            s_runtimeCore.last_transport_payload_size = header->size;
            s_runtimeCore.last_transport_from = header->fromDeviceId;
            s_runtimeCore.last_transport_to = header->toDeviceId;
        }
        else
        {
            s_runtimeCore.transport_packets_filtered++;
            continue;
        }

        m_rx_decoder.pushPacket(buffer.data(), static_cast<size_t>(len), 0, Clock::now());
        syncRxDecoderStats();
    }
}

//===================================================================================
//===================================================================================
// Opens and binds the Linux APFPV UDP socket on the shared APFPV port.
bool LinuxApfpvTransport::openSocket()
{
    std::lock_guard<std::mutex> socket_lock(m_socket_mutex);

    closeSocket();

    m_socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket_fd < 0)
    {
        LOGE("Linux APFPV socket create failed: {}", std::strerror(errno));
        return false;
    }

    timeval timeout = {};
    timeout.tv_sec = 0;
    timeout.tv_usec = kApfpvSocketTimeoutUs;
    setsockopt(m_socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(m_socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    int send_buffer_size = kApfpvSocketSendBufferSize;
    setsockopt(m_socket_fd, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size));

    sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(kApfpvUdpPort);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(m_socket_fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0)
    {
        LOGE("Linux APFPV bind failed: {}", std::strerror(errno));
        closeSocket();
        return false;
    }

    std::memset(&m_peer_addr, 0, sizeof(m_peer_addr));
    m_peer_addr.sin_family = AF_INET;
    m_peer_addr.sin_port = htons(kApfpvUdpPort);
    m_peer_addr.sin_addr.s_addr = inet_addr(kApfpvPeerIp);
    LOGI("Linux APFPV UDP socket ready on port {}", kApfpvUdpPort);
    return true;
}

//===================================================================================
//===================================================================================
// Closes the Linux APFPV UDP socket if it is currently open.
void LinuxApfpvTransport::closeSocket()
{
    if (m_socket_fd >= 0)
    {
        ::close(m_socket_fd);
        m_socket_fd = -1;
    }
}

//===================================================================================
//===================================================================================
// Wraps one payload into an APFPV transport packet and sends it to the fixed camera endpoint.
void LinuxApfpvTransport::sendTransportPacket(const uint8_t* payload, size_t payload_size, uint8_t packet_index)
{
    std::array<uint8_t, sizeof(Packet_Header) + GROUND2AIR_MAX_MTU> packet = {};
    auto* header = reinterpret_cast<Packet_Header*>(packet.data());
    m_packet_filter.apply_packet_header_data(header);
    header->size = static_cast<uint16_t>(GROUND2AIR_MAX_MTU);
    header->block_index = m_next_tx_block_index;
    header->packet_index = packet_index;

    const size_t bounded_size = std::min(payload_size, static_cast<size_t>(GROUND2AIR_MAX_MTU));
    std::memcpy(packet.data() + sizeof(Packet_Header), payload, bounded_size);

    int socket_fd = -1;
    sockaddr_in peer_addr = {};
    {
        std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
        socket_fd = m_socket_fd;
        peer_addr = m_peer_addr;
    }

    if (socket_fd < 0)
    {
        return;
    }

    const ssize_t sent = sendto(socket_fd,
                                packet.data(),
                                packet.size(),
                                MSG_DONTWAIT,
                                reinterpret_cast<const sockaddr*>(&peer_addr),
                                sizeof(peer_addr));
    if (sent > 0)
    {
        std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
        s_gs_stats.outPacketCounter++;
        return;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
    {
        LOGW("Linux APFPV sendto failed: {}", std::strerror(errno));
    }
}
