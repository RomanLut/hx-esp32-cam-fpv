#include "android_wifi_scan_transport.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <thread>

#include <libusb.h>

#include "Log.h"
#include "gs_shared_state.h"
#include "devourer/src/SelectedChannel.h"

namespace
{

//===================================================================================
//===================================================================================
// Converts a GS channel number into the RTL driver channel selection descriptor.
SelectedChannel makeSelectedChannel(int channel)
{
    SelectedChannel sc = {};
    sc.Channel      = static_cast<uint8_t>(std::clamp(channel, 1, 165));
    sc.ChannelOffset = 0;
    sc.ChannelWidth  = CHANNEL_WIDTH_20;
    return sc;
}

} // namespace

//===================================================================================
//===================================================================================
AndroidWifiScanTransport::~AndroidWifiScanTransport()
{
    stopUsbAdapter();
}

//===================================================================================
//===================================================================================
// Initialises the devourer WiFi driver wrapper, then calls the shared base init.
bool AndroidWifiScanTransport::init(const gs::core::RXDescriptor& rx_descriptor,
                                    const gs::core::TXDescriptor& tx_descriptor)
{
    m_devourer_logger = std::make_shared<Logger>();
    m_wifi_driver     = std::make_unique<WiFiDriver>(m_devourer_logger);

    return GSWifiScanTransport::init(rx_descriptor, tx_descriptor);
}

//===================================================================================
//===================================================================================
void AndroidWifiScanTransport::activate()
{
    m_activate_time = Clock::now();
    m_active = true;
}

//===================================================================================
//===================================================================================
void AndroidWifiScanTransport::deactivate()
{
    m_active = false;
    stopUsbAdapter();
    GSWifiScanTransport::deactivate();
}

//===================================================================================
//===================================================================================
std::string AndroidWifiScanTransport::getTransportMessage() const
{
    if (m_active && !isUsbAdapterRunning())
    {
        using namespace std::chrono_literals;
        const Clock::time_point now = Clock::now();
        const Clock::time_point graceStart =
            (m_last_adapter_transition_time > m_activate_time)
                ? m_last_adapter_transition_time
                : m_activate_time;
        if (now - graceStart < 8s)
        {
            return "Initializing USB Wifi adapter...";
        }
        else
        {
            return "RTL88XXAU USB ADAPTER NOT FOUND!";
        }
    }
    return {};
}

//===================================================================================
//===================================================================================
// Opens the RTL8812AU device on the given USB file descriptor and starts the
// receive thread.  The receive callback simply increments the packet counter.
bool AndroidWifiScanTransport::startUsbAdapter(int fd)
{
    if (fd < 0)
    {
        LOGE("Refusing to start adapter with invalid fd={}", fd);
        return false;
    }

    stopUsbAdapter();

    libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    if (libusb_init(&m_libusb_context) < 0)
    {
        LOGE("libusb_init failed");
        m_libusb_context = nullptr;
        return false;
    }

    if (libusb_wrap_sys_device(m_libusb_context,
                               static_cast<intptr_t>(fd),
                               &m_usb_handle) < 0)
    {
        LOGE("libusb_wrap_sys_device failed fd={}", fd);
        libusb_exit(m_libusb_context);
        m_libusb_context = nullptr;
        m_usb_handle     = nullptr;
        return false;
    }

    if (libusb_kernel_driver_active(m_usb_handle, 0) == 1)
    {
        libusb_detach_kernel_driver(m_usb_handle, 0);
    }

    if (libusb_claim_interface(m_usb_handle, 0) < 0)
    {
        LOGE("libusb_claim_interface failed");
        libusb_exit(m_libusb_context);
        m_libusb_context = nullptr;
        m_usb_handle     = nullptr;
        return false;
    }

    std::unique_ptr<RtlJaguarDevice> created_device =
        m_wifi_driver->CreateRtlDevice(m_usb_handle);
    if (!created_device)
    {
        LOGE("CreateRtlDevice failed");
        libusb_release_interface(m_usb_handle, 0);
        libusb_exit(m_libusb_context);
        m_libusb_context = nullptr;
        m_usb_handle     = nullptr;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device        = std::shared_ptr<RtlJaguarDevice>(created_device.release());
        m_active_usb_fd = fd;
    }

    LOGI("Started RTL adapter fd={}", fd);

    // USB event pump thread.
    // Android can report a USB detach while libusb is still servicing callbacks,
    // so teardown must join this thread before releasing libusb/device state.
    m_usb_event_thread = std::make_unique<std::thread>([this]()
    {
        while (true)
        {
            std::shared_ptr<RtlJaguarDevice> dev;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                dev = m_device;
            }
            if (!dev || dev->should_stop)
            {
                break;
            }
            timeval timeout = {};
            timeout.tv_usec = 500000;
            const int rc = libusb_handle_events_timeout(m_libusb_context, &timeout);
            if (rc < 0)
            {
                LOGW("libusb_handle_events_timeout rc={}", rc);
            }
        }
    });

    // Receive thread: accumulate airtime from every received frame.
    m_rx_thread = std::make_unique<std::thread>([this]()
    {
        std::shared_ptr<RtlJaguarDevice> dev;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            dev = m_device;
        }
        if (!dev)
        {
            return;
        }

        try
        {
            const int initial_channel = s_groundstation_config.wifi_channel;
            dev->Init(
                [this](const Packet& packet)
                {
                    const uint32_t frame_bytes = static_cast<uint32_t>(packet.RxAtrib.pkt_len);
                    if (frame_bytes == 0 || frame_bytes > 4096)
                    {
                        return;
                    }

                    // Decode hardware descriptor rate to 500 kb/s units.
                    // CCK  DESC_RATE1M(0x00)..DESC_RATE11M(0x03)
                    // OFDM DESC_RATE6M(0x04)..DESC_RATE54M(0x0b)
                    // HT   DESC_RATEMCS0(0x0c)..DESC_RATEMCS7(0x13)
                    static constexpr uint8_t kLegacy_500kbps[12] = {
                        2, 4, 11, 22,               // CCK:  1/2/5.5/11 Mbps
                        12, 18, 24, 36, 48, 72, 96, 108  // OFDM: 6/9/12/18/24/36/48/54 Mbps
                    };
                    static constexpr uint16_t kHT20LGI_500kbps[8] = { 13, 26, 39,  52,  78,  104, 117, 130 };
                    static constexpr uint16_t kHT40LGI_500kbps[8] = { 27, 54, 81, 108, 162,  216, 243, 270 };

                    const uint8_t dr = packet.RxAtrib.data_rate;
                    uint32_t rate_500kbps;
                    if (dr < 0x0c)
                    {
                        rate_500kbps = kLegacy_500kbps[dr];
                    }
                    else if (dr <= 0x13)
                    {
                        const uint8_t mcs = dr - 0x0c;
                        rate_500kbps = (packet.RxAtrib.bw != 0)
                                       ? kHT40LGI_500kbps[mcs]
                                       : kHT20LGI_500kbps[mcs];
                        if (packet.RxAtrib.sgi)
                            rate_500kbps = rate_500kbps * 10 / 9;
                    }
                    else
                    {
                        rate_500kbps = 0; // unknown; accumulateAirtime falls back to 6 Mbps
                    }

                    accumulateAirtime(frame_bytes, rate_500kbps);
                },
                makeSelectedChannel(initial_channel));
        }
        catch (const std::exception& ex)
        {
            LOGE("wifi-scan RX thread stopped: {}", ex.what());
            m_chSwitchStop.store(true);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_last_adapter_transition_time = Clock::now();
            if (m_device)
            {
                m_device->should_stop = true;
            }
            m_device = nullptr;
            m_active_usb_fd = -1;
        }
        catch (...)
        {
            LOGE("wifi-scan RX thread stopped with unknown exception");
            m_chSwitchStop.store(true);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_last_adapter_transition_time = Clock::now();
            if (m_device)
            {
                m_device->should_stop = true;
            }
            m_device = nullptr;
            m_active_usb_fd = -1;
        }
    });

    // Devourer Init() is blocking and has no queryable "monitor mode ready"
    // state. Delay external retunes long enough for its initial channel setup
    // to finish, otherwise SetMonitorChannel() can race driver bring-up.
    m_channel_switch_ready_time = Clock::now() + std::chrono::seconds(1);

    // Channel-switch thread: applies m_nextChannel requests asynchronously so
    // that setMonitorChannel() never blocks the GS processing / render thread.
    m_chSwitchStop.store(false);
    m_nextChannel.store(0);
    m_chSwitchThread = std::thread(&AndroidWifiScanTransport::channelSwitchLoop, this);

    return true;
}

//===================================================================================
//===================================================================================
void AndroidWifiScanTransport::channelSwitchLoop()
{
    int appliedChannel = 0;

    while (!m_chSwitchStop.load(std::memory_order_relaxed))
    {
        const Clock::time_point now = Clock::now();
        const int wanted = m_nextChannel.load(std::memory_order_relaxed);
        if (now < m_channel_switch_ready_time)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (wanted != 0 && wanted != appliedChannel)
        {
            std::shared_ptr<RtlJaguarDevice> dev;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                dev = m_device;
            }
            if (dev && !dev->should_stop)
            {
                try
                {
                    // Hot-unplug can race a pending retune request. Devourer throws
                    // std::ios_base::failure when register reads fail on the dead
                    // USB handle, so stop scanning instead of aborting the process.
                    dev->SetMonitorChannel(makeSelectedChannel(wanted));
                    appliedChannel = wanted;
                }
                catch (const std::exception& ex)
                {
                    LOGW("SetMonitorChannel failed after USB detach: {}", ex.what());
                    dev->should_stop = true;
                    break;
                }
                catch (...)
                {
                    LOGW("SetMonitorChannel failed after USB detach with unknown exception");
                    dev->should_stop = true;
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

//===================================================================================
//===================================================================================
void AndroidWifiScanTransport::stopUsbAdapter()
{
    std::lock_guard<std::mutex> stopLock(m_stop_mutex);
    const std::thread::id currentThreadId = std::this_thread::get_id();

    // Stop the channel-switch thread first (it doesn't hold the mutex long).
    m_chSwitchStop.store(true);
    if (m_chSwitchThread.joinable())
    {
        if (m_chSwitchThread.get_id() != currentThreadId)
        {
            m_chSwitchThread.join();
        }
        else
        {
            LOGW("Skipping self-join of channel switch thread during stop");
        }
    }

    std::shared_ptr<RtlJaguarDevice> dev;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        dev = m_device;
        if (dev)
        {
            m_last_adapter_transition_time = Clock::now();
            dev->should_stop = true;
        }
    }

    if (m_rx_thread && m_rx_thread->joinable())
    {
        if (m_rx_thread->get_id() != currentThreadId)
        {
            m_rx_thread->join();
        }
        else
        {
            LOGW("Skipping self-join of RX thread during stop");
        }
    }
    if (m_usb_event_thread && m_usb_event_thread->joinable())
    {
        if (m_usb_event_thread->get_id() != currentThreadId)
        {
            m_usb_event_thread->join();
        }
        else
        {
            LOGW("Skipping self-join of USB event thread during stop");
        }
    }
    m_rx_thread.reset();
    m_usb_event_thread.reset();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device        = nullptr;
        m_active_usb_fd = -1;
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

    LOGI("Stopped RTL adapter");
}

//===================================================================================
//===================================================================================
bool AndroidWifiScanTransport::isUsbAdapterRunning() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_device != nullptr;
}

//===================================================================================
//===================================================================================
int AndroidWifiScanTransport::activeUsbFd() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_active_usb_fd;
}

//===================================================================================
//===================================================================================
// Non-blocking: just store the target channel; channelSwitchLoop() applies it.
void AndroidWifiScanTransport::setMonitorChannel(int channel)
{
    m_nextChannel.store(channel, std::memory_order_relaxed);
}
