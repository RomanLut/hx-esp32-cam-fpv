#include "android_raw_broadcast_transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>

#include <android/log.h>
#include <libusb.h>

#include "fec.h"
#include "gs_shared_state.h"
#include "structures.h"
#include "third_party/devourer/src/FrameParser.h"
#include "third_party/devourer/src/SelectedChannel.h"
#include "third_party/devourer/src/ieee80211_radiotap.h"

namespace
{

constexpr const char* kAndroidRawBroadcastLogTag = "AndroidRawBroadcast";
constexpr size_t kTxRateHz = 26000000;
constexpr uint32_t kRxRestartBackjumpBlocks = 64;

//===================================================================================
//===================================================================================
// Appends one byte to the radiotap builder buffer while tracking logical alignment.
void radiotapAddU8(uint8_t*& dst, size_t& idx, uint8_t data)
{
    *dst++ = data;
    idx++;
}

//===================================================================================
//===================================================================================
// Appends one little-endian u16 to the radiotap builder buffer with alignment padding.
void radiotapAddU16(uint8_t*& dst, size_t& idx, uint16_t data)
{
    if ((idx & 1U) == 1U)
    {
        radiotapAddU8(dst, idx, 0);
    }

    *reinterpret_cast<uint16_t*>(dst) = data;
    dst += 2;
    idx += 2;
}

//===================================================================================
//===================================================================================
// Returns whether one 802.11 header matches the legacy raw-broadcast MAC signature.
bool matchesLegacyAir2GroundMacSignature(const uint8_t* ieee_header, size_t ieee_header_size)
{
    if (ieee_header == nullptr || ieee_header_size < 16)
    {
        return false;
    }

    return ieee_header[10] == 0x11 &&
           ieee_header[11] == 0x22 &&
           ieee_header[12] == 0x33 &&
           ieee_header[13] == 0x44 &&
           ieee_header[14] == 0x55 &&
           ieee_header[15] == 0x66;
}

//===================================================================================
//===================================================================================
// Converts one GS channel number into the RTL driver channel selection descriptor.
SelectedChannel makeSelectedChannel(int channel)
{
    SelectedChannel selected_channel = {};
    selected_channel.Channel = static_cast<uint8_t>(std::clamp(channel, 1, 165));
    selected_channel.ChannelOffset = 0;
    selected_channel.ChannelWidth = CHANNEL_WIDTH_20;
    return selected_channel;
}

//===================================================================================
//===================================================================================
// Fills the transport header fields for one fixed-size raw-broadcast packet.
void sealPacket(PacketFilter& packet_filter,
                std::vector<uint8_t>& packet,
                size_t packet_header_offset,
                uint32_t block_index,
                uint8_t packet_index)
{
    Packet_Header* header =
        reinterpret_cast<Packet_Header*>(packet.data() + packet_header_offset);
    packet_filter.apply_packet_header_data(header);
    header->size = static_cast<uint16_t>(packet.size() - packet_header_offset - sizeof(Packet_Header));
    header->block_index = block_index;
    header->packet_index = packet_index;
}

}

//===================================================================================
//===================================================================================
// Releases Android raw-broadcast driver resources and the local FEC encoder state.
AndroidRawBroadcastTransport::~AndroidRawBroadcastTransport()
{
    stopUsbAdapter();
    if (m_tx_fec != nullptr)
    {
        fec_free(m_tx_fec);
        m_tx_fec = nullptr;
    }
}

//===================================================================================
//===================================================================================
// Stores descriptors and initializes the devourer Wi-Fi driver wrapper.
bool AndroidRawBroadcastTransport::init(const gs::core::RXDescriptor& rx_descriptor,
                                        const gs::core::TXDescriptor& tx_descriptor)
{
    storeDescriptors(rx_descriptor, tx_descriptor);
    if (m_tx_descriptor.coding_k == 0 ||
        m_tx_descriptor.coding_n < m_tx_descriptor.coding_k)
    {
        __android_log_print(ANDROID_LOG_ERROR,
                            kAndroidRawBroadcastLogTag,
                            "Invalid TX coding params k=%u n=%u",
                            static_cast<unsigned int>(m_tx_descriptor.coding_k),
                            static_cast<unsigned int>(m_tx_descriptor.coding_n));
        return false;
    }

    m_devourer_logger = std::make_shared<Logger>();
    m_wifi_driver = std::make_unique<WiFiDriver>(m_devourer_logger);
    if (m_tx_fec != nullptr)
    {
        fec_free(m_tx_fec);
        m_tx_fec = nullptr;
    }
    m_tx_fec = fec_new(m_tx_descriptor.coding_k, m_tx_descriptor.coding_n);
    if (m_tx_fec == nullptr)
    {
        __android_log_print(ANDROID_LOG_ERROR,
                            kAndroidRawBroadcastLogTag,
                            "fec_new failed k=%u n=%u",
                            static_cast<unsigned int>(m_tx_descriptor.coding_k),
                            static_cast<unsigned int>(m_tx_descriptor.coding_n));
        return false;
    }

    buildRadiotapHeaderLocked();
    m_packet_header_offset = m_radiotap_header.size() + WLAN_IEEE_HEADER_SIZE;
    m_payload_offset = m_packet_header_offset + sizeof(Packet_Header);
    m_transport_packet_size = m_payload_offset + m_tx_descriptor.mtu;
    resetTxAssemblerLocked();

    FecBlockDecoder::Descriptor decoder_descriptor = {};
    decoder_descriptor.coding_k = m_rx_descriptor.coding_k;
    decoder_descriptor.coding_n = m_rx_descriptor.coding_n;
    decoder_descriptor.mtu = static_cast<uint16_t>(m_rx_descriptor.mtu);
    decoder_descriptor.reset_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(m_rx_descriptor.reset_duration);
    decoder_descriptor.restart_backjump_blocks = kRxRestartBackjumpBlocks;
    decoder_descriptor.max_block_queue_size = 3;
    decoder_descriptor.duplicate_window = 100;
    decoder_descriptor.interface_count = 1;
    if (!m_rx_decoder.init(decoder_descriptor))
    {
        __android_log_print(ANDROID_LOG_ERROR,
                            kAndroidRawBroadcastLogTag,
                            "RX decoder init failed k=%u n=%u mtu=%u",
                            static_cast<unsigned int>(decoder_descriptor.coding_k),
                            static_cast<unsigned int>(decoder_descriptor.coding_n),
                            static_cast<unsigned int>(decoder_descriptor.mtu));
        fec_free(m_tx_fec);
        m_tx_fec = nullptr;
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO,
                        kAndroidRawBroadcastLogTag,
                        "Initialized raw transport channel=%d mtu=%zu tx_k=%u tx_n=%u rx_k=%u rx_n=%u",
                        s_groundstation_config.wifi_channel,
                        m_tx_descriptor.mtu,
                        static_cast<unsigned int>(m_tx_descriptor.coding_k),
                        static_cast<unsigned int>(m_tx_descriptor.coding_n),
                        static_cast<unsigned int>(m_rx_descriptor.coding_k),
                        static_cast<unsigned int>(m_rx_descriptor.coding_n));

    return true;
}

//===================================================================================
//===================================================================================
// Activates Android raw-broadcast mode and updates the shared link-state banner.
void AndroidRawBroadcastTransport::activate()
{
    m_activate_time = Clock::now();
    m_active = true;
}

//===================================================================================
//===================================================================================
// Deactivates Android raw-broadcast mode and signals the USB adapter to stop.
// Does NOT join the rx thread — deactivate() is called from the OSD menu render path
// which holds handle->mutex, and the rx thread callback also acquires handle->mutex.
// Joining here would deadlock. The actual thread join and libusb teardown are deferred
// to stopUsbAdapter(), which is called from Java (without handle->mutex held) or the destructor.
void AndroidRawBroadcastTransport::deactivate()
{
    m_active = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_transport_packet_callback = nullptr;
        if (m_device)
        {
            m_device->should_stop = true;
        }
    }
}

//===================================================================================
//===================================================================================
// Reapplies the configured raw-broadcast channel on immediate reconnect requests.
bool AndroidRawBroadcastTransport::requestImmediateReconnect()
{
    setChannel(s_groundstation_config.wifi_channel);
    return true;
}

//===================================================================================
//===================================================================================
// Reports that Android raw-broadcast uses the shared Wi-Fi channel search flow.
bool AndroidRawBroadcastTransport::usesChannelSearch() const
{
    return true;
}

//===================================================================================
//===================================================================================
// Reports that Android raw-broadcast does not provide custom menu search hooks.
bool AndroidRawBroadcastTransport::supportsMenuSearchOrConnect() const
{
    return false;
}

//===================================================================================
//===================================================================================
// Updates transport data-rate and latched RSSI statistics from the queued RX path.
void AndroidRawBroadcastTransport::process()
{
    m_rx_decoder.process(Clock::now());
    const FecBlockDecoder::Stats stats = m_rx_decoder.getStats();
    if (stats.decoded_bytes_total >= m_last_rx_decoded_bytes_total)
    {
        m_data_stats_data_accumulated +=
            static_cast<size_t>(stats.decoded_bytes_total - m_last_rx_decoded_bytes_total);
    }
    m_last_rx_decoded_bytes_total = stats.decoded_bytes_total;

    const int best_input_dbm = m_best_input_dbm.exchange(std::numeric_limits<int>::lowest());
    if (best_input_dbm != std::numeric_limits<int>::lowest())
    {
        m_latched_input_dbm.store(best_input_dbm);
    }

    const Clock::time_point last_rx = Clock::time_point(Clock::duration(m_last_rx_packet_tp.load()));
    if (Clock::now() - last_rx > std::chrono::seconds(2))
    {
        m_latched_input_dbm.store(0);
    }

    const Clock::time_point now = Clock::now();
    if (now - m_data_stats_last_tp >= std::chrono::seconds(1))
    {
        const float elapsed_seconds = std::chrono::duration<float>(now - m_data_stats_last_tp).count();
        m_data_stats_rate = elapsed_seconds > 0.0f
            ? static_cast<size_t>(static_cast<float>(m_data_stats_data_accumulated) / elapsed_seconds)
            : 0;
        m_data_stats_data_accumulated = 0;
        m_data_stats_last_tp = now;
    }
}

//===================================================================================
//===================================================================================
// Clears queued Android RX session packets and resets transport statistics.
void AndroidRawBroadcastTransport::reset_rx_state()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    resetTxAssemblerLocked();
    m_rx_decoder.reset(Clock::now());
    m_data_stats_rate = 0;
    m_data_stats_data_accumulated = 0;
    m_last_rx_decoded_bytes_total = 0;
    m_data_stats_last_tp = Clock::now();
}

//===================================================================================
//===================================================================================
// Sends one GS session payload as a single raw-broadcast transport packet.
void AndroidRawBroadcastTransport::send(const void* data, size_t size, bool /* flush */)
{
    std::shared_ptr<Rtl8812aDevice> device;
    std::vector<std::vector<uint8_t>> packets_to_send;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        device = m_device;
        if (!device || data == nullptr || size == 0)
        {
            return;
        }

        const uint8_t* read_ptr = reinterpret_cast<const uint8_t*>(data);
        size_t remaining = size;
        while (remaining > 0)
        {
            const size_t chunk_size =
                std::min(remaining, m_transport_packet_size - m_tx_current_packet.size());
            const size_t old_size = m_tx_current_packet.size();
            m_tx_current_packet.resize(old_size + chunk_size);
            std::memcpy(m_tx_current_packet.data() + old_size, read_ptr, chunk_size);
            read_ptr += chunk_size;
            remaining -= chunk_size;

            if (m_tx_current_packet.size() < m_transport_packet_size)
            {
                m_tx_current_packet.resize(m_transport_packet_size);
            }

            sealPacket(m_packet_filter,
                       m_tx_current_packet,
                       m_packet_header_offset,
                       m_next_block_index,
                       static_cast<uint8_t>(m_tx_block_packets.size()));
            packets_to_send.push_back(m_tx_current_packet);
            m_tx_block_packets.push_back(m_tx_current_packet);
            resetTxAssemblerLocked();

            if (m_tx_block_packets.size() >= m_tx_descriptor.coding_k)
            {
                std::vector<const gf*> fec_src_packet_ptrs(m_tx_descriptor.coding_k);
                std::vector<gf*> fec_dst_packet_ptrs(m_tx_descriptor.coding_n - m_tx_descriptor.coding_k);
                for (size_t i = 0; i < m_tx_descriptor.coding_k; ++i)
                {
                    fec_src_packet_ptrs[i] =
                        reinterpret_cast<const gf*>(m_tx_block_packets[i].data() + m_payload_offset);
                }

                for (size_t i = 0; i < m_tx_descriptor.coding_n - m_tx_descriptor.coding_k; ++i)
                {
                    packets_to_send.emplace_back(m_transport_packet_size);
                    std::vector<uint8_t>& fec_packet = packets_to_send.back();
                    std::memcpy(fec_packet.data(), m_radiotap_header.data(), m_radiotap_header.size());
                    std::memcpy(fec_packet.data() + m_radiotap_header.size(),
                                WLAN_IEEE_HEADER_GROUND2AIR,
                                WLAN_IEEE_HEADER_SIZE);
                    fec_dst_packet_ptrs[i] =
                        reinterpret_cast<gf*>(fec_packet.data() + m_payload_offset);
                }

                fec_encode(m_tx_fec,
                           fec_src_packet_ptrs.data(),
                           fec_dst_packet_ptrs.data(),
                           fec_block_nums() + m_tx_descriptor.coding_k,
                           m_tx_descriptor.coding_n - m_tx_descriptor.coding_k,
                           m_tx_descriptor.mtu);

                const size_t fec_start_index = packets_to_send.size() -
                    (m_tx_descriptor.coding_n - m_tx_descriptor.coding_k);
                for (size_t i = 0; i < m_tx_descriptor.coding_n - m_tx_descriptor.coding_k; ++i)
                {
                    sealPacket(m_packet_filter,
                               packets_to_send[fec_start_index + i],
                               m_packet_header_offset,
                               m_next_block_index,
                               static_cast<uint8_t>(m_tx_descriptor.coding_k + i));
                }

                m_tx_block_packets.clear();
                m_next_block_index++;
            }
        }
    }

    for (const std::vector<uint8_t>& packet : packets_to_send)
    {
        sendRawPacket(device, packet);
    }
}

//===================================================================================
//===================================================================================
// Pops one already-filtered session payload received from the Android driver callback.
bool AndroidRawBroadcastTransport::receive(void* data, size_t& size, bool& restoredByFEC)
{
    return m_rx_decoder.receive(data, size, restoredByFEC);
}

//===================================================================================
//===================================================================================
// Retunes the active RTL adapter to the requested monitor-mode channel when running.
void AndroidRawBroadcastTransport::setChannel(int ch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_device)
    {
        __android_log_print(ANDROID_LOG_INFO,
                            kAndroidRawBroadcastLogTag,
                            "Ignoring channel change to %d because no adapter is running",
                            ch);
        return;
    }

    __android_log_print(ANDROID_LOG_INFO,
                        kAndroidRawBroadcastLogTag,
                        "Setting monitor channel to %d",
                        ch);
    m_device->SetMonitorChannel(makeSelectedChannel(ch));
}

//===================================================================================
//===================================================================================
// Returns and resets the count of all raw frames seen (including non-matching MACs).
uint32_t AndroidRawBroadcastTransport::consumeAllFrameCount()
{
    return m_all_frame_count.exchange(0);
}

//===================================================================================
//===================================================================================
// Returns and resets the count of frames that passed the MAC + packet filter.
uint32_t AndroidRawBroadcastTransport::consumeFilteredFrameCount()
{
    return m_filtered_frame_count.exchange(0);
}

//===================================================================================
//===================================================================================
// Sets the TX power level on the active RTL adapter (0 = driver default, 1..63 = dBm scale).
void AndroidRawBroadcastTransport::setTxPower(int txPower)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tx_power = static_cast<uint8_t>(std::clamp(txPower, 0, 63));
    if (m_device)
    {
        __android_log_print(ANDROID_LOG_INFO,
                            kAndroidRawBroadcastLogTag,
                            "Setting TX power to %d",
                            m_tx_power);
        m_device->SetTxPower(m_tx_power);
    }
}

//===================================================================================
//===================================================================================
// Returns the current Android raw-broadcast receive throughput estimate in bytes/s.
size_t AndroidRawBroadcastTransport::get_data_rate() const
{
    return m_data_stats_rate;
}

//===================================================================================
//===================================================================================
// Returns the most recently latched input RSSI estimate from received packets.
int AndroidRawBroadcastTransport::get_input_dBm() const
{
    return m_latched_input_dbm.load();
}

//===================================================================================
//===================================================================================
// Contributes raw-broadcast RSSI and packet counters into the shared GS stats window.
void AndroidRawBroadcastTransport::contributeGroundStats(GSStats& stats)
{
    const int input_dbm = get_input_dBm();
    if (input_dbm != 0)
    {
        stats.rssiDbm[0] = static_cast<int8_t>(std::clamp(input_dbm, -127, 0));
    }
    stats.rssiDbm[1] = 0;
    stats.noiseFloorDbm = 0;
    const uint32_t all_frames = consumeAllFrameCount();
    const uint32_t filtered_frames = consumeFilteredFrameCount();
    // Filtered frames are already counted per-packet by processTransportPacket via the callback.
    // Only add the frames that did NOT pass the filter (unmatched MAC / packet filter rejects)
    // so that inPacketCounterAll reflects total frames seen by the radio without double-counting.
    stats.inPacketCounterAll[0] += static_cast<uint16_t>(all_frames - filtered_frames);
}

//===================================================================================
//===================================================================================
// Returns a status message for the top overlay when the adapter is missing while active.
std::string AndroidRawBroadcastTransport::getTransportMessage() const
{
    if (m_active && !isUsbAdapterRunning())
    {
        using namespace std::chrono_literals;
        if (Clock::now() - m_activate_time >= 3s)
        {
            return "RTL8812AU USB ADAPTER NOT FOUND!";
        }
    }
    return {};
}

//===================================================================================
//===================================================================================
// Starts the Android RTL8812AU adapter from one already-open USB file descriptor.
bool AndroidRawBroadcastTransport::startUsbAdapter(int fd)
{
    if (fd < 0)
    {
        __android_log_print(ANDROID_LOG_ERROR,
                            kAndroidRawBroadcastLogTag,
                            "Refusing to start adapter with invalid fd=%d",
                            fd);
        return false;
    }

    stopUsbAdapter();

    libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    if (libusb_init(&m_libusb_context) < 0)
    {
        __android_log_print(ANDROID_LOG_ERROR,
                            kAndroidRawBroadcastLogTag,
                            "libusb_init failed");
        m_libusb_context = nullptr;
        return false;
    }

    if (libusb_wrap_sys_device(m_libusb_context, static_cast<intptr_t>(fd), &m_usb_handle) < 0)
    {
        __android_log_print(ANDROID_LOG_ERROR,
                            kAndroidRawBroadcastLogTag,
                            "libusb_wrap_sys_device failed fd=%d",
                            fd);
        libusb_exit(m_libusb_context);
        m_libusb_context = nullptr;
        m_usb_handle = nullptr;
        return false;
    }

    if (libusb_kernel_driver_active(m_usb_handle, 0) == 1)
    {
        libusb_detach_kernel_driver(m_usb_handle, 0);
    }

    if (libusb_claim_interface(m_usb_handle, 0) < 0)
    {
        __android_log_print(ANDROID_LOG_ERROR,
                            kAndroidRawBroadcastLogTag,
                            "libusb_claim_interface failed on interface 0");
        libusb_exit(m_libusb_context);
        m_libusb_context = nullptr;
        m_usb_handle = nullptr;
        return false;
    }

    std::unique_ptr<Rtl8812aDevice> created_device = m_wifi_driver->CreateRtlDevice(m_usb_handle);
    if (!created_device)
    {
        __android_log_print(ANDROID_LOG_ERROR,
                            kAndroidRawBroadcastLogTag,
                            "CreateRtlDevice failed");
        libusb_release_interface(m_usb_handle, 0);
        libusb_exit(m_libusb_context);
        m_libusb_context = nullptr;
        m_usb_handle = nullptr;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device = std::shared_ptr<Rtl8812aDevice>(created_device.release());
        m_active_usb_fd = fd;
        resetTxAssemblerLocked();
        m_tx_block_packets.clear();
        m_rx_decoder.reset(Clock::now());
        m_last_rx_decoded_bytes_total = 0;
        m_next_block_index = 1;
        if (m_tx_power > 0)
            m_device->SetTxPower(m_tx_power);
    }

    __android_log_print(ANDROID_LOG_INFO,
                        kAndroidRawBroadcastLogTag,
                        "Started RTL adapter fd=%d channel=%d",
                        fd,
                        s_groundstation_config.wifi_channel);

    m_usb_event_thread = std::make_unique<std::thread>([this]()
    {
        while (true)
        {
            std::shared_ptr<Rtl8812aDevice> device;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                device = m_device;
            }

            if (!device || device->should_stop)
            {
                break;
            }

            timeval timeout = {};
            timeout.tv_usec = 500000;
            const int rc = libusb_handle_events_timeout(m_libusb_context, &timeout);
            if (rc < 0)
            {
                __android_log_print(ANDROID_LOG_WARN,
                                    kAndroidRawBroadcastLogTag,
                                    "libusb_handle_events_timeout rc=%d",
                                    rc);
            }
        }
    });

    m_rx_thread = std::make_unique<std::thread>([this]()
    {
        std::shared_ptr<Rtl8812aDevice> device;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            device = m_device;
        }

        if (!device)
        {
            return;
        }

        device->Init(
            [this](const Packet& packet)
            {
                // RTL8812AU gain_trsw formula (from RTL driver): dBm = (gain & 0x3F) * 2 - 110
                const int dbm0 = (static_cast<int>(packet.RxAtrib.rssi[0] & 0x3F)) * 2 - 110;
                const int dbm1 = (static_cast<int>(packet.RxAtrib.rssi[1] & 0x3F)) * 2 - 110;
                const int input_dbm = std::max(dbm0, dbm1);
                queueReceivedPacket(packet.Data.data(), packet.Data.size(), input_dbm);
            },
            makeSelectedChannel(s_groundstation_config.wifi_channel));
    });

    return true;
}

//===================================================================================
//===================================================================================
// Stops the active Android RTL8812AU adapter and releases its USB/libusb resources.
void AndroidRawBroadcastTransport::stopUsbAdapter()
{
    std::shared_ptr<Rtl8812aDevice> device;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        device = m_device;
        if (device)
        {
            device->should_stop = true;
        }
    }

    if (m_rx_thread && m_rx_thread->joinable())
    {
        m_rx_thread->join();
    }
    if (m_usb_event_thread && m_usb_event_thread->joinable())
    {
        m_usb_event_thread->join();
    }
    m_rx_thread.reset();
    m_usb_event_thread.reset();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device.reset();
        m_active_usb_fd = -1;
        resetTxAssemblerLocked();
        m_tx_block_packets.clear();
    }

    if (m_usb_handle != nullptr)
    {
        libusb_release_interface(m_usb_handle, 0);
        m_usb_handle = nullptr;
    }
    if (m_libusb_context != nullptr)
    {
        libusb_exit(m_libusb_context);
        m_libusb_context = nullptr;
    }

    __android_log_print(ANDROID_LOG_INFO,
                        kAndroidRawBroadcastLogTag,
                        "Stopped RTL adapter");
}

//===================================================================================
//===================================================================================
// Reports whether Android currently owns one running RTL8812AU adapter instance.
bool AndroidRawBroadcastTransport::isUsbAdapterRunning() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_device != nullptr;
}

//===================================================================================
//===================================================================================
// Returns the current active Android USB file descriptor or -1 when no adapter runs.
int AndroidRawBroadcastTransport::activeUsbFd() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_active_usb_fd;
}

//===================================================================================
//===================================================================================
// Installs one callback that receives every filtered raw transport packet immediately.
void AndroidRawBroadcastTransport::setTransportPacketCallback(std::function<void(const uint8_t* data, size_t size, int input_dbm)> callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transport_packet_callback = std::move(callback);
    __android_log_print(ANDROID_LOG_INFO,
                        kAndroidRawBroadcastLogTag,
                        "Transport packet callback installed=%d",
                        m_transport_packet_callback ? 1 : 0);
}

//===================================================================================
//===================================================================================
// Resets the fixed-size TX packet assembler while the caller already holds the mutex.
void AndroidRawBroadcastTransport::resetTxAssemblerLocked()
{
    m_tx_current_packet.resize(m_payload_offset);
    if (!m_radiotap_header.empty())
    {
        std::memcpy(m_tx_current_packet.data(), m_radiotap_header.data(), m_radiotap_header.size());
        std::memcpy(m_tx_current_packet.data() + m_radiotap_header.size(),
                    WLAN_IEEE_HEADER_GROUND2AIR,
                    WLAN_IEEE_HEADER_SIZE);
    }
}

//===================================================================================
//===================================================================================
// Builds the fixed HT20 radiotap transmit header used by the Android RTL driver.
void AndroidRawBroadcastTransport::buildRadiotapHeaderLocked()
{
    m_radiotap_header.resize(1024);
    ieee80211_radiotap_header& hdr =
        reinterpret_cast<ieee80211_radiotap_header&>(*m_radiotap_header.data());
    hdr.it_version = 0;
    hdr.it_present =
        (1 << IEEE80211_RADIOTAP_TX_FLAGS) |
        (1 << IEEE80211_RADIOTAP_DATA_RETRIES) |
        (1 << IEEE80211_RADIOTAP_MCS);

    uint8_t* dst = m_radiotap_header.data() + sizeof(ieee80211_radiotap_header);
    size_t idx = static_cast<size_t>(dst - m_radiotap_header.data());

    radiotapAddU16(dst, idx, IEEE80211_RADIOTAP_F_TX_NOACK);
    radiotapAddU8(dst, idx, 0x00);
    radiotapAddU8(dst, idx, IEEE80211_RADIOTAP_MCS_HAVE_MCS |
                            IEEE80211_RADIOTAP_MCS_HAVE_BW |
                            IEEE80211_RADIOTAP_MCS_HAVE_GI);
    radiotapAddU8(dst, idx, IEEE80211_RADIOTAP_MCS_BW_20);
    radiotapAddU8(dst, idx, std::min(static_cast<uint8_t>(kTxRateHz / 13000000), uint8_t(1)));

    hdr.it_len = static_cast<__le16>(idx);
    m_radiotap_header.resize(idx);
}

//===================================================================================
//===================================================================================
// Sends one fully prepared raw packet through the active RTL8812AU device.
bool AndroidRawBroadcastTransport::sendRawPacket(const std::shared_ptr<Rtl8812aDevice>& device,
                                                 const std::vector<uint8_t>& packet)
{
    if (!device || packet.empty())
    {
        return false;
    }

    if (!device->send_packet(packet.data(), packet.size()))
    {
        __android_log_print(ANDROID_LOG_WARN,
                            kAndroidRawBroadcastLogTag,
                            "send_packet failed size=%zu",
                            packet.size());
        return false;
    }

    return true;
}

//===================================================================================
//===================================================================================
// Filters one received raw-broadcast transport packet and pushes it into the shared FEC decoder.
void AndroidRawBroadcastTransport::queueReceivedPacket(const uint8_t* data,
                                                       size_t size,
                                                       int input_dbm)
{
    static std::atomic<uint32_t> s_rx_seen_count = {0};
    static std::atomic<uint32_t> s_rx_pass_count = {0};

    if (data == nullptr || size < WLAN_IEEE_HEADER_SIZE + sizeof(Packet_Header) + 4)
    {
        return;
    }

    m_all_frame_count.fetch_add(1);
    const uint32_t seen_count = s_rx_seen_count.fetch_add(1) + 1;
    if ((seen_count % 500) == 1)
    {
        __android_log_print(ANDROID_LOG_INFO,
                            kAndroidRawBroadcastLogTag,
                            "RX raw frame count=%u size=%zu rssi=%d mac=%02X:%02X:%02X:%02X:%02X:%02X",
                            seen_count,
                            size,
                            input_dbm,
                            static_cast<unsigned int>(data[10]),
                            static_cast<unsigned int>(data[11]),
                            static_cast<unsigned int>(data[12]),
                            static_cast<unsigned int>(data[13]),
                            static_cast<unsigned int>(data[14]),
                            static_cast<unsigned int>(data[15]));
    }

    if (!matchesLegacyAir2GroundMacSignature(data, size))
    {
        return;
    }

    const uint8_t* transport_packet = data + WLAN_IEEE_HEADER_SIZE;
    size_t transport_size = size - WLAN_IEEE_HEADER_SIZE;

    // The RTL monitor-mode callback includes the 32-bit FCS trailer in Data. The shared
    // packet filter and session pipeline expect the transport packet without that suffix.
    if (transport_size <= 4)
    {
        return;
    }
    transport_size -= 4;

    const PacketFilter::PacketFilterResult filter_result =
        m_packet_filter.filter_packet(transport_packet, transport_size, m_rx_descriptor.mtu);
    if (filter_result != PacketFilter::PacketFilterResult::Pass)
    {
        return;
    }

    const Packet_Header* header = reinterpret_cast<const Packet_Header*>(transport_packet);
    const size_t bounded_session_size =
        std::min(static_cast<size_t>(header->size), transport_size - sizeof(Packet_Header));
    if (bounded_session_size == 0)
    {
        return;
    }

    std::function<void(const uint8_t* data, size_t size, int input_dbm)> callback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        callback = m_transport_packet_callback;
    }
    if (callback)
    {
        callback(transport_packet, transport_size, input_dbm);
    }
    else
    {
        m_rx_decoder.pushPacket(transport_packet, transport_size, 0, Clock::now());
    }
    m_filtered_frame_count.fetch_add(1);
    m_best_input_dbm.store(std::max(m_best_input_dbm.load(), input_dbm));
    m_last_rx_packet_tp.store(Clock::now().time_since_epoch().count());
    const uint32_t rx_count = s_rx_pass_count.fetch_add(1) + 1;
    if (callback && rx_count == 1U)
    {
        __android_log_print(ANDROID_LOG_INFO,
                            kAndroidRawBroadcastLogTag,
                            "Dispatching filtered packet through direct callback");
    }
    if ((rx_count % 100) == 1)
    {
        const Packet_Header* packet_header =
            reinterpret_cast<const Packet_Header*>(transport_packet);
        __android_log_print(ANDROID_LOG_INFO,
                            kAndroidRawBroadcastLogTag,
                            "RX packet count=%u block=%u packet=%u size=%zu rssi=%d",
                            rx_count,
                            packet_header->block_index,
                            static_cast<unsigned int>(packet_header->packet_index),
                            transport_size,
                            input_dbm);
    }
}
