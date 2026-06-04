#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "gs_wifi_scan_transport.h"
#include "devourer/src/RtlJaguarDevice.h"
#include "devourer/src/WiFiDriver.h"
#include "devourer/src/logger.h"

struct libusb_context;
struct libusb_device_handle;

//===================================================================================
//===================================================================================
// Android implementation of the WiFi channel scan transport.
//
// Uses the RTL8812AU userspace driver (Devourer) to count raw 802.11 packets in
// monitor mode.  A packet callback is registered on the device; the callback
// simply increments an atomic counter that consumeReceivedPacketCount() reads and
// resets each second.  Channel switching uses SetMonitorChannel() on the device.
//
// The transport must be activated via startUsbAdapter(fd) after init(), exactly
// like AndroidRawBroadcastTransport. Worker threads are owned and joined during
// teardown because Android USB hot-unplug can invalidate libusb/device state at
// any time; detached threads here would race freed state and crash the app.
class AndroidWifiScanTransport final : public GSWifiScanTransport
{
public:
    ~AndroidWifiScanTransport() override;

    bool init(const gs::core::RXDescriptor& rx_descriptor,
              const gs::core::TXDescriptor& tx_descriptor) override;
    void activate() override;
    void deactivate() override;
    std::string getTransportMessage() const override;

    // Open the RTL8812AU device on the given USB file descriptor.
    bool startUsbAdapter(int fd);
    void stopUsbAdapter();
    bool isUsbAdapterRunning() const;
    int  activeUsbFd() const;

protected:
    void setMonitorChannel(int channel) override;

    void channelSwitchLoop();

private:
    std::atomic<bool>           m_active          = {false};
    Clock::time_point           m_activate_time   = Clock::time_point::min();
    mutable std::mutex          m_mutex;
    std::mutex                  m_stop_mutex;
    std::shared_ptr<RtlJaguarDevice> m_device;
    std::unique_ptr<WiFiDriver> m_wifi_driver;
    Logger_t                    m_devourer_logger;
    Clock::time_point           m_last_adapter_transition_time = Clock::time_point::min();
    std::unique_ptr<std::thread> m_usb_event_thread;
    std::unique_ptr<std::thread> m_rx_thread;
    libusb_context*             m_libusb_context  = nullptr;
    libusb_device_handle*       m_usb_handle      = nullptr;
    int                         m_active_usb_fd   = -1;
    std::atomic<int>            m_nextChannel     = {0};
    Clock::time_point           m_channel_switch_ready_time = Clock::time_point::max();
    std::atomic<bool>           m_chSwitchStop    = {false};
    std::thread                 m_chSwitchThread;
};
