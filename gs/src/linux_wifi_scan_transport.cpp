#include "linux_wifi_scan_transport.h"

#include <pcap.h>
#include <chrono>
#include <cstdio>
#include <endian.h>
#include <sstream>

#include "Log.h"
#include "utils.h"
#include "radiotap/radiotap.h"

namespace
{

//===================================================================================
//===================================================================================
// Returns the centre frequency in MHz for an 802.11 channel number.
int channelToFreqMHz(int channel)
{
    if (channel == 14)                      return 2484;
    if (channel >= 1  && channel <= 13)     return 2407 + channel * 5;
    if (channel >= 36 && channel <= 177)    return 5000 + channel * 5;
    return 0;
}

//===================================================================================
//===================================================================================
// Reports whether Linux exposes one interface in monitor mode.
bool monitorModeMatches(const std::string& interface)
{
    std::string output;
    return runShellCommand(fmt::format("iw dev {} info", interface), &output) &&
           output.find("type monitor") != std::string::npos;
}

//===================================================================================
//===================================================================================
// Reports whether Linux exposes the requested channel for one monitor interface.
bool monitorChannelMatches(const std::string& interface, int expected_channel)
{
    std::string output;
    if (!runShellCommand(fmt::format("iw dev {} info", interface), &output))
    {
        return false;
    }

    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line))
    {
        int reported_channel = 0;
        if (std::sscanf(line.c_str(), " channel %d", &reported_channel) == 1)
        {
            return reported_channel == expected_channel;
        }
    }

    return false;
}

} // namespace

//===================================================================================
//===================================================================================
// Stops capture and releases the pcap handle during object destruction.
LinuxWifiScanTransport::~LinuxWifiScanTransport()
{
    stopCaptureThread();
    closeMonitorPcap();
}

//===================================================================================
//===================================================================================
// Configures monitor mode once and opens the persistent pcap handle used for all scan hops.
bool LinuxWifiScanTransport::openMonitorPcap(const std::string& interface)
{
    char error_buf[PCAP_ERRBUF_SIZE] = {};

    // RAW already leaves the receive radios in monitor mode. Repeating link-down/up
    // for every scan channel wedges rtl88x2eu reception and makes mode changes take
    // tens of seconds, so only change mode when the interface genuinely needs it.
    if (!monitorModeMatches(interface))
    {
        LOGI("WifiScan: configuring {} for monitor capture", interface);
        runShellCommand(fmt::format("ip link set {} down", interface));
        runShellCommand(fmt::format("iw dev {} set type monitor", interface));
        runShellCommand(fmt::format("ip link set {} up", interface));
    }
    else
    {
        LOGI("WifiScan: {} is already in monitor mode", interface);
    }

    m_pcap = pcap_create(interface.c_str(), error_buf);
    if (m_pcap == nullptr)
    {
        LOGE("WifiScan: pcap_create failed on {}: {}", interface, error_buf);
        return false;
    }

    pcap_set_snaplen(m_pcap, 128);       // enough to capture the full radiotap header for rate parsing
    pcap_set_promisc(m_pcap, 1);
    // Do NOT call pcap_set_rfmon() — we already put the interface into monitor mode
    // via 'iw dev set type monitor' above.  Asking pcap to enable RFMON itself fails
    // on some WSL / out-of-tree drivers even though the interface is already in
    // monitor mode (pcap_activate returns PCAP_ERROR_RFMON_NOTSUP = -6).
    pcap_set_timeout(m_pcap, 100);       // limits packet-buffer latency while traffic is arriving
    pcap_set_buffer_size(m_pcap, 4000000);
    if (pcap_set_immediate_mode(m_pcap, 1) < 0)
    {
        LOGE("WifiScan: failed to enable immediate pcap mode on {}: {}", interface, pcap_geterr(m_pcap));
        closeMonitorPcap();
        return false;
    }

    const int activate_ret = pcap_activate(m_pcap);
    if (activate_ret < 0)
    {
        LOGE("WifiScan: pcap_activate failed on {} ({}): {}", interface, activate_ret, pcap_geterr(m_pcap));
        closeMonitorPcap();
        return false;
    }
    if (activate_ret > 0)
    {
        LOGW("WifiScan: pcap_activate on {} returned warning ({}): {}", interface, activate_ret, pcap_geterr(m_pcap));
    }

    LOGI("WifiScan: pcap_activate OK on {} (ret={}), datalink={}", interface, activate_ret, pcap_datalink(m_pcap));

    // pcap_breakloop does not reliably wake a blocked rtl88x2eu dispatch after
    // channel reconfiguration. Nonblocking dispatch makes every hop and mode
    // transition bounded; the capture loop sleeps briefly while no data is ready.
    if (pcap_setnonblock(m_pcap, 1, error_buf) < 0)
    {
        LOGE("WifiScan: failed to make pcap nonblocking on {}: {}", interface, error_buf);
        closeMonitorPcap();
        return false;
    }

    // rtl88x2eu can classify frames received over the air as outgoing monitor
    // frames. RAW capture therefore cannot use PCAP_D_IN, and scan capture must
    // follow the same rule or its activity graph stays empty during real traffic.

    LOGI("WifiScan: monitor pcap open on {}", interface);
    return true;
}

//===================================================================================
//===================================================================================
void LinuxWifiScanTransport::closeMonitorPcap()
{
    if (m_pcap != nullptr)
    {
        pcap_close(m_pcap);
        m_pcap = nullptr;
    }
}

//===================================================================================
//===================================================================================
bool LinuxWifiScanTransport::init(const gs::core::RXDescriptor& rx_descriptor,
                                   const gs::core::TXDescriptor& tx_descriptor)
{
    m_interface = rx_descriptor.interfaces.empty() ? "" : rx_descriptor.interfaces.front();
    if (!m_interface.empty())
    {
        if (openMonitorPcap(m_interface))
        {
            m_captureStop = false;
            m_captureThread = std::thread(&LinuxWifiScanTransport::captureThreadFunc, this);
        }
    }
    else
    {
        LOGW("WifiScan: no RX interface configured; packet counting disabled");
    }

    return GSWifiScanTransport::init(rx_descriptor, tx_descriptor);
}

//===================================================================================
//===================================================================================
// Stops packet capture and releases its handle when this transport is deselected.
void LinuxWifiScanTransport::deactivate()
{
    stopCaptureThread();
    closeMonitorPcap();
    GSWifiScanTransport::deactivate();
}

//===================================================================================
//===================================================================================
// Wakes and joins the pcap capture thread before its handle is closed.
void LinuxWifiScanTransport::stopCaptureThread()
{
    m_captureStop = true;

    // A pcap packet-buffer timeout is not guaranteed to expire while an interface is
    // idle. Wake pcap_dispatch explicitly or a transport switch can block forever in join().
    if (m_pcap != nullptr)
    {
        pcap_breakloop(m_pcap);
    }

    if (m_captureThread.joinable())
    {
        m_captureThread.join();
    }
}

//===================================================================================
//===================================================================================
// Background thread: blocks in pcap_dispatch() and counts every arriving packet.
void LinuxWifiScanTransport::captureThreadFunc()
{
    LOGI("WifiScan: capture thread started");
    int total = 0;
    auto last_log = std::chrono::steady_clock::now();

    while (!m_captureStop)
    {
        // pcap_dispatch with cnt=-1 returns after one buffer-worth of packets. The
        // stop path uses pcap_breakloop because an idle Linux capture need not honor
        // the packet-buffer timeout until at least one packet arrives.
        const int n = pcap_dispatch(m_pcap, -1,
                                    &LinuxWifiScanTransport::packetCallback,
                                    reinterpret_cast<u_char*>(this));

        if (n == PCAP_ERROR_BREAK && m_captureStop)
        {
            break;
        }

        if (n < 0)
        {
            LOGE("WifiScan: pcap_dispatch error: {}", pcap_geterr(m_pcap));
            break;
        }

        if (n == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        total += n;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(5))
        {
            LOGI("WifiScan: capture thread alive, {} packets in last 5s", total);
            total = 0;
            last_log = now;
        }
    }

    LOGI("WifiScan: capture thread stopped");
}

//===================================================================================
//===================================================================================
// pcap packet callback: parses the radiotap header to determine the data rate,
// then accumulates airtime (in microseconds) for each received frame.
//
// Airtime calculation:
//   airtime_us = (frame_bytes_on_air * 8) / rate_kbps
//              = frame_bytes * 16 / rate_500kbps
//
// "frame_bytes_on_air" is the original on-wire length minus the radiotap header
// (and minus 4 if the FCS is included in the reported length per FLAGS field).
//
// Rate is taken from the RATE field (legacy 802.11a/b/g, units of 500 kb/s) or
// from the MCS field (HT, converted to equivalent 500 kb/s units for HT20 LGI/SGI).
// If neither is present, a conservative 6 Mbps fallback is used.
void LinuxWifiScanTransport::packetCallback(u_char* user,
                                            const struct pcap_pkthdr* h,
                                            const u_char* bytes)
{
    auto* self = reinterpret_cast<LinuxWifiScanTransport*>(user);

    // Need at least the 8-byte fixed radiotap header to read it_len.
    if (h->caplen < 8)
    {
        // Fallback: treat as one minimal-airtime frame (6 Mbps, 50-byte frame ≈ 66 µs)
        self->m_airtimeUs.fetch_add(66, std::memory_order_relaxed);
        return;
    }

    const auto* rthdr = reinterpret_cast<const ieee80211_radiotap_header*>(bytes);
    const uint16_t rt_len = le16toh(rthdr->it_len);

    // Sanity: radiotap header must fit within captured length and on-wire length.
    if (rt_len < 8 || rt_len > h->caplen || rt_len > h->len)
    {
        self->m_airtimeUs.fetch_add(66, std::memory_order_relaxed);
        return;
    }

    // On-air frame size: original length minus radiotap header.
    uint32_t frame_bytes = h->len - rt_len;

    // HT20 LGI data rates indexed by MCS 0..7, in units of 500 kb/s.
    // (6.5, 13, 19.5, 26, 39, 52, 58.5, 65 Mbps → × 2 = 13, 26, 39, 52, 78, 104, 117, 130)
    static const uint8_t kHT20LGI_500kbps[8] = { 13, 26, 39, 52, 78, 104, 117, 130 };

    uint32_t rate_500kbps = 0;
    bool has_fcs = false;
    int packet_freq_mhz = 0;

    struct ieee80211_radiotap_iterator iter;
    if (ieee80211_radiotap_iterator_init(&iter,
            const_cast<ieee80211_radiotap_header*>(rthdr),
            static_cast<int>(h->caplen)) == 0)
    {
        while (ieee80211_radiotap_iterator_next(&iter) == 0)
        {
            switch (iter.this_arg_index)
            {
            case IEEE80211_RADIOTAP_FLAGS:
                if (*iter.this_arg & IEEE80211_RADIOTAP_F_FCS)
                    has_fcs = true;
                break;

            case IEEE80211_RADIOTAP_CHANNEL:
            {
                // Two le16 fields: frequency (MHz), then channel flags.
                uint16_t freq;
                std::memcpy(&freq, iter.this_arg, sizeof(freq));
                packet_freq_mhz = static_cast<int>(le16toh(freq));
                break;
            }

            case IEEE80211_RADIOTAP_RATE:
                // Legacy rate field: u8 in units of 500 kb/s.
                rate_500kbps = *iter.this_arg;
                break;

            case IEEE80211_RADIOTAP_MCS:
            {
                // Three bytes: known, flags, mcs_index.
                const uint8_t known = iter.this_arg[0];
                const uint8_t flags = iter.this_arg[1];
                const uint8_t mcs   = iter.this_arg[2] & 0x7f;
                if (mcs < 8)
                {
                    rate_500kbps = kHT20LGI_500kbps[mcs];
                    // Apply SGI correction (+11%) if the driver reports it.
                    if ((known & IEEE80211_RADIOTAP_MCS_HAVE_GI) &&
                        (flags & IEEE80211_RADIOTAP_MCS_SGI))
                    {
                        rate_500kbps = rate_500kbps * 10 / 9;
                    }
                }
                break;
            }

            default:
                break;
            }
        }
    }

    // Drop packets that leaked from a different channel.
    // Compare against the frequency we set; allow a small ±2 MHz tolerance for rounding.
    const int target_freq = self->m_channelFreqMHz.load(std::memory_order_relaxed);
    if (target_freq != 0 && packet_freq_mhz != 0 &&
        std::abs(packet_freq_mhz - target_freq) > 2)
    {
        return;
    }

    // Subtract FCS (4 bytes) from the on-air length if included.
    if (has_fcs && frame_bytes >= 4)
        frame_bytes -= 4;

    // rate_500kbps == 0 → accumulateAirtime falls back to 6 Mbps.
    self->accumulateAirtime(frame_bytes, rate_500kbps);
}

//===================================================================================
//===================================================================================
void LinuxWifiScanTransport::setMonitorChannel(int channel)
{
    LOGI("WifiScan: switching to channel {}", channel);
    m_channelFreqMHz.store(channelToFreqMHz(channel), std::memory_order_relaxed);

    if (!m_interface.empty() && !monitorChannelMatches(m_interface, channel))
    {
        // rtl88x2eu returns success from `iw set channel` without always applying
        // the requested channel. Wireless Extensions applies the retune reliably;
        // verify the reported state so the graph label cannot diverge from the radio.
        if (!runShellCommand(fmt::format("iwconfig {} channel {}", m_interface, channel)) ||
            !monitorChannelMatches(m_interface, channel))
        {
            LOGE("WifiScan: failed to apply channel {} on {}", channel, m_interface);
        }
    }

}
