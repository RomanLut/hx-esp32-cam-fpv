#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/transport_base.h"
#include "fec_block_decoder.h"
#include "gs_runtime_state.h"

/*
 APFPV Linux transport design:

 - The APFPV transport is a single comms-thread-owned state machine. That one thread
   owns Wi-Fi search, connect, reconnect, and UDP packet processing state.
 - On activation, the transport builds the APFPV interface set from Linux RX/TX
   descriptors, filters it through the configured `APFPV Interface` setting, and
   switches the selected interface(s) to managed mode with:
   `ip link set <iface> down -> iw dev <iface> set type managed -> ip link set <iface> up`.
 - APFPV UDP backend initialization also happens during activation. The transport opens
   the local UDP socket on port 5600, configures socket timeouts/buffers, and starts
   the backend receive thread before Wi-Fi association is attempted.
 - If a preferred camera id is already stored in settings, the state machine first runs
   discovery scans until that specific camera becomes visible, and only then starts a
   managed-mode connect attempt to that camera.
 - APFPV connect/search uses the dedicated `APFPV Interface` selected in GS settings
   when that interface is available in the Linux transport descriptors.
 - Discovery is not a background service. APFPV camera scanning only runs after the
   user explicitly chooses `Search...` in the Connect menu.
 - `Search...` closes any current APFPV Wi-Fi link, clears the shared discovered-camera
   list, and moves the state machine into a search state. While search is active, the
   comms thread periodically scans for APFPV SSIDs and updates the shared menu snapshot.
 - If search finds one APFPV camera, that camera becomes the preferred target and the
   state machine immediately starts connecting to it. If search finds multiple cameras,
   search stops and the Connect menu shows `Connect to:` rows for the user to choose.
 - A normal APFPV connect attempt is performed on the selected interface with the
   sequence:
   `iw dev <iface> disconnect -> ip link set dev <iface> down -> iw dev <iface> set
   type managed -> ip link set dev <iface> up -> iw dev <iface> connect -w <ssid>`.
   After association succeeds, the transport assigns a random static local address on
   the APFPV camera subnet and then latches the connected SSID into shared runtime
   state before waiting for APFPV packets/config on the already-open UDP socket.
 - Waiting for APFPV stream/session startup is bounded to 20 seconds so a healthy
   Wi-Fi association without camera traffic returns to the reconnect path.
 - While connecting, the state machine may use `iw dev <iface> link` polling to confirm
   association with the target SSID. This probing is limited to connect / wait phases.
 - Some rtl88xxau-based adapters can wedge on same-interface APFPV camera handoff and
   report `Operation already in progress (-114)` on reconnect. After two consecutive
   association failures on Linux, the transport resolves the USB device behind the selected
   interface through `/sys/class/net/<iface>/device`, performs an in-place USB
   `unbind/bind` reset of that same adapter, brings the interface back up, and then
   retries the managed-mode connect without requiring a full board reboot.
 - Once APFPV packet flow has started, the hot path avoids `iw` probing entirely so the
   comms thread can process received packets with minimum latency.
 - If no packets are received for 5 seconds while APFPV is connected, the transport
   starts fallback Wi-Fi health checks every 3 seconds. If `iw dev <iface> link` no
   longer reports the expected APFPV SSID, the state machine returns to connect mode
   and retries the preferred APFPV camera.
 - The Search & Connect menu reads the shared discovered-camera cache populated by the
   APFPV state machine during explicit search only. Selecting `Connect to:` closes the
   menu, updates the preferred target, and lets the same state machine perform handoff.
*/
//===================================================================================
//===================================================================================
// Implements the Linux APFPV UDP transport with managed-mode Wi-Fi setup and FEC decode.
class LinuxApfpvTransport final : public gs::core::TransportBase
{
public:
    //===================================================================================
    //===================================================================================
    // Tracks the APFPV Wi-Fi connection lifecycle driven by the comms-thread state machine.
    enum class WifiState : uint8_t
    {
        Inactive = 0,
        Idle = 1,
        Searching = 2,
        Connecting = 3,
        WaitingForLink = 4,
        Connected = 5
    };

    //===================================================================================
    //===================================================================================
    // Captures one APFPV camera found during an explicit comms-thread search pass.
    struct SearchCandidate
    {
        std::string interface;
        ApfpvCameraDescriptor camera;
        int frequency_mhz = 0;
    };

    LinuxApfpvTransport();
    ~LinuxApfpvTransport() override;

    bool init(const gs::core::RXDescriptor& rx_descriptor,
              const gs::core::TXDescriptor& tx_descriptor) override;
    void activate() override;
    void deactivate() override;
    bool requestImmediateReconnect() override;
    bool supportsMenuSearchOrConnect() const override;
    void process() override;
    void reset_rx_state() override;
    void beginMenuSearchOrConnect() override;
    bool advanceMenuSearchOrConnect() override;
    void cancelMenuSearchOrConnect() override;
    void send(const void* data, size_t size, bool flush) override;
    bool receive(void* data, size_t& size, bool& restoredByFEC) override;
    size_t get_data_rate() const override;
    int get_input_dBm() const override;

private:
    void advanceWifiStateMachine();
    void transitionToIdle(Clock::time_point now, bool preserve_apfpv_state);
    void startConnectToPreferredCamera(Clock::time_point now);
    void handlePendingMenuRequests(Clock::time_point now);
    void startMenuSearch(Clock::time_point now);
    void advanceSearchState(Clock::time_point now);
    std::vector<SearchCandidate> runSearchPass() const;
    std::vector<SearchCandidate> normalizeCandidates(std::vector<SearchCandidate> candidates) const;
    std::optional<SearchCandidate> selectSingleSearchCandidate(const std::vector<SearchCandidate>& candidates) const;
    std::optional<SearchCandidate> selectPreferredSearchCandidate(uint16_t preferred_camera_id,
                                                                  const std::vector<SearchCandidate>& candidates) const;
    void latchConnectedCamera(const std::string& interface, const std::string& ssid, Clock::time_point now);
    bool detectCurrentWifiLink(std::string& interface, std::string& ssid) const;
    void disconnectCurrentWifiLink();
    void handleCameraWifiDisconnect();
    void resetWifiAutoconnectState();
    void waitForPreferredCameraVisibility(Clock::time_point now);
    std::optional<std::string> currentCameraSsid(const std::string& interface) const;
    bool connectToCameraNetwork(const std::string& interface, const std::string& ssid, int frequency_mhz);
    bool configureApfpvLocalAddress(const std::string& interface, const std::string& ssid);
    void ensureRxDecoderConfig();
    void syncRxDecoderStats();
    bool shouldPollWifiLink(Clock::time_point now) const;
    bool startBackend();
    void stopBackend();
    void rxThreadProc();
    bool openSocket();
    void closeSocket();
    void sendTransportPacket(const uint8_t* payload, size_t payload_size, uint8_t packet_index);

    int m_socket_fd = -1;
    sockaddr_in m_peer_addr = {};
    std::mutex m_socket_mutex;

    std::atomic<bool> m_exit_requested = false;
    std::atomic<bool> m_backend_running = false;
    std::thread m_rx_thread;

    FecBlockDecoder m_rx_decoder;
    fec_t* m_tx_fec = nullptr;
    std::array<uint8_t, GROUND2AIR_MAX_MTU> m_first_tx_payload = {};
    bool m_has_first_tx_payload = false;
    uint32_t m_next_tx_block_index = 1;

    size_t m_data_stats_rate = 0;
    size_t m_data_stats_data_accumulated = 0;
    uint64_t m_last_rx_decoded_bytes_total = 0;
    Clock::time_point m_data_stats_last_tp = Clock::now();

    int m_input_dbm = 0;
    std::vector<std::string> m_apfpv_interfaces;
    std::string m_connected_interface;
    std::string m_connected_ssid;
    std::string m_target_interface;
    std::string m_target_ssid;
    int m_target_frequency_mhz = 0;
    uint8_t m_associate_failure_count = 0;
    Clock::time_point m_next_retry_tp = Clock::now();
    Clock::time_point m_wait_for_link_deadline_tp = Clock::now();
    Clock::time_point m_stream_connect_deadline_tp = Clock::now();
    Clock::time_point m_next_link_poll_tp = Clock::now();
    Clock::time_point m_next_search_scan_tp = Clock::now();
    Clock::time_point m_search_started_tp = Clock::now();
    WifiState m_wifi_state = WifiState::Inactive;
    std::vector<SearchCandidate> m_discovered_candidates;
    bool m_waiting_for_search_selection = false;
    std::atomic<bool> m_menu_search_request_pending = false;
    std::atomic<bool> m_menu_search_cancel_requested = false;
    std::atomic<bool> m_menu_search_active = false;
    std::atomic<bool> m_menu_search_done = false;
};
