#include "android_raw_broadcast_transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>

#include <libusb.h>

#include "fec.h"
#include "Log.h"
#include "gs_shared_state.h"
#include "structures.h"
#include "devourer/src/FrameParser.h"
#include "devourer/src/SelectedChannel.h"
#include "devourer/src/ieee80211_radiotap.h"

namespace
{

constexpr size_t kTxRateHz = 26000000;
constexpr uint32_t kRxRestartBackjumpBlocks = 64;
constexpr size_t kAndroidRawAdapterCount = 2;

//===================================================================================
//===================================================================================
// Returns the stable menu label for one Android RTL USB adapter slot.
std::string rawUsbAdapterLabel(size_t index)
{
    return "RTL USB Adapter " + std::to_string(index + 1);
}

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
    m_rx_descriptor.interfaces.clear();
    for (size_t index = 0; index < kAndroidRawAdapterCount; index++)
    {
        m_rx_descriptor.interfaces.push_back(rawUsbAdapterLabel(index));
    }
    if (m_tx_descriptor.coding_k == 0 ||
        m_tx_descriptor.coding_n < m_tx_descriptor.coding_k)
    {
        LOGE("Invalid TX coding params k={} n={}",
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
        LOGE("fec_new failed k={} n={}",
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
    decoder_descriptor.interface_count = 2;
    if (!m_rx_decoder.init(decoder_descriptor))
    {
        LOGE("RX decoder init failed k={} n={} mtu={}",
             static_cast<unsigned int>(decoder_descriptor.coding_k),
             static_cast<unsigned int>(decoder_descriptor.coding_n),
             static_cast<unsigned int>(decoder_descriptor.mtu));
        fec_free(m_tx_fec);
        m_tx_fec = nullptr;
        return false;
    }

    LOGI("Initialized raw transport channel={} mtu={} tx_k={} tx_n={} rx_k={} rx_n={}",
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
// Returns Android raw USB adapter status lines for the GS Wi-Fi settings menu.
std::vector<std::string> AndroidRawBroadcastTransport::copyInterfaceStatusLines() const
{
    std::vector<std::string> lines;
    std::lock_guard<std::mutex> lock(m_mutex);
    lines.reserve(m_usb_adapters.size());
    const std::string selected_tx_interface =
        m_tx_descriptor.interface.empty() ? s_groundstation_config.txInterface
                                          : m_tx_descriptor.interface;

    for (const std::shared_ptr<UsbAdapter>& adapter : m_usb_adapters)
    {
        const std::string label = rawUsbAdapterLabel(adapter->index);
        const bool is_selected_tx =
            (selected_tx_interface == "auto" && adapter->index == 0) ||
            selected_tx_interface == label ||
            (selected_tx_interface.empty() && adapter->index == 0);
        lines.push_back(label + ": available" + (is_selected_tx ? " (TX)" : ""));
    }

    return lines;
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
// Deactivates Android raw-broadcast mode and signals every USB adapter to stop.
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
        for (const std::shared_ptr<UsbAdapter>& adapter : m_usb_adapters)
        {
            if (adapter->device)
            {
                adapter->device->should_stop = true;
            }
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
        device = txDeviceLocked();
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
// Retunes every active RTL adapter to the requested monitor-mode channel when running.
void AndroidRawBroadcastTransport::setChannel(int ch)
{
    std::vector<std::shared_ptr<UsbAdapter>> adapters;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        adapters = m_usb_adapters;
    }

    if (adapters.empty())
    {
        LOGI("Ignoring channel change to {} because no adapter is running", ch);
        return;
    }

    std::lock_guard<std::mutex> io_lock(m_device_io_mutex);
    for (const std::shared_ptr<UsbAdapter>& adapter : adapters)
    {
        try
        {
            if (!adapter->device || adapter->device->should_stop)
            {
                continue;
            }

            LOGI("Setting monitor channel to {} on adapter {}", ch, adapter->index);
            adapter->device->SetMonitorChannel(makeSelectedChannel(ch));
        }
        catch (const std::exception& e)
        {
            LOGW("SetMonitorChannel failed after USB detach on adapter {}: {}", adapter->index, e.what());
            adapter->device->should_stop = true;
        }
        catch (...)
        {
            LOGW("SetMonitorChannel failed after USB detach on adapter {} with unknown exception", adapter->index);
            adapter->device->should_stop = true;
        }
    }
}

//===================================================================================
//===================================================================================
// Sets the TX power level on active RTL adapters (0 = driver default, 1..63 = dBm scale).
void AndroidRawBroadcastTransport::setTxPower(int txPower)
{
    std::vector<std::shared_ptr<UsbAdapter>> adapters;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tx_power = static_cast<uint8_t>(std::clamp(txPower, 0, 63));
        adapters = m_usb_adapters;
    }

    if (!adapters.empty())
    {
        std::lock_guard<std::mutex> io_lock(m_device_io_mutex);
        for (const std::shared_ptr<UsbAdapter>& adapter : adapters)
        {
            try
            {
                if (!adapter->device || adapter->device->should_stop)
                {
                    continue;
                }

                LOGI("Setting TX power to {} on adapter {}", m_tx_power, adapter->index);
                adapter->device->SetTxPower(m_tx_power);
            }
            catch (const std::exception& e)
            {
                LOGW("SetTxPower failed after USB detach on adapter {}: {}", adapter->index, e.what());
                adapter->device->should_stop = true;
            }
            catch (...)
            {
                LOGW("SetTxPower failed after USB detach on adapter {} with unknown exception", adapter->index);
                adapter->device->should_stop = true;
            }
        }
    }
}

//===================================================================================
//===================================================================================
// Selects which available Android RTL USB adapter should transmit ground packets.
void AndroidRawBroadcastTransport::setTxInterface(const std::string& interface)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tx_descriptor.interface = interface;
    LOGI("Selected raw TX adapter {}", interface);
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
    stats.noiseFloorDbm = 0;
    std::vector<std::shared_ptr<UsbAdapter>> adapters;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        adapters = m_usb_adapters;
    }
    for (const std::shared_ptr<UsbAdapter>& adapter : adapters)
    {
        const size_t stats_index = std::min(adapter->index, static_cast<size_t>(1));
        const uint32_t all_frames = adapter->all_frame_count.exchange(0);
        const uint32_t filtered_frames = adapter->filtered_frame_count.exchange(0);
        const int adapter_dbm =
            adapter->best_input_dbm.exchange(std::numeric_limits<int>::lowest());
        if (adapter_dbm != std::numeric_limits<int>::lowest())
        {
            stats.rssiDbm[stats_index] = static_cast<int8_t>(std::clamp(adapter_dbm, -127, 0));
        }
        // Filtered frames are already counted per-packet by processTransportPacket via
        // the callback. Only add unmatched frames so all-packet stats stay accurate.
        stats.inPacketCounterAll[stats_index] += static_cast<uint16_t>(all_frames - filtered_frames);
    }
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
// Starts one Android RTL adapter from an already-open USB file descriptor.
bool AndroidRawBroadcastTransport::startUsbAdapter(int fd)
{
    std::lock_guard<std::mutex> stop_lock(m_stop_mutex);
    if (fd < 0)
    {
        LOGE("Refusing to start adapter with invalid fd={}", fd);
        return false;
    }

    std::shared_ptr<UsbAdapter> adapter = std::make_shared<UsbAdapter>();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const std::shared_ptr<UsbAdapter>& active_adapter : m_usb_adapters)
        {
            if (active_adapter->fd == fd)
            {
                LOGI("RTL adapter fd={} is already running", fd);
                return true;
            }
        }
        if (m_usb_adapters.size() >= 2)
        {
            LOGW("Ignoring extra RTL adapter fd={} because Android raw transport supports two", fd);
            return false;
        }
        adapter->fd = fd;
        adapter->index = m_usb_adapters.size();
    }

    libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    if (libusb_init(&adapter->libusb_context) < 0)
    {
        LOGE("libusb_init failed");
        adapter->libusb_context = nullptr;
        return false;
    }

    if (libusb_wrap_sys_device(adapter->libusb_context,
                               static_cast<intptr_t>(fd),
                               &adapter->usb_handle) < 0)
    {
        LOGE("libusb_wrap_sys_device failed fd={}", fd);
        libusb_exit(adapter->libusb_context);
        adapter->libusb_context = nullptr;
        adapter->usb_handle = nullptr;
        return false;
    }

    if (libusb_kernel_driver_active(adapter->usb_handle, 0) == 1)
    {
        libusb_detach_kernel_driver(adapter->usb_handle, 0);
    }

    if (libusb_claim_interface(adapter->usb_handle, 0) < 0)
    {
        LOGE("libusb_claim_interface failed on interface 0");
        libusb_exit(adapter->libusb_context);
        adapter->libusb_context = nullptr;
        adapter->usb_handle = nullptr;
        return false;
    }

    std::unique_ptr<Rtl8812aDevice> created_device = m_wifi_driver->CreateRtlDevice(adapter->usb_handle);
    if (!created_device)
    {
        LOGE("CreateRtlDevice failed");
        libusb_release_interface(adapter->usb_handle, 0);
        libusb_exit(adapter->libusb_context);
        adapter->libusb_context = nullptr;
        adapter->usb_handle = nullptr;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        adapter->device = std::shared_ptr<Rtl8812aDevice>(created_device.release());
        if (m_usb_adapters.empty())
        {
            resetTxAssemblerLocked();
            m_tx_block_packets.clear();
            m_rx_decoder.reset(Clock::now());
            m_last_rx_decoded_bytes_total = 0;
            m_next_block_index = 1;
        }
        if (m_tx_power > 0)
        {
            std::lock_guard<std::mutex> io_lock(m_device_io_mutex);
            adapter->device->SetTxPower(m_tx_power);
        }
        m_usb_adapters.push_back(adapter);
    }

    LOGI("Started RTL adapter {} fd={} channel={}",
         adapter->index,
         fd,
         s_groundstation_config.wifi_channel);

    adapter->usb_event_thread = std::make_unique<std::thread>([adapter]()
    {
        while (true)
        {
            if (!adapter->device || adapter->device->should_stop)
            {
                break;
            }

            timeval timeout = {};
            timeout.tv_usec = 500000;
            const int rc = libusb_handle_events_timeout(adapter->libusb_context, &timeout);
            if (rc < 0)
            {
                LOGW("libusb_handle_events_timeout adapter={} rc={}", adapter->index, rc);
            }
        }
    });

    adapter->rx_thread = std::make_unique<std::thread>([this, adapter]()
    {
        if (!adapter->device)
        {
            return;
        }

        adapter->device->Init(
            [this, adapter](const Packet& packet)
            {
                // RTL8812AU gain_trsw formula (from RTL driver): dBm = (gain & 0x3F) * 2 - 110
                const int dbm0 = (static_cast<int>(packet.RxAtrib.rssi[0] & 0x3F)) * 2 - 110;
                const int dbm1 = (static_cast<int>(packet.RxAtrib.rssi[1] & 0x3F)) * 2 - 110;
                const int input_dbm = std::max(dbm0, dbm1);
                queueReceivedPacket(adapter, packet.Data.data(), packet.Data.size(), input_dbm);
            },
            makeSelectedChannel(s_groundstation_config.wifi_channel));
    });

    return true;
}

//===================================================================================
//===================================================================================
// Stops active Android RTL adapters and releases their USB/libusb resources.
void AndroidRawBroadcastTransport::stopUsbAdapter()
{
    std::lock_guard<std::mutex> stop_lock(m_stop_mutex);
    std::vector<std::shared_ptr<UsbAdapter>> adapters;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        adapters = std::move(m_usb_adapters);
        m_usb_adapters.clear();
    }

    for (const std::shared_ptr<UsbAdapter>& adapter : adapters)
    {
        stopUsbAdapterLocked(adapter);
    }

    // Hot-unplug can leave queued TX work still trying to touch libusb while Java is stopping
    // the transport. Hold the device I/O gate while clearing TX assembly so no send path
    // can retain work for an adapter that is no longer active.
    std::lock_guard<std::mutex> io_lock(m_device_io_mutex);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        resetTxAssemblerLocked();
        m_tx_block_packets.clear();
    }

    LOGI("Stopped {} RTL adapter(s)", adapters.size());
}

//===================================================================================
//===================================================================================
// Stops one Android RTL adapter after it has been removed from the active adapter list.
void AndroidRawBroadcastTransport::stopUsbAdapterLocked(const std::shared_ptr<UsbAdapter>& adapter)
{
    if (adapter->device)
    {
        adapter->device->should_stop = true;
    }
    if (adapter->rx_thread && adapter->rx_thread->joinable())
    {
        adapter->rx_thread->join();
    }
    if (adapter->usb_event_thread && adapter->usb_event_thread->joinable())
    {
        adapter->usb_event_thread->join();
    }
    adapter->rx_thread.reset();
    adapter->usb_event_thread.reset();

    std::lock_guard<std::mutex> io_lock(m_device_io_mutex);
    adapter->device.reset();
    if (adapter->usb_handle != nullptr)
    {
        libusb_release_interface(adapter->usb_handle, 0);
        adapter->usb_handle = nullptr;
    }
    if (adapter->libusb_context != nullptr)
    {
        libusb_exit(adapter->libusb_context);
        adapter->libusb_context = nullptr;
    }
}

//===================================================================================
//===================================================================================
// Reports whether Android currently owns any running RTL adapter instance.
bool AndroidRawBroadcastTransport::isUsbAdapterRunning() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_usb_adapters.empty();
}

//===================================================================================
//===================================================================================
// Returns how many Android RTL USB adapters are currently open for raw transport.
size_t AndroidRawBroadcastTransport::activeUsbAdapterCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_usb_adapters.size();
}

//===================================================================================
//===================================================================================
// Returns the primary Android USB file descriptor or -1 when no adapter runs.
int AndroidRawBroadcastTransport::activeUsbFd() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_usb_adapters.empty() ? -1 : m_usb_adapters.front()->fd;
}

//===================================================================================
//===================================================================================
// Installs one callback that receives every filtered raw transport packet immediately.
void AndroidRawBroadcastTransport::setTransportPacketCallback(
    std::function<void(const uint8_t* data, size_t size, int input_dbm, size_t interface_index)> callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transport_packet_callback = std::move(callback);
    LOGI("Transport packet callback installed={}", m_transport_packet_callback ? 1 : 0);
}

//===================================================================================
//===================================================================================
// Returns the first active adapter device used for ground-to-air TX.
std::shared_ptr<Rtl8812aDevice> AndroidRawBroadcastTransport::txDeviceLocked() const
{
    if (m_usb_adapters.empty())
    {
        return nullptr;
    }

    const std::string selected_tx_interface =
        m_tx_descriptor.interface.empty() ? s_groundstation_config.txInterface
                                          : m_tx_descriptor.interface;
    if (!selected_tx_interface.empty() && selected_tx_interface != "auto")
    {
        for (const std::shared_ptr<UsbAdapter>& adapter : m_usb_adapters)
        {
            if (selected_tx_interface == rawUsbAdapterLabel(adapter->index))
            {
                return adapter->device;
            }
        }
    }

    // A selected adapter can be unplugged. Keep the control link alive by using
    // the first remaining device until the user changes the menu selection.
    return m_usb_adapters.front()->device;
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

    std::lock_guard<std::mutex> io_lock(m_device_io_mutex);
    if (device->should_stop)
    {
        return false;
    }

    try
    {
        if (!device->send_packet(packet.data(), packet.size()))
        {
            LOGW("send_packet failed size={}", packet.size());
            return false;
        }
    }
    catch (const std::exception& e)
    {
        LOGW("send_packet threw after USB detach: {}", e.what());
        device->should_stop = true;
        return false;
    }
    catch (...)
    {
        LOGW("send_packet threw after USB detach with unknown exception");
        device->should_stop = true;
        return false;
    }

    return true;
}

//===================================================================================
//===================================================================================
// Filters one received raw-broadcast transport packet and pushes it into the shared FEC decoder.
void AndroidRawBroadcastTransport::queueReceivedPacket(const std::shared_ptr<UsbAdapter>& adapter,
                                                       const uint8_t* data,
                                                       size_t size,
                                                       int input_dbm)
{
    static std::atomic<uint32_t> s_rx_seen_count = {0};
    static std::atomic<uint32_t> s_rx_pass_count = {0};

    if (data == nullptr || size < WLAN_IEEE_HEADER_SIZE + sizeof(Packet_Header) + 4)
    {
        return;
    }

    adapter->all_frame_count.fetch_add(1);
    const uint32_t seen_count = s_rx_seen_count.fetch_add(1) + 1;
    if ((seen_count % 500) == 1)
    {
        LOGI("RX raw frame count={} size={} rssi={} mac={:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
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

    std::function<void(const uint8_t* data, size_t size, int input_dbm, size_t interface_index)> callback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        callback = m_transport_packet_callback;
    }
    if (callback)
    {
        callback(transport_packet, transport_size, input_dbm, adapter->index);
    }
    else
    {
        m_rx_decoder.pushPacket(transport_packet, transport_size, adapter->index, Clock::now());
    }
    adapter->filtered_frame_count.fetch_add(1);
    adapter->best_input_dbm.store(std::max(adapter->best_input_dbm.load(), input_dbm));
    m_best_input_dbm.store(std::max(m_best_input_dbm.load(), input_dbm));
    m_last_rx_packet_tp.store(Clock::now().time_since_epoch().count());
    const uint32_t rx_count = s_rx_pass_count.fetch_add(1) + 1;
    if (callback && rx_count == 1U)
    {
        LOGI("Dispatching filtered packet through direct callback");
    }
    if ((rx_count % 100) == 1)
    {
        const Packet_Header* packet_header =
            reinterpret_cast<const Packet_Header*>(transport_packet);
        LOGI("RX packet count={} block={} packet={} size={} rssi={}",
             rx_count,
             packet_header->block_index,
             static_cast<unsigned int>(packet_header->packet_index),
             transport_size,
             input_dbm);
    }
}
