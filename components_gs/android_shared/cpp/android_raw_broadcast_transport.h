#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "fec_block_decoder.h"
#include "core/transport_base.h"
#include "devourer/src/Rtl8812aDevice.h"
#include "devourer/src/WiFiDriver.h"
#include "devourer/src/logger.h"

struct libusb_context;
struct libusb_device_handle;

//===================================================================================
//===================================================================================
// Implements the Android raw-broadcast transport using the RTL8812AU userspace driver.
class AndroidRawBroadcastTransport final : public gs::core::TransportBase
{
public:
    ~AndroidRawBroadcastTransport() override;
    bool init(const gs::core::RXDescriptor& rx_descriptor,
              const gs::core::TXDescriptor& tx_descriptor) override;
    void activate() override;
    void deactivate() override;
    bool requestImmediateReconnect() override;
    bool usesChannelSearch() const override;
    bool supportsMenuSearchOrConnect() const override;
    void process() override;
    void reset_rx_state() override;
    void send(const void* data, size_t size, bool flush) override;
    bool receive(void* data, size_t& size, bool& restoredByFEC) override;
    void setChannel(int ch) override;
    void setTxPower(int txPower) override;
    size_t get_data_rate() const override;
    int get_input_dBm() const override;
    void contributeGroundStats(GSStats& stats) override;

    std::string getTransportMessage() const override;

    bool startUsbAdapter(int fd);
    void stopUsbAdapter();
    bool isUsbAdapterRunning() const;
    int activeUsbFd() const;
    void setTransportPacketCallback(std::function<void(const uint8_t* data, size_t size, int input_dbm)> callback);
    uint32_t consumeAllFrameCount();      // all raw frames seen, including non-matching
    uint32_t consumeFilteredFrameCount(); // frames that passed MAC + packet filter

private:
    void buildRadiotapHeaderLocked();
    void resetTxAssemblerLocked();
    bool sendRawPacket(const std::shared_ptr<Rtl8812aDevice>& device, const std::vector<uint8_t>& packet);
    void queueReceivedPacket(const uint8_t* data, size_t size, int input_dbm);

    mutable std::mutex m_mutex;
    mutable std::mutex m_stop_mutex;
    mutable std::mutex m_device_io_mutex;
    std::atomic<bool> m_active = {false};
    Clock::time_point m_activate_time = Clock::time_point::min();
    std::vector<uint8_t> m_radiotap_header;
    std::vector<uint8_t> m_tx_current_packet;
    std::vector<std::vector<uint8_t>> m_tx_block_packets;
    std::unique_ptr<WiFiDriver> m_wifi_driver;
    Logger_t m_devourer_logger;
    std::shared_ptr<Rtl8812aDevice> m_device;
    fec_t* m_tx_fec = nullptr;
    FecBlockDecoder m_rx_decoder;
    std::unique_ptr<std::thread> m_usb_event_thread;
    std::unique_ptr<std::thread> m_rx_thread;
    libusb_context* m_libusb_context = nullptr;
    libusb_device_handle* m_usb_handle = nullptr;
    int m_active_usb_fd = -1;
    uint8_t m_tx_power = 0;
    std::atomic<Clock::time_point::rep> m_last_rx_packet_tp {Clock::time_point::min().time_since_epoch().count()};
    std::atomic<uint32_t> m_all_frame_count = {0};
    std::atomic<uint32_t> m_filtered_frame_count = {0};
    size_t m_packet_header_offset = 0;
    size_t m_payload_offset = 0;
    size_t m_transport_packet_size = 0;
    uint32_t m_next_block_index = 1;
    std::atomic<int> m_best_input_dbm = {0};
    std::atomic<int> m_latched_input_dbm = {0};
    size_t m_data_stats_rate = 0;
    size_t m_data_stats_data_accumulated = 0;
    uint64_t m_last_rx_decoded_bytes_total = 0;
    Clock::time_point m_data_stats_last_tp = Clock::now();
    std::function<void(const uint8_t* data, size_t size, int input_dbm)> m_transport_packet_callback;
};
