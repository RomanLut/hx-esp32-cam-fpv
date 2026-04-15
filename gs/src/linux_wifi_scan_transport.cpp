#include "linux_wifi_scan_transport.h"

#include <pcap.h>
#include <chrono>
#include <endian.h>

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
// Sets the monitor-mode channel on a Linux interface using iw (falls back to iwconfig).
void setMonitorChannelIw(const std::string& interface, int channel)
{
    if (channel <= 0)
    {
        return;
    }

    if (!runShellCommand(fmt::format("iw dev {} set channel {} HT20", interface, channel)))
    {
        LOGW("WifiScan: iw set channel {} failed on {}, trying iwconfig", channel, interface);
        runShellCommand(fmt::format("iwconfig {} channel {}", interface, channel));
    }
}

} // namespace

//===================================================================================
//===================================================================================
LinuxWifiScanTransport::~LinuxWifiScanTransport()
{
    m_captureStop = true;
    if (m_captureThread.joinable())
    {
        m_captureThread.join();
    }
    closeMonitorPcap();
}

//===================================================================================
//===================================================================================
// Opens a pcap capture handle on the given interface in monitor mode.
bool LinuxWifiScanTransport::openMonitorPcap(const std::string& interface)
{
    char error_buf[PCAP_ERRBUF_SIZE] = {};

    LOGI("WifiScan: bringing {} down", interface);
    runShellCommand(fmt::format("ip link set {} down", interface));
    LOGI("WifiScan: setting {} to monitor mode", interface);
    runShellCommand(fmt::format("iw dev {} set type monitor", interface));
    LOGI("WifiScan: bringing {} up", interface);
    runShellCommand(fmt::format("ip link set {} up", interface));

    // Brief settle time so the interface reports up before pcap_create.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Verify interface is actually in monitor mode
    runShellCommand(fmt::format("iw dev {} info", interface));

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
    pcap_set_timeout(m_pcap, 100);       // 100 ms so the capture thread wakes to check stop flag
    pcap_set_buffer_size(m_pcap, 4000000);

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

    pcap_setdirection(m_pcap, PCAP_D_IN);

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
void LinuxWifiScanTransport::deactivate()
{
    m_captureStop = true;
    if (m_captureThread.joinable())
    {
        m_captureThread.join();
    }
    closeMonitorPcap();
    GSWifiScanTransport::deactivate();
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
        // pcap_dispatch with cnt=-1 returns after one buffer-worth of packets or
        // the read timeout.  We use a 100 ms timeout so the stop flag is checked
        // frequently without spinning.
        const int n = pcap_dispatch(m_pcap, -1,
                                    &LinuxWifiScanTransport::packetCallback,
                                    reinterpret_cast<u_char*>(this));

        if (n < 0)
        {
            LOGE("WifiScan: pcap_dispatch error: {}", pcap_geterr(m_pcap));
            break;
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
    if (!m_interface.empty())
    {
        setMonitorChannelIw(m_interface, channel);
    }
}
