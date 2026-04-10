#include "linux_raw_broadcast_transport.h"
#include <pcap.h>
#include "radiotap/radiotap.h"
#include <mutex>
#include <condition_variable>
#include <deque>
#include <set>
#include <cassert>
#include <atomic>
#include <iostream>
#include "fec.h"
#include "Log.h"
#include "Pool.h"
#include "structures.h"
#include <algorithm>
#include "gs_linux_runtime.h"
#include "gs_runtime_core.h"
#include "gs_stats.h"
#include "packets.h"
#include "shared/frame_packets_debug.h"
#include "utils.h"
#include <fstream>
#include <cerrno>
#include <cstring>

//#define DEBUG_PCAP

static constexpr size_t DEFAULT_RATE_HZ = 26000000;
static constexpr uint32_t RX_RESTART_BACKJUMP_BLOCKS = 64;

static std::vector<uint8_t> RADIOTAP_HEADER;

static constexpr size_t SRC_MAC_LASTBYTE = 15;
static constexpr size_t DST_MAC_LASTBYTE = 21;

namespace
{

//===================================================================================
//===================================================================================
// Forces one monitor-mode interface onto the configured raw-broadcast channel with HT20 width.
void setMonitorChannel(const std::string& interface, int channel)
{
    if (channel <= 0)
    {
        return;
    }

    if (!runShellCommand(fmt::format("iw dev {} set channel {} HT20", interface, channel)))
    {
        LOGW("Failed to set monitor channel {} HT20 on {} via iw; falling back to iwconfig",
             channel,
             interface);
        runShellCommand(fmt::format("iwconfig {} channel {}", interface, channel));
    }
}

//===================================================================================
//===================================================================================
// Waits until the Linux network interface reports an operational up state.
bool waitForInterfaceUp(const std::string& interface)
{
    const std::string operstate_path = fmt::format("/sys/class/net/{}/operstate", interface);
    const std::string carrier_path = fmt::format("/sys/class/net/{}/carrier", interface);

    for (int attempt = 0; attempt < 20; ++attempt)
    {
        {
            std::ifstream operstate_file(operstate_path);
            std::string operstate;
            if (operstate_file.is_open())
            {
                std::getline(operstate_file, operstate);
                if (operstate == "up" || operstate == "unknown")
                {
                    return true;
                }
            }
        }

        {
            std::ifstream carrier_file(carrier_path);
            std::string carrier;
            if (carrier_file.is_open())
            {
                std::getline(carrier_file, carrier);
                if (carrier == "1")
                {
                    return true;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return false;
}

//===================================================================================
//===================================================================================
// Returns whether one 802.11 header matches the exact 6-byte legacy MAC signature used by the old pcap filter.
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

}

//===================================================================================
//===================================================================================
// Reports that the raw-broadcast transport uses wifi-channel search.
bool LinuxRawBroadcastTransport::usesChannelSearch() const
{
    return true;
}

//===================================================================================
//===================================================================================
// Reports that raw-broadcast search is handled by the generic channel-search flow instead of transport hooks.
bool LinuxRawBroadcastTransport::supportsMenuSearchOrConnect() const
{
    return false;
}

//===================================================================================
//===================================================================================
// Reapplies the raw-broadcast activation path after menu-driven reconnect requests.
bool LinuxRawBroadcastTransport::requestImmediateReconnect()
{
    activate();
    return true;
}

//===================================================================================
//===================================================================================
// Leaves menu-search control to the generic transport manager for raw-broadcast mode.
void LinuxRawBroadcastTransport::beginMenuSearchOrConnect()
{
}

//===================================================================================
//===================================================================================
// Leaves menu-search control to the generic transport manager for raw-broadcast mode.
bool LinuxRawBroadcastTransport::advanceMenuSearchOrConnect()
{
    return false;
}

//===================================================================================
//===================================================================================
// Leaves menu-search control to the generic transport manager for raw-broadcast mode.
void LinuxRawBroadcastTransport::cancelMenuSearchOrConnect()
{
}

//===================================================================================
//===================================================================================
// Activates raw-broadcast mode by switching the configured RX interfaces to monitor mode
// and reopening the pcap backend on the freshly reconfigured interface handles.
void LinuxRawBroadcastTransport::activate()
{
    // Reopen the backend on every activation because switching the same adapter through
    // APFPV managed mode can invalidate the old pcap handle and later inject/read calls
    // fail with "No such device or address" on the stale descriptor.
    setLinkState(LinkState::LookingForWifiNetwork);
    setLinkStateDetailText("Setting monitor mode...");
    stopBackend();
    setMonitorMode(m_rx_descriptor.interfaces);
    setChannel(s_groundstation_config.wifi_channel);
    if (!startBackend())
    {
        LOGE("Failed to restart raw-broadcast backend after switching to monitor mode");
        return;
    }
    setLinkStateDetailText({});
}

//===================================================================================
//===================================================================================
// Stops the raw-broadcast backend while another transport temporarily owns the interfaces.
void LinuxRawBroadcastTransport::deactivate()
{
    stopBackend();
}

/*
// Penumbra IEEE80211 header
static uint8_t IEEE_HEADER[] =
{
    0x08,
    0x01,
    0x00,
    0x00,
    0x13,
    0x22,
    0x33,
    0x44,
    0x55,
    0x66,
    0x13,
    0x22,
    0x33,
    0x44,
    0x55,
    0x66,
    0x13,
    0x22,
    0x33,
    0x44,
    0x55,
    0x66,
    0x10,
    0x86,
};
*/

#pragma pack(push, 1)

struct Penumbra_Radiotap_Header
{
    int32_t channel = 0;
    int32_t channel_flags = 0;
    int32_t rate = 0;
    int32_t input_dBm = 0;
    int32_t radiotap_flags = 0;
};

/*
struct Packet_Header
{
    uint32_t block_index : 24;
    uint32_t packet_index : 8;
    uint16_t size = 0;
};
*/

#pragma pack(pop)

//===================================================================================
//===================================================================================
struct LinuxRawBroadcastTransport::PCap
{
    std::mutex mutex;
    pcap_t* pcap = nullptr;
    char error_buffer[PCAP_ERRBUF_SIZE] = {0};
    int rx_pcap_selectable_fd = 0;

    size_t _80211_header_length = 0;
    size_t index = 0;
};

//===================================================================================
//===================================================================================
struct LinuxRawBroadcastTransport::TX
{
    std::thread thread;

    fec_t* fec = nullptr;
    std::array<uint8_t const*, 16> fec_src_packet_ptrs;
    std::array<uint8_t*, 32> fec_dst_packet_ptrs;

    PCap* pcap = nullptr;

    struct Packet 
    {
        std::vector<uint8_t> data;      //=RADIOTAP_HEADER.size() + sizeof(WLAN_IEEE_HEADER_GROUND2AIR) + Packet_Header + mtu
    };
    using Packet_ptr = Pool<Packet>::Ptr;
    Pool<Packet> packet_pool;

    size_t transport_packet_size = 0;   //=RADIOTAP_HEADER.size() + sizeof(WLAN_IEEE_HEADER_GROUND2AIR) + Packet_Header + mtu
    size_t streaming_packet_size = 0;   //Packet_Header + mtu
    size_t payload_size = 0;            //mtu

    ////////
    //These are accessed by both the TX thread and the main thread
    std::mutex packet_queue_mutex;
    std::deque<Packet_ptr> packet_queue;
    std::condition_variable packet_queue_cv;
    ////////

    ////////
    //these live in the TX thread only
    std::deque<Packet_ptr> ready_packet_queue;
    std::vector<Packet_ptr> block_packets;
    std::vector<Packet_ptr> block_fec_packets;
    ///////

    Packet_ptr crt_packet;

    uint32_t last_block_index = 1;
};

//===================================================================================
//===================================================================================
struct LinuxRawBroadcastTransport::RX
{
    std::vector<std::thread> threads;

    std::vector<std::string> interfaces;  //this list contans both RX and TX interfaces. We do not support TX only interface.
    std::vector<std::unique_ptr<PCap>> pcaps;
    FecBlockDecoder decoder;
};

//===================================================================================
//===================================================================================
static void seal_packet(PacketFilter& packet_filter,
                        LinuxRawBroadcastTransport::TX::Packet& packet,
                        size_t header_offset,
                        uint32_t block_index,
                        uint8_t packet_index)
{
    //header_offset = RADIOTAP_HEADER.size() + sizeof(WLAN_IEEE_HEADER_GROUND2AIR);
    assert(packet.data.size() >= header_offset + sizeof(Packet_Header));

    Packet_Header& header = *reinterpret_cast<Packet_Header*>(packet.data.data() + header_offset);

    packet_filter.apply_packet_header_data(&header);

    header.size = packet.data.size() - header_offset - sizeof( Packet_Header ); //size of user data, without Packet_header
    header.block_index = block_index;
    header.packet_index = packet_index;
}

//===================================================================================
//===================================================================================
struct LinuxRawBroadcastTransport::Impl
{
    size_t tx_packet_header_length = 0;  //=RADIOTAP_HEADER.size() + sizeof(WLAN_IEEE_HEADER_GROUND2AIR);

    TX tx;
    RX rx;
};

/*
//===================================================================================
//===================================================================================
std::vector<std::string> LinuxRawBroadcastTransport::enumerate_interfaces()
{
    std::vector<std::string> res;

    char error_buf[PCAP_ERRBUF_SIZE];
    pcap_if_t* head = nullptr;
    if (pcap_findalldevs(&head, error_buf) == -1)
    {
        LOGE("Error enumerating rfmon interfaces: {}", error_buf);
        return {};
    }

    pcap_if_t* crt = head;
    while (crt)
    {
        if (crt->flags & PCAP_IF_LOOPBACK)
            LOGI("Skipping {} because it's a loppback interface", crt->name);
        else if ((crt->flags & PCAP_IF_UP) == 0)
            LOGI("Skipping {} because it's down", crt->name);
        else
            res.push_back(crt->name);
        crt = crt->next;
    }

    pcap_freealldevs(head);

    return res;
}
*/

//===================================================================================
//===================================================================================
LinuxRawBroadcastTransport::LinuxRawBroadcastTransport()
{
}

//===================================================================================
//===================================================================================
LinuxRawBroadcastTransport::~LinuxRawBroadcastTransport()
{
    stopBackend();

    if (m_impl && m_impl->tx.fec)
    {
        fec_free(m_impl->tx.fec);
        m_impl->tx.fec = nullptr;
    }
}

//===================================================================================
//===================================================================================
bool LinuxRawBroadcastTransport::prepare_filter(PCap& pcap)
{
    int link_encap = pcap_datalink(pcap.pcap);
    const char* datalink_name = pcap_datalink_val_to_name(link_encap);
    LOGI("pcap datalink: {} ({})", link_encap, datalink_name != nullptr ? datalink_name : "unknown");

    switch (link_encap)
    {
    case DLT_PRISM_HEADER:
        LOGI("DLT_PRISM_HEADER Encap");
        pcap._80211_header_length = 0x20; // ieee80211 comes after this
        break;

    case DLT_IEEE802_11_RADIO:
        LOGI("DLT_IEEE802_11_RADIO Encap");
        pcap._80211_header_length = 0x18; // ieee80211 comes after this
        break;

    default:
        LOGE("!!! unknown encapsulation");
        return false;
    }

    pcap.rx_pcap_selectable_fd = pcap_get_selectable_fd(pcap.pcap);
    if (pcap.rx_pcap_selectable_fd < 0)
    {
        LOGE("pcap_get_selectable_fd failed for interface {}", m_rx_descriptor.interfaces[pcap.index]);
        return false;
    }

    return true;
}

//===================================================================================
//===================================================================================
static void radiotap_add_u8(uint8_t*& dst, size_t& idx, uint8_t data)
{
    *dst++ = data;
    idx++;
}

//===================================================================================
//===================================================================================
static void radiotap_add_u16(uint8_t*& dst, size_t& idx, uint16_t data)
{
    if ((idx & 1) == 1) //not aligned, pad first
    {
        radiotap_add_u8(dst, idx, 0);
    }
    *reinterpret_cast<uint16_t*>(dst) = data;
    dst += 2;
    idx += 2;
}

//===================================================================================
//===================================================================================
void LinuxRawBroadcastTransport::prepare_radiotap_header(size_t rate_hz)
{
    RADIOTAP_HEADER.resize(1024);
    ieee80211_radiotap_header& hdr = reinterpret_cast<ieee80211_radiotap_header& >(*RADIOTAP_HEADER.data());
    hdr.it_version = 0;
    hdr.it_present = 0
                        //| (1 << IEEE80211_RADIOTAP_RATE)
                        | (1 << IEEE80211_RADIOTAP_TX_FLAGS)
                        //| (1 << IEEE80211_RADIOTAP_RTS_RETRIES)
                        | (1 << IEEE80211_RADIOTAP_DATA_RETRIES)
                        //| (1 << IEEE80211_RADIOTAP_CHANNEL)
                        | (1 << IEEE80211_RADIOTAP_MCS)
        ;

    auto* dst = RADIOTAP_HEADER.data() + sizeof(ieee80211_radiotap_header);
    size_t idx = dst - RADIOTAP_HEADER.data();

    if (hdr.it_present & (1 << IEEE80211_RADIOTAP_RATE))
        radiotap_add_u8(dst, idx, std::min(static_cast<uint8_t>(rate_hz / 500000), uint8_t(1)));
    if (hdr.it_present & (1 << IEEE80211_RADIOTAP_TX_FLAGS))
        radiotap_add_u16(dst, idx, IEEE80211_RADIOTAP_F_TX_NOACK); //used to be 0x18
    if (hdr.it_present & (1 << IEEE80211_RADIOTAP_RTS_RETRIES))
        radiotap_add_u8(dst, idx, 0x0);
    if (hdr.it_present & (1 << IEEE80211_RADIOTAP_DATA_RETRIES))
        radiotap_add_u8(dst, idx, 0x0);
    if (hdr.it_present & (1 << IEEE80211_RADIOTAP_MCS))
    {
        radiotap_add_u8(dst, idx, IEEE80211_RADIOTAP_MCS_HAVE_MCS | IEEE80211_RADIOTAP_MCS_HAVE_BW | IEEE80211_RADIOTAP_MCS_HAVE_GI ); // short gI
        radiotap_add_u8(dst, idx, IEEE80211_RADIOTAP_MCS_BW_20 );  //HT20
        radiotap_add_u8(dst, idx, 1);  //MCS Index 1 13M
    }
    if (hdr.it_present & (1 << IEEE80211_RADIOTAP_CHANNEL))
    {
        radiotap_add_u16(dst, idx, 2467);
        radiotap_add_u16(dst, idx, 0);
    }

    //finish it
    hdr.it_len = static_cast<__le16>(idx);
    RADIOTAP_HEADER.resize(idx);

    //    RADIOTAP_HEADER.resize(sizeof(RADIOTAP_HEADER_original));
    //    memcpy(RADIOTAP_HEADER.data(), RADIOTAP_HEADER_original, sizeof(RADIOTAP_HEADER_original));
}

//===================================================================================
//===================================================================================
void LinuxRawBroadcastTransport::prepare_tx_packet_header(uint8_t* buffer)
{
    //prepare the buffers with headers
    uint8_t* pu8 = buffer;

    memcpy(pu8, RADIOTAP_HEADER.data(), RADIOTAP_HEADER.size());
    pu8 += RADIOTAP_HEADER.size();

    memcpy(pu8, WLAN_IEEE_HEADER_GROUND2AIR, sizeof(WLAN_IEEE_HEADER_GROUND2AIR));
    pu8 += sizeof(WLAN_IEEE_HEADER_GROUND2AIR);
}

//===================================================================================
//===================================================================================
// Drains all packets currently available from one raw pcap handle.
bool LinuxRawBroadcastTransport::process_rx_packet(PCap& pcap)
{
    struct pcap_pkthdr* pcap_packet_header = nullptr;
    uint8_t* payload;

    while (true)
    {
        {
            std::lock_guard<std::mutex> lg(pcap.mutex);
            int retval = pcap_next_ex(pcap.pcap, &pcap_packet_header, (const u_char**)&payload);
            if (retval < 0)
            {
                LOGE("Socket broken: {}", pcap_geterr(pcap.pcap));
                return false;
            }
            if (retval != 1)
            {
                break;
            }
        }

        if (pcap.index < 2)
        {
            std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
            s_gs_stats.inPacketCounterAll[pcap.index]++;
        }

        size_t header_len = (payload[2] + (payload[3] << 8));
        if (pcap_packet_header->len < (header_len + pcap._80211_header_length))
        {
            //LOGW("packet too small");
            return true;
        }

        const uint8_t* ieee_header = payload + header_len;
        size_t bytes = pcap_packet_header->len - (header_len + pcap._80211_header_length);

        ieee80211_radiotap_iterator rti;
        if (ieee80211_radiotap_iterator_init(&rti, (struct ieee80211_radiotap_header*)payload, pcap_packet_header->len) < 0)
        {
            LOGE("iterator null");
            return true;
        }

        Penumbra_Radiotap_Header prh;
        prh.input_dBm = -1000;
        while (ieee80211_radiotap_iterator_next(&rti) == 0)
        {
            switch (rti.this_arg_index)
            {
            case IEEE80211_RADIOTAP_RATE:
                prh.rate = (*rti.this_arg);
                break;

            case IEEE80211_RADIOTAP_CHANNEL:
                prh.channel = (*((uint16_t*)rti.this_arg));
                prh.channel_flags = (*((uint16_t*)(rti.this_arg + 2)));
                break;

            case IEEE80211_RADIOTAP_DBM_ANTSIGNAL:
                prh.input_dBm = *(int8_t*)rti.this_arg;
                {
                    std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
                    s_gs_stats.rssiDbm[pcap.index] = *(int8_t*)rti.this_arg;
                }
                break;

            case IEEE80211_RADIOTAP_DBM_ANTNOISE:
                {
                    std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
                    s_gs_stats.noiseFloorDbm = *(int8_t*)rti.this_arg;
                }
                break;

            /*
            case IEEE80211_RADIOTAP_ANTENNA:
                if ((uint8_t*)rti.this_arg == 0 ) 
                {
                    s_gs_stats.antena1PacketsCounter++;
                }
                else
                {
                    s_gs_stats.antena2PacketsCounter++;
                }
                break;
            */

            case IEEE80211_RADIOTAP_FLAGS:
                prh.radiotap_flags = *rti.this_arg;
                break;
            }
        }
        payload += header_len + pcap._80211_header_length;

        if (!matchesLegacyAir2GroundMacSignature(ieee_header, pcap._80211_header_length))
        {
            s_runtimeCore.transport_packets_filtered++;
            continue;
        }

        if (prh.radiotap_flags & IEEE80211_RADIOTAP_F_FCS)
            bytes -= 4;

        bool checksum_correct = (prh.radiotap_flags & 0x40) == 0;

        //    block_num = seq_nr / param_retransmission_block_size;//if retr_block_size would be limited to powers of two, this could be replaced by a logical AND operation

        //printf("rec %x bytes %d crc %d\n", seq_nr, bytes, checksum_correct);

#ifdef DEBUG_PCAP
        std::cout << "PCAP RX>>";
        std::copy(payload, payload + bytes, std::ostream_iterator<uint8_t>(std::cout));
        std::cout << "<<PCAP RX";
#endif
        if (!checksum_correct)
        {
            LOGW("invalid checksum.");
            return true;
        }

/*
        //ignore packets from neightbour channels
        //update: does not work? no channel number in packet?
        if ( prh.channel != s_groundstation_config.wifi_channel )
        {
            return true; 
        }
*/
        s_runtimeCore.transport_packets_seen++;
        if (bytes >= sizeof(Packet_Header))
        {
            const Packet_Header& header = *reinterpret_cast<const Packet_Header*>(payload);
            s_runtimeCore.last_transport_block = header.block_index;
            s_runtimeCore.last_transport_packet_index = header.packet_index;
            s_runtimeCore.last_transport_payload_size = header.size;
            s_runtimeCore.last_transport_from = header.fromDeviceId;
            s_runtimeCore.last_transport_to = header.toDeviceId;
        }
        else
        {
            s_runtimeCore.last_transport_block = 0;
            s_runtimeCore.last_transport_packet_index = 0;
            s_runtimeCore.last_transport_payload_size = 0;
            s_runtimeCore.last_transport_from = 0;
            s_runtimeCore.last_transport_to = 0;
        }

        auto res = m_packet_filter.filter_packet(payload, bytes, m_rx_descriptor.mtu);
        if ( res != PacketFilter::PacketFilterResult::Pass )
        {
            s_runtimeCore.transport_packets_filtered++;
            if ( res == PacketFilter::PacketFilterResult::WrongVersion )
            {
                s_incompatibleFirmwareTime = Clock::now();
            }
            continue;
        }

        s_runtimeCore.transport_packets_passed_filter++;
        if (pcap.index < 2)
        {
            std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
            s_gs_stats.inPacketCounter[pcap.index]++;
        }
        {
            if (prh.input_dBm > -1000)
            {
                int best_input_dBm = m_best_input_dBm;
                m_best_input_dBm = std::max(best_input_dBm, prh.input_dBm);
            }
        }

        m_impl->rx.decoder.pushPacket(payload, bytes, pcap.index, Clock::now());
        sync_rx_decoder_stats();

        //m_impl->rx_queue.enqueue(payload, bytes);
        //        if (receive_callback && bytes > 0)
        //        {
        //            receive_callback(payload, bytes);
        //        }

#ifdef DEBUG_THROUGHPUT
        {
            static int xxx_data = 0;
            static std::chrono::system_clock::time_point xxx_last_tp = std::chrono::system_clock::now();
            xxx_data += bytes;
            auto now = std::chrono::system_clock::now();
            if (now - xxx_last_tp >= std::chrono::seconds(1))
            {
                float r = std::chrono::duration<float>(now - xxx_last_tp).count();
                LOGI("Received: {} KB/s", float(xxx_data) / r / 1024.f);
                xxx_data = 0;
                xxx_last_tp = now;
            }
        }
#endif
    }

    return true;
}

//===================================================================================
//===================================================================================
bool LinuxRawBroadcastTransport::prepare_pcap(std::string const& interface, PCap& pcap, RX_Descriptor const& rx_descriptor)
{
    LOGI("Opening interface {} in monitor mode", interface);

    runShellCommand(fmt::format("ip link set {} up", interface));
    if (!waitForInterfaceUp(interface))
    {
        LOGW("Interface {} did not report up before pcap activation", interface);
    }

    pcap.pcap = pcap_create(interface.c_str(), pcap.error_buffer);
    if (pcap.pcap == nullptr)
    {
        LOGE("Unable to open interface {}: {}", interface, pcap.error_buffer);
        return false;
    }
    if (pcap_set_snaplen(pcap.pcap, 1800) < 0)
    {
        LOGE("Error setting pcap_set_snaplen: {}", pcap_geterr(pcap.pcap));
        return false;
    }
    if (pcap_set_promisc(pcap.pcap, 1) < 0)
    {
        LOGE("Error setting pcap_set_promisc: {}", pcap_geterr(pcap.pcap));
        return false;
    }
    if (!rx_descriptor.skip_mon_mode_cfg)
    {
        if (pcap_set_rfmon(pcap.pcap, 1) < 0)
        {
                LOGE("Error setting pcap_set_rfmon: {}", pcap_geterr(pcap.pcap));
                LOGE("Try running with -sm 1 flag\n");
                return false;
        }
    }
    if (pcap_set_timeout(pcap.pcap, -1) < 0)
    {
        LOGE("Error setting pcap_set_timeout: {}", pcap_geterr(pcap.pcap));
        return false;
    }
    if (pcap_set_immediate_mode(pcap.pcap, 1) < 0)
    {
        LOGE("Error setting pcap_set_immediate_mode: {}", pcap_geterr(pcap.pcap));
        return false;
    }
    if (pcap_set_buffer_size(pcap.pcap, 16000000) < 0)
    {
        LOGE("Error setting pcap_set_buffer_size: {}", pcap_geterr(pcap.pcap));
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int res = pcap_activate(pcap.pcap);
    if (res < 0)
    {
        LOGE("Error in pcap_activate: {}", pcap_geterr(pcap.pcap));
        LOGE("Try running under root account.");
        return false;
    }
    else if (res == PCAP_WARNING_PROMISC_NOTSUP)
    {
        LOGE("Error in pcap_activate - not promiscous: {}", pcap_geterr(pcap.pcap));
        return false;
    }
    else if (res == PCAP_WARNING_TSTAMP_TYPE_NOTSUP)
    {
        //nothing, we don't care about timestamps
    }
    else if (res == PCAP_WARNING)
    {
        LOGW("Warning in pcap_activate: {}", pcap_geterr(pcap.pcap));
    }

    //    if (pcap_setnonblock(pcap.pcap, 1, pcap_error) < 0)
    //    {
    //        LOGE("Error setting pcap_set_snaplen: {}", pcap_geterr(pcap.pcap));
    //        return false;
    //    }
    if (pcap_setdirection(pcap.pcap, PCAP_D_IN) < 0)
    {
        LOGE("Error setting pcap_setdirection: {}", pcap_geterr(pcap.pcap));
        return false;
    }

    if (!prepare_filter(pcap))
        return false;

    return true;
}

//===================================================================================
//===================================================================================
bool LinuxRawBroadcastTransport::init(RX_Descriptor const& rx_descriptor, TX_Descriptor const& tx_descriptor)
{
    if (tx_descriptor.interface.empty())
    {
        LOGE("Invalid TX interface");
        return false;
    }

    m_impl.reset(new Impl);

    m_tx_descriptor = tx_descriptor;
    //m_tx_descriptor.mtu = std::min(tx_descriptor.mtu, AIR2GROUND_MAX_MTU);

    if (m_tx_descriptor.coding_k == 0 || 
        m_tx_descriptor.coding_n < m_tx_descriptor.coding_k || 
        m_tx_descriptor.coding_k > m_impl->tx.fec_src_packet_ptrs.size() || 
        m_tx_descriptor.coding_n > m_impl->tx.fec_dst_packet_ptrs.size())
    {
        LOGE("Invalid coding params: {} / {}", m_tx_descriptor.coding_k, m_tx_descriptor.coding_n);
        return false;
    }

    if (m_impl->tx.fec)
        fec_free(m_impl->tx.fec);

    m_impl->tx.fec = fec_new(m_tx_descriptor.coding_k, m_tx_descriptor.coding_n);

    /////////
    
    if (rx_descriptor.interfaces.empty())
    {
        LOGE("Invalid RX interfaces");
        return false;
    }

    this->m_rx_descriptor = rx_descriptor;

    //make sure TX interface is in the list of RX interfaces
    if (std::find(this->m_rx_descriptor.interfaces.begin(), this->m_rx_descriptor.interfaces.end(), tx_descriptor.interface) == this->m_rx_descriptor.interfaces.end()) 
    {
        this->m_rx_descriptor.interfaces.push_back(tx_descriptor.interface);
    }

    //this->m_rx_descriptor.mtu = std::min(this->m_rx_descriptor.mtu, AIR2GROUND_MAX_MTU);

    if (m_rx_descriptor.coding_k == 0 || 
        m_rx_descriptor.coding_n < m_rx_descriptor.coding_k)
    {
        LOGE("Invalid coding params: {} / {}", m_rx_descriptor.coding_k, m_rx_descriptor.coding_n);
        return false;
    }

    //    IEEE_HEADER[SRC_MAC_LASTBYTE] = 0;
    //    IEEE_HEADER[DST_MAC_LASTBYTE] = 0;

    prepare_radiotap_header(DEFAULT_RATE_HZ);
    m_impl->tx_packet_header_length = RADIOTAP_HEADER.size() + sizeof(WLAN_IEEE_HEADER_GROUND2AIR);
    LOGI("Radiocap header size: {}, IEEE header size: {}", RADIOTAP_HEADER.size(), sizeof(WLAN_IEEE_HEADER_GROUND2AIR));

    /////////////////////
    //calculate some offsets and sizes
    m_packet_header_offset = m_impl->tx_packet_header_length;
    m_payload_offset = m_packet_header_offset + sizeof(Packet_Header);

    m_impl->tx.transport_packet_size = m_payload_offset + m_tx_descriptor.mtu;
    m_impl->tx.streaming_packet_size = m_impl->tx.transport_packet_size - m_impl->tx_packet_header_length;
    m_impl->tx.payload_size = m_tx_descriptor.mtu;

    /////////////////////

    m_impl->tx.packet_pool.on_acquire = [this](TX::Packet& packet) 
    {
        if (packet.data.empty())
        {
            packet.data.resize(m_payload_offset);           //=RADIOTAP_HEADER.size() + sizeof(WLAN_IEEE_HEADER_GROUND2AIR) + Packet_Header
            prepare_tx_packet_header(packet.data.data());   //prepare RADIOTAP_HEADER, WLAN_IEEE_HEADER_GROUND2AIR
        }
        else
            packet.data.resize(m_payload_offset);
    };

    //    m_impl->pcap = pcap_open_live(m_interface.c_str(), 2048, 1, -1, pcap_error);
    //    if (m_impl->pcap == nullptr)
    //    {
    //        LOGE("Unable to open interface {} in pcap: {}", m_interface, pcap_error);
    //        return false;
    //    }

    ////    if (pcap_setnonblock(m_impl->pcap, 1, pcap_error) < 0)
    ////    {
    ////        LOGE("Error setting {} to nonblocking mode: {}", m_interface, pcap_error);
    ////        return false;
    ////    }

    //    if (pcap_setdirection(m_impl->pcap, PCAP_D_IN) < 0)
    //    {
    //        LOGE("Error setting {} to IN capture only: {}", m_interface, pcap_geterr(m_impl->pcap));
    //        return false;
    //    }

    FecBlockDecoder::Descriptor decoder_descriptor = {};
    decoder_descriptor.coding_k = m_rx_descriptor.coding_k;
    decoder_descriptor.coding_n = m_rx_descriptor.coding_n;
    decoder_descriptor.mtu = static_cast<uint16_t>(m_rx_descriptor.mtu);
    decoder_descriptor.reset_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(m_rx_descriptor.reset_duration);
    decoder_descriptor.restart_backjump_blocks = RX_RESTART_BACKJUMP_BLOCKS;
    decoder_descriptor.max_block_queue_size = 3;
    decoder_descriptor.duplicate_window = 100;
    decoder_descriptor.interface_count = this->m_rx_descriptor.interfaces.size();
    if (!m_impl->rx.decoder.init(decoder_descriptor))
    {
        LOGE("Failed to initialize RX FEC block decoder");
        return false;
    }

    FecBlockDecoder::Callbacks callbacks = {};
    callbacks.on_packet_received = [](uint32_t block_index, uint32_t packet_index, const uint8_t* packet_data, bool old)
    {
        g_framePacketsDebug.onPacketReceived(block_index, packet_index, packet_data, old);
    };
    callbacks.on_packet_restored = [](uint32_t block_index, uint32_t packet_index, const uint8_t* packet_data)
    {
        g_framePacketsDebug.onPacketRestored(block_index, packet_index, packet_data);
    };
    callbacks.on_stream_restart_detected = [](uint32_t previous_block, uint32_t new_block, uint32_t delta)
    {
        LOGW("Detected TX stream restart: block jump back {} -> {} (delta {})", previous_block, new_block, delta);
    };
    m_impl->rx.decoder.setCallbacks(std::move(callbacks));
    sync_rx_decoder_stats();

#if defined RASPBERRY_PI_XXX
    {
        //        int policy = SCHED_OTHER;
        //        struct sched_param param;
        //        param.sched_priority = 0;
        int policy = SCHED_FIFO;
        struct sched_param param;
        param.sched_priority = sched_get_priority_max(policy);
        if (pthread_setschedparam(m_impl->tx.thread.native_handle(), policy, &param) != 0)
            perror("Cannot set TX thread priority - using normal");
        if (pthread_setschedparam(m_impl->rx.thread.native_handle(), policy, &param) != 0)
            perror("Cannot set TX thread priority - using normal");
    }
#endif

    return true;
}

//===================================================================================
//===================================================================================
// Reopens all raw-broadcast pcap handles and worker threads on the current monitor-mode interfaces.
bool LinuxRawBroadcastTransport::startBackend()
{
    if (!m_impl)
    {
        return false;
    }

    m_exit = false;
    m_impl->tx.pcap = nullptr;
    m_impl->rx.pcaps.clear();
    m_impl->rx.pcaps.resize(m_rx_descriptor.interfaces.size());

    size_t index = 0;
    for (const auto& interf : m_rx_descriptor.interfaces)
    {
        m_impl->rx.pcaps[index] = std::make_unique<PCap>();
        m_impl->rx.pcaps[index]->index = index;
        if (!prepare_pcap(interf, *m_impl->rx.pcaps[index], m_rx_descriptor))
        {
            stopBackend();
            return false;
        }

        if (m_tx_descriptor.interface == interf)
        {
            m_impl->tx.pcap = m_impl->rx.pcaps[index].get();
        }

        index++;
    }

    if (m_impl->tx.pcap == nullptr)
    {
        LOGE("Raw-broadcast TX interface {} is not available in reopened pcap set", m_tx_descriptor.interface);
        stopBackend();
        return false;
    }

    reset_rx_state();

    m_impl->tx.thread = std::thread([this]() { tx_thread_proc(); });
    for (size_t i = 0; i < m_rx_descriptor.interfaces.size(); i++)
    {
        m_impl->rx.threads.push_back(std::thread([this, i]() { rx_thread_proc(i); }));
    }

    return true;
}

//===================================================================================
//===================================================================================
// Closes raw-broadcast worker threads and stale pcap handles before another mode reuses the adapter.
void LinuxRawBroadcastTransport::stopBackend()
{
    if (!m_impl)
    {
        return;
    }

    m_exit = true;
    m_impl->tx.packet_queue_cv.notify_all();

    for (auto& thread : m_impl->rx.threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    m_impl->rx.threads.clear();

    if (m_impl->tx.thread.joinable())
    {
        m_impl->tx.thread.join();
    }

    for (auto& pcap : m_impl->rx.pcaps)
    {
        if (pcap && pcap->pcap != nullptr)
        {
            pcap_close(pcap->pcap);
            pcap->pcap = nullptr;
        }
    }
    m_impl->rx.pcaps.clear();
    m_impl->tx.pcap = nullptr;

    {
        std::lock_guard<std::mutex> lg(m_impl->tx.packet_queue_mutex);
        m_impl->tx.packet_queue.clear();
    }
    m_impl->tx.ready_packet_queue.clear();
    m_impl->tx.block_packets.clear();
    m_impl->tx.block_fec_packets.clear();
    m_impl->tx.crt_packet.reset();

}

//===================================================================================
//===================================================================================
// Waits for pcap RX readiness on one interface and drains packets when that interface descriptor becomes readable.
void LinuxRawBroadcastTransport::rx_thread_proc(size_t index)
{
    RX& rx = m_impl->rx;
    PCap& pcap = *rx.pcaps[index];

    while (!m_exit)
    {
        fd_set readset;
        struct timeval to;

        to.tv_sec = 0;
        to.tv_usec = 30000;

        FD_ZERO(&readset);
        FD_SET(pcap.rx_pcap_selectable_fd, &readset);

        int n = select(pcap.rx_pcap_selectable_fd + 1, &readset, nullptr, nullptr, &to);
        if (n < 0)
        {
            LOGW("Raw RX select failed on {} fd={} errno={} ({})",
                 m_rx_descriptor.interfaces[pcap.index],
                 pcap.rx_pcap_selectable_fd,
                 errno,
                 std::strerror(errno));
            continue;
        }
        if (n == 0)
        {
            continue;
        }
        if (FD_ISSET(pcap.rx_pcap_selectable_fd, &readset))
        {
            process_rx_packet(pcap);
        }
    }
}

//===================================================================================
//===================================================================================
void LinuxRawBroadcastTransport::tx_thread_proc()
{
    TX& tx = m_impl->tx;
    uint32_t coding_k = m_tx_descriptor.coding_k;
    uint32_t coding_n = m_tx_descriptor.coding_n;

#if 0 //TEST THROUGHPUT
     TX::Packet_ptr packet = tx.packet_pool.acquire();

     size_t s = std::min(8192u, m_transport_packet_size - packet->data.size());
     size_t offset = packet->data.size();
     packet->data.resize(offset + s);

     auto start = Clock::now();

     size_t packets = 0;
     size_t total_size = 0;

     while (!m_exit)
     {
         int isize = static_cast<int>(packet->data.size());

         std::lock_guard<std::mutex> lg(tx.pcap.mutex);
         int r = pcap_inject(tx.pcap.pcap, packet->data.data(), isize);
         if (r <= 0)
         {
             LOGW("Trouble injecting packet: {} / {}: {}", r, isize, pcap_geterr(tx.pcap.pcap));
             //result = Result::ERROR;
         }
         if (r > 0)
         {
             if (r != isize)
             {
                 LOGW("Incomplete packet sent: {} / {}", r, isize);
             }
             else
             {
                 packets++;
                 total_size += isize;
             }
         }

         auto now = Clock::now();
         if (now - start > std::chrono::seconds(1))
         {
             float f = std::chrono::duration<float>(now - start).count();
             start = now;

             LOGI("Packets: {}, Size: {}MB", packets / f, (total_size / (1024.f * 1024.f)) / f);
             packets = 0;
             total_size = 0;
         }
     }
#endif

    while (!m_exit)
    {
        {
            //wait for data
            std::unique_lock<std::mutex> lg(tx.packet_queue_mutex);
            if (tx.packet_queue.empty())
                tx.packet_queue_cv.wait(lg, [this, &tx] { return tx.packet_queue.empty() == false || m_exit == true; });

            if (m_exit)
                break;

            TX::Packet_ptr packet;
            if (!tx.packet_queue.empty())
            {
                packet = tx.packet_queue.front();
                tx.packet_queue.pop_front();
            }

            if (packet)
            {
                //m_packet_header_offset = RADIOTAP_HEADER.size() + sizeof(WLAN_IEEE_HEADER_GROUND2AIR);
                //packet->data() + m_packet_header_offset => Packet_Header
                seal_packet(m_packet_filter,
                            *packet,
                            m_packet_header_offset,
                            tx.last_block_index,
                            tx.block_packets.size());
                tx.ready_packet_queue.push_back(packet); //ready to send
                tx.block_packets.push_back(packet);
            }
        }

        //compute fec packets
        if (tx.block_packets.size() >= coding_k)
        {
            if (1)
            {
                //auto start = Clock::now();

                //init data for the fec_encode
                for (size_t i = 0; i < coding_k; i++)
                    tx.fec_src_packet_ptrs[i] = tx.block_packets[i]->data.data() + m_payload_offset;  //points to mtu

                size_t fec_count = coding_n - coding_k;
                tx.block_fec_packets.resize(fec_count);
                for (size_t i = 0; i < fec_count; i++)
                {
                    tx.block_fec_packets[i] = tx.packet_pool.acquire();
                    tx.block_fec_packets[i]->data.resize(tx.transport_packet_size);  //=RADIOTAP_HEADER.size() + sizeof(WLAN_IEEE_HEADER_GROUND2AIR) + Packet_Header + mtu
                    tx.fec_dst_packet_ptrs[i] = tx.block_fec_packets[i]->data.data() + m_payload_offset;  //points to mtu
                }

                //encode
                fec_encode(tx.fec, tx.fec_src_packet_ptrs.data(), tx.fec_dst_packet_ptrs.data(), fec_block_nums() + coding_k, coding_n - coding_k, tx.payload_size);

                //seal the result
                for (size_t i = 0; i < fec_count; i++)
                {
                    seal_packet(m_packet_filter,
                                *tx.block_fec_packets[i],
                                m_packet_header_offset,
                                tx.last_block_index,
                                coding_k + i);
                    tx.ready_packet_queue.push_back(tx.block_fec_packets[i]); //ready to send
                }

                //LOGI("Encoded fec: {}", Clock::now() - start);
            }

            tx.block_packets.clear();
            tx.block_fec_packets.clear();
            tx.last_block_index++;
        }

        while (!tx.ready_packet_queue.empty())
        {
            TX::Packet_ptr packet = tx.ready_packet_queue.front();
            tx.ready_packet_queue.pop_front();

            std::lock_guard<std::mutex> lg(tx.pcap->mutex);

            int isize = static_cast<int>(packet->data.size());
            int r = pcap_inject(tx.pcap->pcap, packet->data.data(), isize);
            //                    if (r <= 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            //                    {
            //                        break;
            //                    }
            //                    else
            {
                if (r <= 0)
                {
                    LOGW("Trouble injecting packet: {} / {}: {}", r, isize, pcap_geterr(tx.pcap->pcap));
                    //result = Result::ERROR;
                }
                if (r > 0 && r != isize)
                {
                    LOGW("Incomplete packet sent: {} / {}", r, isize);
                    //result = Result::ERROR;
                }
                else
                {
                    s_gs_stats.outPacketCounter++;
                }
            }
        }

        {
            //std::this_thread::sleep_for(std::chrono::milliseconds(1));
            //std::this_thread::yield();

#ifdef DEBUG_THROUGHPUT
            {
                static size_t xxx_data = 0;
                static size_t xxx_real_data = 0;
                static std::chrono::system_clock::time_point xxx_last_tp = std::chrono::system_clock::now();
                xxx_data += tx_buffer.size();
                xxx_real_data += AIR2GROUND_MAX_MTU;
                auto now = std::chrono::system_clock::now();
                if (now - xxx_last_tp >= std::chrono::seconds(1))
                {
                    float r = std::chrono::duration<float>(now - xxx_last_tp).count();
                    LOGI("Sent: {} KB/s / {} KB/s", float(xxx_data) / r / 1024.f, float(xxx_real_data) / r / 1024.f);
                    xxx_data = 0;
                    xxx_real_data = 0;
                    xxx_last_tp = now;
                }
            }
#endif
        }
    }
}

//===================================================================================
//===================================================================================
//appends data to current packet
//or appends and flushes
//currently flush=true is always used
//because we want the data headers to be on the beginning of each block
void LinuxRawBroadcastTransport::send(void const* _data, size_t size, bool flush)
{
    TX& tx = m_impl->tx;

    TX::Packet_ptr& packet = tx.crt_packet;

    uint8_t const* data = reinterpret_cast<uint8_t const*>(_data);

    while (size > 0)
    {
        if (!packet)
            packet = tx.packet_pool.acquire();

        size_t s = std::min(size, tx.transport_packet_size - packet->data.size());
        size_t offset = packet->data.size();
        packet->data.resize(offset + s);
        memcpy(packet->data.data() + offset, data, s);
        data += s;
        size -= s;

        if (packet->data.size() >= tx.transport_packet_size || flush)
        {
            if (packet->data.size() < tx.transport_packet_size)
                packet->data.resize(tx.transport_packet_size);

            //send the current packet
            {
                std::unique_lock<std::mutex> lg(tx.packet_queue_mutex);
                tx.packet_queue.push_back(packet);
            }
            packet = tx.packet_pool.acquire();

            tx.packet_queue_cv.notify_all();
        }
    }
}

//===================================================================================
//===================================================================================
size_t LinuxRawBroadcastTransport::get_data_rate() const
{
    return m_data_stats_rate;
}

//===================================================================================
//===================================================================================
int LinuxRawBroadcastTransport::get_input_dBm() const
{
    return m_latched_input_dBm;
}

//===================================================================================
//===================================================================================
void LinuxRawBroadcastTransport::reset_rx_state()
{
    if (!m_impl)
        return;

    Clock::time_point now = Clock::now();
    m_impl->rx.decoder.reset(now);
    sync_rx_decoder_stats();

    m_data_stats_data_accumulated = 0;
    m_data_stats_last_tp = now;
    m_last_rx_decoded_bytes_total = 0;
}

//===================================================================================
//===================================================================================
void LinuxRawBroadcastTransport::process_rx_packets()
{
    m_impl->rx.decoder.process(Clock::now());
    sync_rx_decoder_stats();
}

//===================================================================================
//===================================================================================
bool LinuxRawBroadcastTransport::receive(void* data, size_t& size, bool& restoredByFEC)
{
    return m_impl->rx.decoder.receive(data, size, restoredByFEC);
}

//===================================================================================
//===================================================================================
void LinuxRawBroadcastTransport::process()
{
    if (!m_impl)
        return;

    process_rx_packets();

    if (m_best_input_dBm != std::numeric_limits<int>::lowest())
        m_latched_input_dBm = m_best_input_dBm.load();
    m_best_input_dBm = std::numeric_limits<int>::lowest();

    Clock::time_point now = Clock::now();

    if (now - m_impl->rx.decoder.lastBlockTime() > std::chrono::seconds(2))
        m_latched_input_dBm = 0;

    if (now - m_data_stats_last_tp >= std::chrono::seconds(1))
    {
        float d = std::chrono::duration<float>(now - m_data_stats_last_tp).count();
        m_data_stats_rate = static_cast<size_t>(static_cast<float>(m_data_stats_data_accumulated) / d);
        m_data_stats_data_accumulated = 0;
        m_data_stats_last_tp = now;
    }
}

void LinuxRawBroadcastTransport::sync_rx_decoder_stats()
{
    const FecBlockDecoder::Stats stats = m_impl->rx.decoder.getStats();
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
        m_data_stats_data_accumulated += static_cast<size_t>(stats.decoded_bytes_total - m_last_rx_decoded_bytes_total);
    }
    m_last_rx_decoded_bytes_total = stats.decoded_bytes_total;
}

//===================================================================================
//===================================================================================
void LinuxRawBroadcastTransport::setChannel(int ch)
{
    for (const auto& itf:m_rx_descriptor.interfaces)  //the list contains both RX and TX interfaces
    {
        setMonitorChannel(itf, ch);
    }
}

//===================================================================================
//===================================================================================
void LinuxRawBroadcastTransport::setTxPower(int txPower)
{
    //iw dev wlan1 set txpower fixed -4500
    std::string s = fmt::format("iw dev {} set txpower fixed {}", m_tx_descriptor.interface, -(txPower * 100) );
    LOGI("Setting TX power with command: {}", s);
    runShellCommand(s);
}

//===================================================================================
//===================================================================================
void LinuxRawBroadcastTransport::setMonitorMode(const std::vector<std::string> interfaces)
{
    for (const auto& itf:interfaces)
    {
        LOGI("Setting monitor mode on {}", itf);
        runShellCommand(fmt::format("ip link set {} down", itf));
        runShellCommand(fmt::format("iw dev {} set type monitor", itf));
        runShellCommand(fmt::format("ip link set {} up", itf));
        if (!waitForInterfaceUp(itf))
        {
            LOGW("Interface {} did not report up after switching to monitor mode", itf);
        }
    }
}

//===================================================================================
//===================================================================================
PacketFilter& LinuxRawBroadcastTransport::getPacketFilter()
{
    return m_packet_filter;
}

//===================================================================================
//===================================================================================
const LinuxRawBroadcastTransport::RX_Descriptor& LinuxRawBroadcastTransport::getRXDescriptor() const
{
    return this->m_rx_descriptor;
}

//===================================================================================
//===================================================================================
void LinuxRawBroadcastTransport::setTxInterface(const std::string& interface)
{
    std::lock_guard<std::mutex> lg( m_impl->tx.pcap->mutex );

    for ( size_t i = 0; i < m_rx_descriptor.interfaces.size(); ++i ) 
    {
        if ( m_rx_descriptor.interfaces[i] == interface ) 
        {
            m_impl->tx.pcap = m_impl->rx.pcaps[i].get(); 
            m_tx_descriptor.interface = interface;
            break;
        }
    }
    //Note: Do set TX power on new TX interface
}
