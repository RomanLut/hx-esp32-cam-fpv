#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "fec_block_decoder.h"
#include "core/transport_base.h"
#include "devourer/src/IRtlDevice.h"
#include "devourer/src/WiFiDriver.h"
#include "devourer/src/logger.h"

struct libusb_context;
struct libusb_device_handle;

//===================================================================================
//===================================================================================
// Implements the Android raw-broadcast transport using up to two RTL USB adapters.
// Devourer's IRtlDevice interface is chip-family-agnostic: the same code below
// drives Jaguar1 (RTL8812AU/8811AU/8821AU/8814AU), Jaguar2 (RTL8822BU/8811CU/
// 8821CU) and Jaguar3 (RTL8812CU/8822CU/8812EU/8822EU) adapters, since the actual
// chip family is detected from hardware at WiFiDriver::CreateRtlDevice() time.
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
    bool supportsNetworkInterfaceStatus() const override { return false; }
    std::vector<std::string> copyInterfaceStatusLines() const override;
    void process() override;
    void reset_rx_state() override;
    void send(const void* data, size_t size, bool flush) override;
    bool receive(void* data, size_t& size, bool& restoredByFEC) override;
    void setChannel(int ch) override;
    void setTxPower(int txPower) override;
    void setTxInterface(const std::string& interface) override;
    size_t get_data_rate() const override;
    int get_input_dBm() const override;
    void contributeGroundStats(GSStats& stats) override;

    std::string getTransportMessage() const override;

    bool startUsbAdapter(int fd);
    void stopUsbAdapter();
    bool isUsbAdapterRunning() const;
    size_t activeUsbAdapterCount() const;
    int activeUsbFd() const;
    void setTransportPacketCallback(std::function<void(const uint8_t* data,
                                                       size_t size,
                                                       int input_dbm,
                                                       size_t interface_index)> callback);

private:
    //===================================================================================
    //===================================================================================
    // Owns the driver, libusb handles, and receive counters for one Android RTL adapter.
    struct UsbAdapter
    {
        std::shared_ptr<IRtlDevice> device;
        // IRtlDevice has no should_stop getter (only the StopRxLoop() setter), so
        // the adapter tracks its own stop flag alongside calling StopRxLoop(). Once set,
        // this lifecycle flag must never be cleared; recovery creates a new UsbAdapter.
        std::atomic<bool> should_stop = {false};
        std::unique_ptr<std::thread> rx_thread;
        libusb_context* libusb_context = nullptr;
        libusb_device_handle* usb_handle = nullptr;
        int fd = -1;
        size_t index = 0;
        Clock::time_point channel_change_ready_time = Clock::time_point::min();
        std::atomic<uint32_t> all_frame_count = {0};
        std::atomic<uint32_t> filtered_frame_count = {0};
        std::atomic<uint64_t> filtered_frame_lifetime_count = {0};
        std::atomic<int> best_input_dbm = {std::numeric_limits<int>::lowest()};
    };

    void buildRadiotapHeaderLocked();
    void resetTxAssemblerLocked();
    bool sendRawPacket(const std::shared_ptr<UsbAdapter>& adapter, const std::vector<uint8_t>& packet);
    bool sendRawPacketWithFailover(const std::vector<uint8_t>& packet);
    void queueReceivedPacket(const std::shared_ptr<UsbAdapter>& adapter,
                             const uint8_t* data,
                             size_t size,
                             int input_dbm);
    void stopUsbAdapterLocked(const std::shared_ptr<UsbAdapter>& adapter);
    std::shared_ptr<UsbAdapter> txAdapterLocked(const UsbAdapter* excluded_adapter = nullptr) const;

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
    std::vector<std::shared_ptr<UsbAdapter>> m_usb_adapters;
    Clock::time_point m_last_adapter_transition_time = Clock::time_point::min();
    fec_t* m_tx_fec = nullptr;
    FecBlockDecoder m_rx_decoder;
    uint8_t m_tx_power = 0;
    std::atomic<Clock::time_point::rep> m_last_rx_packet_tp {Clock::time_point::min().time_since_epoch().count()};
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
    std::function<void(const uint8_t* data, size_t size, int input_dbm, size_t interface_index)>
        m_transport_packet_callback;
};
