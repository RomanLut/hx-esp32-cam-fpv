#include "linux_apfpv_transport.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <random>
#include <set>
#include <sys/socket.h>
#include <tuple>
#include <unistd.h>

#include "Log.h"
#include "fmt/core.h"
#include "gs_runtime_core.h"
#include "gs_runtime_config.h"
#include "gs_runtime_state.h"
#include "gs_shared_runtime.h"
#include "gs_stats.h"
#include "shared/frame_packets_debug.h"
#include "utils.h"

namespace
{

constexpr uint16_t kApfpvUdpPort = 5600;
constexpr const char* kApfpvPeerIp = "192.168.4.1";
constexpr const char* kApfpvSsidPrefix = "esp32cam-fpv-";
constexpr uint8_t kApfpvDefaultRxCodingN = 8;
constexpr int kApfpvSocketTimeoutUs = 250000;
constexpr int kApfpvSocketSendBufferSize = 8 * 1024;
constexpr int kApfpvSearchScanTimeoutSeconds = 10;
constexpr auto kApfpvSearchScanInterval = std::chrono::seconds(2);
constexpr auto kApfpvSearchDuration = std::chrono::seconds(10);
constexpr auto kApfpvWifiRetryInterval = std::chrono::seconds(3);
constexpr auto kApfpvWifiConnectTimeout = std::chrono::seconds(8);
constexpr uint8_t kApfpvAssociateFailuresBeforeUsbReset = 2;
constexpr auto kApfpvStreamConnectTimeout = std::chrono::seconds(20);
constexpr int kApfpvStaticIpHostMin = 50;
constexpr int kApfpvStaticIpHostMax = 250;
constexpr auto kApfpvWifiLinkPollInterval = std::chrono::milliseconds(250);
constexpr auto kApfpvWifiNoPacketsBeforeHealthPoll = std::chrono::seconds(5);
constexpr auto kApfpvWifiConnectedHealthPollInterval = std::chrono::seconds(3);

//===================================================================================
//===================================================================================
// Builds a progress string that always includes the target camera identity when it can be derived.
std::string buildApfpvProgressText(const std::string& ssid, const std::string& stage)
{
    const uint16_t camera_id = parseApfpvCameraIdFromSsid(ssid);
    if (camera_id != 0)
    {
        return "Connecting to " + formatApfpvCameraId(camera_id) + ": " + stage;
    }

    if (!ssid.empty())
    {
        return "Connecting to " + ssid + ": " + stage;
    }

    return stage;
}

//===================================================================================
//===================================================================================
// Builds a unique Linux interface list from the RX and TX transport descriptors.
std::vector<std::string> buildApfpvInterfaces(const gs::core::RXDescriptor& rx_descriptor,
                                              const gs::core::TXDescriptor& tx_descriptor)
{
    std::vector<std::string> interfaces;
    std::set<std::string> seen_interfaces;

    for (const std::string& interface : rx_descriptor.interfaces)
    {
        if (!interface.empty() && seen_interfaces.insert(interface).second)
        {
            interfaces.push_back(interface);
        }
    }

    if (!tx_descriptor.interface.empty() && seen_interfaces.insert(tx_descriptor.interface).second)
    {
        interfaces.push_back(tx_descriptor.interface);
    }

    return interfaces;
}

//===================================================================================
//===================================================================================
// Returns the APFPV Wi-Fi interfaces that should be used for scanning and connecting.
// APFPV follows the dedicated APFPV interface selected in the menu when available.
std::vector<std::string> selectApfpvConnectInterfaces(const std::vector<std::string>& interfaces)
{
    if (interfaces.empty())
    {
        return {};
    }

    if (!s_groundstation_config.apfpvInterface.empty() &&
        s_groundstation_config.apfpvInterface != "auto")
    {
        const auto it = std::find(interfaces.begin(), interfaces.end(), s_groundstation_config.apfpvInterface);
        if (it != interfaces.end())
        {
            return {*it};
        }
    }

    return interfaces;
}

// Extracts the first SSID value from iw output using the provided line prefix.
std::optional<std::string> findSsidInOutput(const std::string& output, const std::string& line_prefix)
{
    size_t search_from = 0;
    while (search_from < output.size())
    {
        const size_t line_end = output.find('\n', search_from);
        const size_t line_length =
            line_end == std::string::npos ? output.size() - search_from : line_end - search_from;
        const std::string line = trimAsciiWhitespace(output.substr(search_from, line_length));
        if (line.rfind(line_prefix, 0) == 0)
        {
            const std::string ssid = trimAsciiWhitespace(line.substr(line_prefix.size()));
            if (!ssid.empty())
            {
                return ssid;
            }
        }

        if (line_end == std::string::npos)
        {
            break;
        }
        search_from = line_end + 1;
    }

    return std::nullopt;
}

//===================================================================================
//===================================================================================
// Extracts the first integer value that follows the provided line prefix in shell output.
std::optional<int> findIntInOutput(const std::string& output, const std::string& line_prefix)
{
    size_t search_from = 0;
    while (search_from < output.size())
    {
        const size_t line_end = output.find('\n', search_from);
        const size_t line_length =
            line_end == std::string::npos ? output.size() - search_from : line_end - search_from;
        const std::string line = trimAsciiWhitespace(output.substr(search_from, line_length));
        if (line.rfind(line_prefix, 0) == 0)
        {
            const std::string value = trimAsciiWhitespace(line.substr(line_prefix.size()));
            if (!value.empty())
            {
                try
                {
                    return std::stoi(value);
                }
                catch (...)
                {
                    return std::nullopt;
                }
            }
        }

        if (line_end == std::string::npos)
        {
            break;
        }
        search_from = line_end + 1;
    }

    return std::nullopt;
}

//===================================================================================
//===================================================================================
// Resolves a Linux networking executable path once and falls back to the plain command name if needed.
const std::string& resolveApfpvTool(const char* executable_name)
{
    static std::map<std::string, std::string> resolved_tools;

    const auto it = resolved_tools.find(executable_name);
    if (it != resolved_tools.end())
    {
        return it->second;
    }

    const std::optional<std::string> resolved_path = findExecutablePath(executable_name);
    const std::string tool_path = resolved_path.has_value() ? *resolved_path : std::string(executable_name);
    resolved_tools.emplace(executable_name, tool_path);
    return resolved_tools.find(executable_name)->second;
}

//===================================================================================
//===================================================================================
// Returns the resolved Linux ip command path used by APFPV Wi-Fi management.
const std::string& ipTool()
{
    return resolveApfpvTool("ip");
}

//===================================================================================
//===================================================================================
// Returns the resolved Linux iw command path used by APFPV Wi-Fi management.
const std::string& iwTool()
{
    return resolveApfpvTool("iw");
}

//===================================================================================
//===================================================================================
// Returns the resolved Linux ifconfig command path when available.
std::optional<std::string> ifconfigTool()
{
    return findExecutablePath("ifconfig");
}

//===================================================================================
//===================================================================================
// Switches Linux Wi-Fi interfaces back to managed mode for APFPV camera connections.
void setManagedMode(const std::vector<std::string>& interfaces)
{
    for (const std::string& interface : interfaces)
    {
        LOGI("Setting managed mode on {}", interface);
        runShellCommand(fmt::format("{} link set {} down", ipTool(), interface));
        runShellCommand(fmt::format("{} dev {} set type managed", iwTool(), interface));
        runShellCommand(fmt::format("{} link set {} up", ipTool(), interface));
    }
}

//===================================================================================
//===================================================================================
// Clears APFPV-managed IPv4 state from one interface before another transport reuses it.
void clearApfpvInterfaceAddressing(const std::string& interface)
{
    const std::string quoted_interface = shellQuote(interface);
    const std::string quoted_ip_tool = shellQuote(ipTool());
    runShellCommand(
        fmt::format("sh -lc \""
                    "{0} route del 192.168.4.0/24 dev {1} >/dev/null 2>&1 || true; "
                    "{0} addr flush dev {1} scope global >/dev/null 2>&1 || true"
                    "\"",
                    quoted_ip_tool,
                    quoted_interface));
}

//===================================================================================
//===================================================================================
// Fully tears down one APFPV interface so raw-broadcast can take ownership without a reboot.
void teardownApfpvInterface(const std::string& interface)
{
    if (interface.empty())
    {
        return;
    }

    // Some Linux Wi-Fi drivers keep the managed association half-alive across a transport
    // switch. Explicit disconnect plus APFPV subnet cleanup makes the later monitor-mode
    // transition reliable without requiring a board reboot.
    runShellCommand(fmt::format("{} dev {} disconnect >/dev/null 2>&1 || true",
                                iwTool(),
                                shellQuote(interface)));
    clearApfpvInterfaceAddressing(interface);
}

//===================================================================================
//===================================================================================
// Builds the shared frame-packet debug callbacks used by the Linux APFPV decoder.
FecBlockDecoder::Callbacks makeApfpvDecoderCallbacks()
{
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
        LOGW("Linux APFPV detected TX stream restart: {} -> {} (delta {})",
             previous_block,
             new_block,
             delta);
    };
    return callbacks;
}

} // namespace

//===================================================================================
//===================================================================================
// Stores a new transport message visible to the render thread via getTransportMessage().
void LinuxApfpvTransport::setMessage(std::string message)
{
    std::lock_guard<std::mutex> lock(m_message_mutex);
    m_transport_message = std::move(message);
}

//===================================================================================
//===================================================================================
// Returns the current user-facing transport status message for the top overlay.
std::string LinuxApfpvTransport::getTransportMessage() const
{
    std::lock_guard<std::mutex> lock(m_message_mutex);
    return m_transport_message;
}

//===================================================================================
//===================================================================================
// Resets the same USB Wi-Fi adapter in place and brings the managed interface back up.
static bool resetUsbBackedWifiInterface(const std::string& interface)
{
    std::error_code ec;
    const std::filesystem::path interface_device_path =
        std::filesystem::read_symlink(std::filesystem::path("/sys/class/net") / interface / "device", ec);
    if (ec)
    {
        LOGW("Linux APFPV could not resolve device symlink for {}: {}", interface, ec.message());
        return false;
    }

    std::string usb_device = interface_device_path.filename().string();
    const size_t interface_suffix = usb_device.find(':');
    if (interface_suffix != std::string::npos)
    {
        usb_device = usb_device.substr(0, interface_suffix);
    }
    if (usb_device.empty())
    {
        LOGW("Linux APFPV could not resolve/reset USB device for {} from {}",
             interface,
             interface_device_path.string());
        return false;
    }

    const std::optional<std::string> ifconfig = ifconfigTool();
    const std::string up_down_command = ifconfig.has_value()
                                            ? fmt::format("{0} {1}", shellQuote(*ifconfig), shellQuote(interface))
                                            : fmt::format("{0} link set dev {1}",
                                                          shellQuote(ipTool()),
                                                          shellQuote(interface));

    LOGW("Linux APFPV resetting USB adapter {} for {}", usb_device, interface);
    if (!runShellCommand(
            fmt::format("sh -lc \""
                        "{0} dev {1} disconnect >/dev/null 2>&1 || true; "
                        "{2} down >/dev/null 2>&1 || true; "
                        "sleep 2; "
                        "echo {3} > /sys/bus/usb/drivers/usb/unbind; "
                        "sleep 4; "
                        "echo {3} > /sys/bus/usb/drivers/usb/bind; "
                        "sleep 10; "
                        "{2} up >/dev/null 2>&1 || true; "
                        "sleep 4"
                        "\"",
                        shellQuote(iwTool()),
                        shellQuote(interface),
                        up_down_command,
                        shellQuote(usb_device))))
    {
        LOGW("Linux APFPV USB reset command failed for {} via {}", interface, usb_device);
        return false;
    }
    return true;
}

//===================================================================================
//===================================================================================
// Runs one managed-mode APFPV connect attempt using the provided SSID and optional frequency.
bool attemptApfpvWifiConnect(const std::string& interface, const std::string& ssid, int frequency_mhz, std::string* output)
{
    const std::string quoted_interface = shellQuote(interface);
    const std::string quoted_ssid = shellQuote(ssid);
    const std::string quoted_ip_tool = shellQuote(ipTool());
    const std::string quoted_iw_tool = shellQuote(iwTool());
    const std::string frequency_suffix = frequency_mhz > 0 ? fmt::format(" {}", frequency_mhz) : std::string();

    return runShellCommand(
        fmt::format("timeout {5}s sh -lc \""
                    "{2} dev {0} disconnect >/dev/null 2>&1 || true; "
                    "{1} link set dev {0} down; "
                    "sleep 1; "
                    "{2} dev {0} set type managed; "
                    "{1} link set dev {0} up; "
                    "sleep 2; "
                    "{2} dev {0} connect -w {3}{4}"
                    "\" 2>&1",
                    quoted_interface,
                    quoted_ip_tool,
                    quoted_iw_tool,
                    quoted_ssid,
                    frequency_suffix,
                    static_cast<int>(kApfpvWifiConnectTimeout.count())),
        output);
}

//===================================================================================
//===================================================================================
// Initializes the Linux APFPV transport runtime state and FEC helpers.
LinuxApfpvTransport::LinuxApfpvTransport()
{
    m_tx_fec = fec_new(2, 3);

    FecBlockDecoder::Descriptor decoder_descriptor = {};
    decoder_descriptor.coding_k = FEC_K;
    decoder_descriptor.coding_n = kApfpvDefaultRxCodingN;
    decoder_descriptor.mtu = AIR2GROUND_MAX_MTU;
    decoder_descriptor.reset_duration = std::chrono::milliseconds(0);
    decoder_descriptor.restart_backjump_blocks = 64;
    decoder_descriptor.max_block_queue_size = 3;
    decoder_descriptor.duplicate_window = 100;
    decoder_descriptor.interface_count = 1;
    m_rx_decoder.init(decoder_descriptor);
    m_rx_decoder.setCallbacks(makeApfpvDecoderCallbacks());
}

//===================================================================================
//===================================================================================
// Stops the Linux APFPV backend thread and releases FEC resources.
LinuxApfpvTransport::~LinuxApfpvTransport()
{
    stopBackend();

    if (m_tx_fec != nullptr)
    {
        fec_free(m_tx_fec);
        m_tx_fec = nullptr;
    }
}

//===================================================================================
//===================================================================================
// Stores descriptors and prepares the Linux APFPV UDP backend for activation.
bool LinuxApfpvTransport::init(const gs::core::RXDescriptor& rx_descriptor,
                               const gs::core::TXDescriptor& tx_descriptor)
{
    storeDescriptors(rx_descriptor, tx_descriptor);
    m_apfpv_interfaces = buildApfpvInterfaces(rx_descriptor, tx_descriptor);
    ensureRxDecoderConfig();
    reset_rx_state();
    resetWifiAutoconnectState();
    return true;
}

//===================================================================================
//===================================================================================
// Activates APFPV mode by switching the configured Linux interfaces to managed mode.
void LinuxApfpvTransport::activate()
{
    const std::vector<std::string> connect_interfaces = selectApfpvConnectInterfaces(m_apfpv_interfaces);
    setManagedMode(connect_interfaces);
    resetWifiAutoconnectState();
    startBackend();
    if (getApfpvPreferredCameraId() != 0)
    {
        startConnectToPreferredCamera(Clock::now());
    }
    else
    {
        m_wifi_state = WifiState::Idle;
        setMessage("");
    }
}

//===================================================================================
//===================================================================================
// Deactivates APFPV mode by stopping backend I/O and clearing APFPV menu-search state.
void LinuxApfpvTransport::deactivate()
{
    for (const std::string& interface : selectApfpvConnectInterfaces(m_apfpv_interfaces))
    {
        teardownApfpvInterface(interface);
    }
    stopBackend();
    m_wifi_state = WifiState::Inactive;
    m_target_interface.clear();
    m_target_ssid.clear();
    m_target_frequency_mhz = 0;
    m_connected_interface.clear();
    m_connected_ssid.clear();
    m_discovered_candidates.clear();
    m_waiting_for_search_selection = false;
    m_menu_search_request_pending.store(false);
    m_menu_search_cancel_requested.store(false);
    m_menu_search_active.store(false);
    m_menu_search_done.store(false);
    updateApfpvDiscoveredCameras({});
}

//===================================================================================
//===================================================================================
// Forces an immediate APFPV reconnect to the currently preferred camera from the menu path.
bool LinuxApfpvTransport::requestImmediateReconnect()
{
    LOGI("Linux APFPV immediate reconnect requested for preferred {}",
         formatApfpvCameraId(getApfpvPreferredCameraId()));
    disconnectCurrentWifiLink();
    transitionToIdle(Clock::now(), true);
    waitForPreferredCameraVisibility(Clock::now());
    return true;
}

//===================================================================================
//===================================================================================
// Reports that APFPV exposes an explicit menu-driven search flow in addition to reconnect.
bool LinuxApfpvTransport::supportsMenuSearchOrConnect() const
{
    return true;
}

//===================================================================================
//===================================================================================
// Requests that the comms-thread state machine start an explicit APFPV camera search.
void LinuxApfpvTransport::beginMenuSearchOrConnect()
{
    // Immediately clear the user-facing connect overlay while the comms thread
    // is still unwinding any in-flight APFPV reconnect attempt.
    setMessage("");
    m_menu_search_done.store(false);
    m_menu_search_cancel_requested.store(false);
    m_menu_search_active.store(true);
    m_menu_search_request_pending.store(true);
    m_waiting_for_search_selection = false;
}

//===================================================================================
//===================================================================================
// Returns whether the explicit APFPV menu search/connect flow has reached a terminal state.
bool LinuxApfpvTransport::advanceMenuSearchOrConnect()
{
    return m_menu_search_done.load();
}

//===================================================================================
//===================================================================================
// Requests that the comms-thread state machine stop any active APFPV menu search flow.
void LinuxApfpvTransport::cancelMenuSearchOrConnect()
{
    m_menu_search_cancel_requested.store(true);
}

//===================================================================================
//===================================================================================
// Processes the APFPV decoder state and refreshes derived throughput statistics.
void LinuxApfpvTransport::process()
{
    const Clock::time_point now = Clock::now();
    handlePendingMenuRequests(now);
    advanceWifiStateMachine();
    if (!m_connected_interface.empty() && s_runtimeCore.session.connectedAirDeviceId() != 0)
    {
        setMessage("");
        if (m_menu_search_active.load())
        {
            m_menu_search_active.store(false);
            m_menu_search_done.store(true);
        }
    }

    ensureRxDecoderConfig();
    m_rx_decoder.process(now);
    syncRxDecoderStats();

    if (now - m_data_stats_last_tp >= std::chrono::seconds(1))
    {
        const float seconds = std::chrono::duration<float>(now - m_data_stats_last_tp).count();
        if (seconds > 0.0f)
        {
            m_data_stats_rate = static_cast<size_t>(static_cast<float>(m_data_stats_data_accumulated) / seconds);
        }
        else
        {
            m_data_stats_rate = 0;
        }
        m_data_stats_data_accumulated = 0;
        m_data_stats_last_tp = now;
    }
}

//===================================================================================
//===================================================================================
// Resets APFPV session state after the Wi-Fi link to the camera is lost.
void LinuxApfpvTransport::handleCameraWifiDisconnect()
{
    LOGW("Linux APFPV handleCameraWifiDisconnect connected_interface={} connected_ssid={} "
         "target_interface={} target_ssid={} connected_air=0x{:04X} got_config={}",
         m_connected_interface,
         m_connected_ssid,
         m_target_interface,
         m_target_ssid,
         s_runtimeCore.session.connectedAirDeviceId(),
         s_runtimeCore.session.gotConfigPacket());
    if (!m_connected_interface.empty())
    {
        LOGW("Linux APFPV lost camera Wi-Fi link on {}", m_connected_interface);
        runShellCommand(fmt::format("{} dev {} disconnect", iwTool(), m_connected_interface));
    }
    else
    {
        LOGW("Linux APFPV lost camera Wi-Fi link");
    }

    m_connected_interface.clear();
    m_connected_ssid.clear();
    m_target_interface.clear();
    m_target_ssid.clear();
    m_target_frequency_mhz = 0;
    m_associate_failure_count = 0;
    clearApfpvActiveCamera();
    s_runtimeCore.resetTransportRuntimePreserveApfpvState(*this, Clock::now());
    if (m_menu_search_active.load())
    {
        m_next_search_scan_tp = Clock::now();
        m_wifi_state = WifiState::Searching;
        setMessage("");
        return;
    }

    waitForPreferredCameraVisibility(Clock::now());
}

//===================================================================================
//===================================================================================
// Resets Linux APFPV Wi-Fi state, shared APFPV camera cache, and menu-search flags.
void LinuxApfpvTransport::resetWifiAutoconnectState()
{
    m_connected_interface.clear();
    m_connected_ssid.clear();
    m_target_interface.clear();
    m_target_ssid.clear();
    m_target_frequency_mhz = 0;
    m_associate_failure_count = 0;
    m_discovered_candidates.clear();
    m_waiting_for_search_selection = false;
    m_next_retry_tp = Clock::now();
    m_wait_for_link_deadline_tp = Clock::now();
    m_stream_connect_deadline_tp = Clock::now();
    m_next_link_poll_tp = Clock::now();
    m_next_search_scan_tp = Clock::now();
    m_search_started_tp = Clock::now();
    m_menu_search_request_pending.store(false);
    m_menu_search_cancel_requested.store(false);
    m_menu_search_active.store(false);
    m_menu_search_done.store(false);
    clearApfpvCameraRuntimeState();
    updateApfpvDiscoveredCameras({});
    setMessage("");
    m_wifi_state = WifiState::Inactive;
}

//===================================================================================
//===================================================================================
// Returns the currently linked APFPV camera SSID for the interface when already connected.
std::optional<std::string> LinuxApfpvTransport::currentCameraSsid(const std::string& interface) const
{
    std::string output;
    if (!runShellCommand(fmt::format("{} dev {} link", iwTool(), interface), &output))
    {
        return std::nullopt;
    }

    const std::optional<std::string> ssid = findSsidInOutput(output, "SSID:");
    if (!ssid.has_value() || ssid->rfind(kApfpvSsidPrefix, 0) != 0)
    {
        return std::nullopt;
    }

    return ssid;
}

//===================================================================================
//===================================================================================
// Builds a random APFPV local IPv4 address inside 192.168.4.50..250 for this connect attempt.
std::string buildRandomApfpvLocalIp()
{
    static std::random_device random_device;
    static std::mt19937 generator(random_device());
    static std::uniform_int_distribution<int> distribution(kApfpvStaticIpHostMin, kApfpvStaticIpHostMax);
    return fmt::format("192.168.4.{}/24", distribution(generator));
}

//===================================================================================
//===================================================================================
// Returns whether the interface already has a local IPv4 address inside the APFPV camera subnet.
bool hasApfpvLocalAddress(const std::string& interface)
{
    std::string output;
    if (!runShellCommand(fmt::format("{} -4 addr show dev {}", ipTool(), shellQuote(interface)), &output))
    {
        return false;
    }

    return output.find("inet 192.168.4.") != std::string::npos;
}

//===================================================================================
//===================================================================================
// Configures a random static APFPV subnet address immediately after association.
bool configureStaticApfpvAddress(const std::string& interface)
{
    const std::string quoted_interface = shellQuote(interface);
    const std::string quoted_ip_tool = shellQuote(ipTool());
    const std::string local_ip = buildRandomApfpvLocalIp();
    const std::string local_ip_src = local_ip.substr(0, local_ip.find('/'));
    LOGI("Linux APFPV assigning static local address {} on {}", local_ip_src, interface);
    return runShellCommand(
        fmt::format(
            "sh -lc \""
            "{1} addr flush dev {0} && "
            "{1} addr add {2} dev {0} && "
            "{1} route replace 192.168.4.0/24 dev {0} src {3}"
            "\"",
            quoted_interface,
            quoted_ip_tool,
            local_ip,
            local_ip_src));
}

//===================================================================================
//===================================================================================
// Assigns a random static APFPV subnet address immediately after a successful association.
bool LinuxApfpvTransport::configureApfpvLocalAddress(const std::string& interface, const std::string& ssid)
{
    setMessage(buildApfpvProgressText(ssid, "Configuring IP..."));
    // APFPV runs on a fixed camera subnet and the Linux DHCP tools have proven unstable
    // on reconnect, so the transport assigns its local address directly instead.
    if (configureStaticApfpvAddress(interface))
    {
        setMessage(buildApfpvProgressText(ssid, "Waiting for stream..."));
        m_stream_connect_deadline_tp = Clock::now() + kApfpvStreamConnectTimeout;
        return true;
    }

    LOGW("Linux APFPV failed to configure a static local address on {}", interface);
    setMessage("");
    return false;
}

//===================================================================================
//===================================================================================
// Connects the interface to the selected open APFPV camera SSID and resets USB after repeated association failures.
bool LinuxApfpvTransport::connectToCameraNetwork(const std::string& interface,
                                                 const std::string& ssid,
                                                 int frequency_mhz)
{
    LOGI("Connecting {} to APFPV camera network {}", interface, ssid);
    setMessage(buildApfpvProgressText(ssid, "Associating..."));
    runShellCommand(fmt::format("{} dev {} disconnect", iwTool(), interface));

    std::string connect_output;
    if (!attemptApfpvWifiConnect(interface, ssid, frequency_mhz, &connect_output))
    {
        LOGW("Failed to connect {} to APFPV SSID {}: {}",
             interface,
             ssid,
             trimAsciiWhitespace(connect_output));

        // Some rtl88xxau runs report a failed/timeout `iw connect -w` even though the
        // interface is already associated when probed immediately after the command.
        // Treat that as a successful association so DHCP/static IP setup is not skipped.
        const std::optional<std::string> linked_ssid = currentCameraSsid(interface);
        if (linked_ssid.has_value() && *linked_ssid == ssid)
        {
            LOGW("Linux APFPV continuing after connect command failure because {} is already linked to {}",
                 interface,
                 ssid);
            m_associate_failure_count = 0;
            if (!configureApfpvLocalAddress(interface, ssid))
            {
                runShellCommand(fmt::format("{} dev {} disconnect", iwTool(), interface));
                return false;
            }
            return true;
        }

        m_associate_failure_count++;

        if (m_menu_search_request_pending.load())
        {
            LOGI("Linux APFPV skipping reconnect fallback for {} because menu search is pending", ssid);
            return false;
        }

        if (m_associate_failure_count < kApfpvAssociateFailuresBeforeUsbReset)
        {
            LOGI("Linux APFPV deferring USB reset for {} after associate failure {}/{}",
                 ssid,
                 static_cast<int>(m_associate_failure_count),
                 static_cast<int>(kApfpvAssociateFailuresBeforeUsbReset));
            return false;
        }

        setMessage("Resetting USB interface...");
        if (resetUsbBackedWifiInterface(interface))
        {
            // Reset the consecutive-failure counter once the USB adapter reset has run.
            m_associate_failure_count = 0;
            connect_output.clear();
            setMessage(buildApfpvProgressText(ssid, "Associating..."));
            if (attemptApfpvWifiConnect(interface, ssid, frequency_mhz, &connect_output))
            {
                m_associate_failure_count = 0;
                if (!configureApfpvLocalAddress(interface, ssid))
                {
                    runShellCommand(fmt::format("{} dev {} disconnect", iwTool(), interface));
                    return false;
                }
                return true;
            }

            LOGW("Failed to connect {} to APFPV SSID {} after USB reset retry: {}",
                 interface,
                 ssid,
                 trimAsciiWhitespace(connect_output));
            m_associate_failure_count++;
        }

        return false;
    }

    m_associate_failure_count = 0;
    if (m_menu_search_request_pending.load())
    {
        LOGI("Linux APFPV aborting successful connect to {} because menu search is pending", ssid);
        runShellCommand(fmt::format("{} dev {} disconnect", iwTool(), interface));
        setMessage("");
        return false;
    }

    if (!configureApfpvLocalAddress(interface, ssid))
    {
        runShellCommand(fmt::format("{} dev {} disconnect", iwTool(), interface));
        return false;
    }
    return true;
}

//===================================================================================
//===================================================================================
// Handles pending menu search start/cancel requests before the APFPV state machine advances.
void LinuxApfpvTransport::handlePendingMenuRequests(Clock::time_point now)
{
    if (m_menu_search_cancel_requested.exchange(false))
    {
        m_menu_search_active.store(false);
        m_menu_search_done.store(true);
        if (m_wifi_state == WifiState::Searching)
        {
            transitionToIdle(now, true);
        }
    }

    if (m_menu_search_request_pending.exchange(false))
    {
        startMenuSearch(now);
    }
}

//===================================================================================
//===================================================================================
// Starts an explicit APFPV menu search by dropping the current link and clearing old results.
void LinuxApfpvTransport::startMenuSearch(Clock::time_point now)
{
    setMessage("");
    disconnectCurrentWifiLink();
    setManagedMode(selectApfpvConnectInterfaces(m_apfpv_interfaces));
    clearApfpvActiveCamera();
    m_discovered_candidates.clear();
    m_waiting_for_search_selection = false;
    updateApfpvDiscoveredCameras({});
    s_runtimeCore.resetTransportRuntimePreserveApfpvState(*this, now);
    m_target_interface.clear();
    m_target_ssid.clear();
    m_target_frequency_mhz = 0;
    m_next_retry_tp = now;
    m_wait_for_link_deadline_tp = now;
    m_stream_connect_deadline_tp = now;
    m_next_link_poll_tp = now;
    m_next_search_scan_tp = now;
    m_search_started_tp = now;
    m_wifi_state = WifiState::Searching;
    m_menu_search_active.store(true);
    m_menu_search_done.store(false);
    setMessage("");
}

//===================================================================================
//===================================================================================
// Advances one explicit APFPV camera search pass and reacts to the resulting candidate count.
void LinuxApfpvTransport::advanceSearchState(Clock::time_point now)
{
    setMessage("");
    if (now < m_next_search_scan_tp)
    {
        return;
    }

    m_next_search_scan_tp = now + kApfpvSearchScanInterval;
    m_discovered_candidates = normalizeCandidates(runSearchPass());

    std::vector<ApfpvCameraDescriptor> cameras;
    for (const SearchCandidate& candidate : m_discovered_candidates)
    {
        cameras.push_back(candidate.camera);
    }
    updateApfpvDiscoveredCameras(cameras);

    if (m_discovered_candidates.empty())
    {
        if (now - m_search_started_tp >= kApfpvSearchDuration)
        {
            m_waiting_for_search_selection = false;
            m_menu_search_active.store(false);
            m_menu_search_done.store(true);
            m_wifi_state = WifiState::Idle;
        }
        return;
    }

    if (m_discovered_candidates.size() >= 2)
    {
        m_waiting_for_search_selection = true;
        m_menu_search_active.store(false);
        m_menu_search_done.store(true);
        m_wifi_state = WifiState::Idle;
        return;
    }

    const std::optional<SearchCandidate> candidate = selectSingleSearchCandidate(m_discovered_candidates);
    if (!candidate.has_value())
    {
        return;
    }

    s_groundstation_config.apfpvPreferredCameraId = candidate->camera.device_id;
    setApfpvPreferredCameraId(candidate->camera.device_id);
    s_settingsStorage.saveGroundStationConfig();
    m_waiting_for_search_selection = false;
    m_target_interface = candidate->interface;
    m_target_ssid = candidate->camera.ssid;
    m_target_frequency_mhz = candidate->frequency_mhz;
    m_wifi_state = WifiState::Connecting;
    setMessage("Connecting to WiFi network...");
}

//===================================================================================
//===================================================================================
// Waits for the saved preferred APFPV camera to appear in discovery results before attempting association.
void LinuxApfpvTransport::waitForPreferredCameraVisibility(Clock::time_point now)
{
    const uint16_t preferred_camera_id = getApfpvPreferredCameraId();
    const std::vector<std::string> connect_interfaces = selectApfpvConnectInterfaces(m_apfpv_interfaces);
    if (preferred_camera_id == 0 || connect_interfaces.empty())
    {
        m_wifi_state = WifiState::Idle;
        setMessage("");
        return;
    }

    m_waiting_for_search_selection = false;
    m_connected_interface.clear();
    m_connected_ssid.clear();
    m_target_interface.clear();
    m_target_ssid.clear();
    m_target_frequency_mhz = 0;
    m_associate_failure_count = 0;
    m_discovered_candidates.clear();
    m_next_retry_tp = now;
    m_next_search_scan_tp = now;
    m_search_started_tp = now;
    m_wifi_state = WifiState::Searching;
    setMessage("Connecting to " + formatApfpvCameraId(preferred_camera_id) + ": Searching...");
}

//===================================================================================
//===================================================================================
// Drives APFPV Wi-Fi search, connect, reconnect, and link-health transitions on the comms thread.
void LinuxApfpvTransport::advanceWifiStateMachine()
{
    const Clock::time_point now = Clock::now();
    const uint16_t preferred_camera_id = getApfpvPreferredCameraId();

    if (m_wifi_state == WifiState::Inactive)
    {
        return;
    }

    // Check the stream deadline before Wi-Fi health polling; a healthy link can otherwise re-latch
    // the same SSID and keep pushing normal processing away from the timeout path.
    if (m_wifi_state == WifiState::Connected && s_runtimeCore.session.connectedAirDeviceId() == 0
        && now >= m_stream_connect_deadline_tp)
    {
        LOGW("Linux APFPV timed out waiting for stream from {}", m_connected_ssid);
        handleCameraWifiDisconnect();
        return;
    }

    if ((m_wifi_state == WifiState::Searching || m_wifi_state == WifiState::Idle) &&
        preferred_camera_id != 0 && now >= m_next_link_poll_tp)
    {
        m_next_link_poll_tp = now + kApfpvWifiLinkPollInterval;
        std::string recovered_interface;
        std::string recovered_ssid;
        if (detectCurrentWifiLink(recovered_interface, recovered_ssid))
        {
            const uint16_t recovered_camera_id = parseApfpvCameraIdFromSsid(recovered_ssid);
            if (recovered_camera_id == preferred_camera_id)
            {
                // Some adapters finish the managed-mode association after the original connect
                // attempt has already dropped back to search. Recover that existing link here.
                if (!hasApfpvLocalAddress(recovered_interface) &&
                    !configureApfpvLocalAddress(recovered_interface, recovered_ssid))
                {
                    LOGW("Linux APFPV failed to recover late link on {}", recovered_interface);
                    disconnectCurrentWifiLink();
                    m_next_retry_tp = now + kApfpvWifiRetryInterval;
                    setMessage("");
                    return;
                }

                LOGI("Linux APFPV recovered existing Wi-Fi link on {} to {} while searching",
                     recovered_interface,
                     recovered_ssid);
                latchConnectedCamera(recovered_interface, recovered_ssid, now);
                return;
            }
        }
    }

    std::string linked_interface;
    std::string linked_ssid;
    if (shouldPollWifiLink(now))
    {
        if (m_wifi_state == WifiState::WaitingForLink)
        {
            m_next_link_poll_tp = now + kApfpvWifiLinkPollInterval;
        }
        const bool link_present = detectCurrentWifiLink(linked_interface, linked_ssid);
        if (link_present)
        {
            const uint16_t linked_camera_id = parseApfpvCameraIdFromSsid(linked_ssid);
            if (preferred_camera_id != 0 && linked_camera_id != 0 && linked_camera_id != preferred_camera_id)
            {
                LOGI("Linux APFPV forcing reconnect from linked camera {} to preferred {}",
                     formatApfpvCameraId(linked_camera_id),
                     formatApfpvCameraId(preferred_camera_id));
                if (m_wifi_state == WifiState::Connected)
                {
                    handleCameraWifiDisconnect();
                    return;
                }
            }
            if (preferred_camera_id == 0 || linked_camera_id == preferred_camera_id)
            {
                if (m_wifi_state != WifiState::Connected && !hasApfpvLocalAddress(linked_interface))
                {
                    // Some rtl88xxau reconnects complete after `iw connect -w` has already failed,
                    // so a later link probe must still finish local APFPV IP configuration.
                    if (!configureApfpvLocalAddress(linked_interface, linked_ssid))
                    {
                        LOGW("Linux APFPV failed to finish late IP configuration on {}", linked_interface);
                        disconnectCurrentWifiLink();
                        m_next_retry_tp = now + kApfpvWifiRetryInterval;
                        m_wifi_state = m_menu_search_active.load() || preferred_camera_id != 0 ? WifiState::Searching
                                                                                                : WifiState::Idle;
                        setMessage("");
                        return;
                    }
                }
                latchConnectedCamera(linked_interface, linked_ssid, now);
                return;
            }
        }
        else if (m_wifi_state == WifiState::Connected)
        {
            m_next_link_poll_tp = now + kApfpvWifiConnectedHealthPollInterval;
            handleCameraWifiDisconnect();
            return;
        }
    }

    if (m_wifi_state == WifiState::Connected)
    {
        return;
    }

    if (m_wifi_state == WifiState::Searching)
    {
        if (!m_menu_search_active.load() && preferred_camera_id != 0)
        {
            setMessage("Connecting to " + formatApfpvCameraId(preferred_camera_id) + ": Searching...");
            if (now < m_next_search_scan_tp)
            {
                return;
            }

            m_next_search_scan_tp = now + kApfpvSearchScanInterval;
            m_discovered_candidates = normalizeCandidates(runSearchPass());
            const std::optional<SearchCandidate> preferred_candidate =
                selectPreferredSearchCandidate(preferred_camera_id, m_discovered_candidates);
            if (!preferred_candidate.has_value())
            {
                return;
            }

            m_target_interface = preferred_candidate->interface;
            m_target_ssid = preferred_candidate->camera.ssid;
            m_target_frequency_mhz = preferred_candidate->frequency_mhz;
            m_wifi_state = WifiState::Connecting;
            setMessage(buildApfpvProgressText(m_target_ssid, "Associating..."));
            return;
        }

        advanceSearchState(now);
        return;
    }

    if (m_wifi_state == WifiState::Connecting)
    {
        setMessage("Connecting to WiFi network...");
        if (connectToCameraNetwork(m_target_interface, m_target_ssid, m_target_frequency_mhz))
        {
            m_wifi_state = WifiState::WaitingForLink;
            m_wait_for_link_deadline_tp = now + kApfpvWifiConnectTimeout;
            m_next_link_poll_tp = now;
            return;
        }

        LOGW("Linux APFPV failed to connect to {} on {}", m_target_ssid, m_target_interface);
        m_next_retry_tp = now + kApfpvWifiRetryInterval;
        m_wifi_state = m_menu_search_active.load() || preferred_camera_id != 0 ? WifiState::Searching
                                                                                : WifiState::Idle;
        setMessage("");
        return;
    }

    if (m_wifi_state == WifiState::WaitingForLink)
    {
        // APFPV packets can arrive before the driver reports a stable managed-mode link via `iw link`.
        // Once the session is already bound to an air device, keep the current target latched instead of
        // falling back to search on the Wi-Fi link timeout path.
        if (s_runtimeCore.session.connectedAirDeviceId() != 0 && !m_target_interface.empty() && !m_target_ssid.empty())
        {
            LOGI("Linux APFPV stream is active before link confirmation on {}; latching {}",
                 m_target_interface,
                 m_target_ssid);
            latchConnectedCamera(m_target_interface, m_target_ssid, now);
            return;
        }
        if (now >= m_wait_for_link_deadline_tp)
        {
            LOGW("Linux APFPV timed out waiting for Wi-Fi link to {}", m_target_ssid);
            disconnectCurrentWifiLink();
            m_next_retry_tp = now + kApfpvWifiRetryInterval;
            m_wifi_state = m_menu_search_active.load() || preferred_camera_id != 0 ? WifiState::Searching
                                                                                    : WifiState::Idle;
            setMessage("");
        }
        return;
    }

    if (m_wifi_state != WifiState::Idle)
    {
        m_wifi_state = WifiState::Idle;
    }

    if (now < m_next_retry_tp || preferred_camera_id == 0 || m_waiting_for_search_selection)
    {
        return;
    }

    waitForPreferredCameraVisibility(now);
}

//===================================================================================
//===================================================================================
// Resets APFPV runtime to idle mode and optionally preserves the currently published camera list.
void LinuxApfpvTransport::transitionToIdle(Clock::time_point now, bool preserve_apfpv_state)
{
    m_connected_interface.clear();
    m_connected_ssid.clear();
    m_target_interface.clear();
    m_target_ssid.clear();
    m_target_frequency_mhz = 0;
    m_associate_failure_count = 0;
    clearApfpvActiveCamera();
    if (preserve_apfpv_state)
    {
        s_runtimeCore.resetTransportRuntimePreserveApfpvState(*this, now);
    }
    else
    {
        s_runtimeCore.resetTransportRuntime(*this, now);
    }
    m_next_retry_tp = now;
    m_wait_for_link_deadline_tp = now;
    m_stream_connect_deadline_tp = now;
    m_next_link_poll_tp = now;
    m_next_search_scan_tp = now;
    m_wifi_state = WifiState::Idle;
    setMessage("");
}

//===================================================================================
//===================================================================================
// Starts a direct APFPV connect attempt to the currently preferred camera on the selected interface.
void LinuxApfpvTransport::startConnectToPreferredCamera(Clock::time_point now)
{
    waitForPreferredCameraVisibility(now);
}

//===================================================================================
//===================================================================================
// Latches the currently connected APFPV Wi-Fi link into shared runtime and menu state.
void LinuxApfpvTransport::latchConnectedCamera(const std::string& interface,
                                               const std::string& ssid,
                                               Clock::time_point /* now */)
{
    const bool already_connected_to_target =
        m_wifi_state == WifiState::Connected && m_connected_interface == interface && m_connected_ssid == ssid;
    if (m_connected_interface != interface || m_connected_ssid != ssid)
    {
        LOGI("Linux APFPV connected on {} to {}", interface, ssid);
    }

    m_connected_interface = interface;
    m_connected_ssid = ssid;
    m_target_interface.clear();
    m_target_ssid.clear();
    m_target_frequency_mhz = 0;
    m_associate_failure_count = 0;
    m_wifi_state = WifiState::Connected;
    m_next_link_poll_tp = Clock::now() + kApfpvWifiNoPacketsBeforeHealthPoll;
    setApfpvActiveCamera(ssid);
    if (s_runtimeCore.session.connectedAirDeviceId() == 0)
    {
        if (!already_connected_to_target)
        {
            m_stream_connect_deadline_tp = Clock::now() + kApfpvStreamConnectTimeout;
        }
        setMessage(buildApfpvProgressText(ssid, "Waiting for stream..."));
    }
    else
    {
        setMessage("");
    }
}

//===================================================================================
//===================================================================================
// Returns whether the APFPV transport should probe Wi-Fi link state on this process tick.
bool LinuxApfpvTransport::shouldPollWifiLink(Clock::time_point now) const
{
    if (now < m_next_link_poll_tp)
    {
        return false;
    }

    if (m_wifi_state == WifiState::WaitingForLink)
    {
        return true;
    }

    if (m_wifi_state == WifiState::Connected)
    {
        const auto packet_silence = now - s_runtimeCore.last_packet_tp;
        if (packet_silence < kApfpvWifiNoPacketsBeforeHealthPoll)
        {
            return false;
        }

        return true;
    }

    return false;
}

//===================================================================================
//===================================================================================
// Performs one APFPV search scan pass on the managed interfaces and returns normalized candidates.
std::vector<LinuxApfpvTransport::SearchCandidate> LinuxApfpvTransport::runSearchPass() const
{
    std::vector<SearchCandidate> candidates;
    const std::vector<std::string> connect_interfaces = selectApfpvConnectInterfaces(m_apfpv_interfaces);
    for (const std::string& interface : connect_interfaces)
    {
        std::string output;
        const std::string scan_command = fmt::format(
            "sh -lc \"timeout {2}s {0} dev {1} scan ap-force\"",
            shellQuote(iwTool()),
            shellQuote(interface),
            kApfpvSearchScanTimeoutSeconds);
        if (!runShellCommand(scan_command, &output))
        {
            continue;
        }

        size_t search_from = 0;
        int current_frequency_mhz = 0;
        while (search_from < output.size())
        {
            const size_t line_end = output.find('\n', search_from);
            const size_t line_length =
                line_end == std::string::npos ? output.size() - search_from : line_end - search_from;
            const std::string line = trimAsciiWhitespace(output.substr(search_from, line_length));
            if (line.rfind("freq:", 0) == 0)
            {
                const std::optional<int> parsed_frequency = findIntInOutput(line, "freq:");
                if (parsed_frequency.has_value())
                {
                    current_frequency_mhz = *parsed_frequency;
                }
            }
            else if (line.rfind("SSID:", 0) == 0)
            {
                const std::string ssid = trimAsciiWhitespace(line.substr(5));
                const uint16_t device_id = parseApfpvCameraIdFromSsid(ssid);
                if (device_id != 0 && ssid.rfind(kApfpvSsidPrefix, 0) == 0)
                {
                    candidates.push_back({interface, {device_id, ssid}, current_frequency_mhz});
                }
            }

            if (line_end == std::string::npos)
            {
                break;
            }
            search_from = line_end + 1;
        }
    }

    return normalizeCandidates(std::move(candidates));
}

//===================================================================================
//===================================================================================
// Sorts and deduplicates APFPV search candidates by device id for stable menu output.
std::vector<LinuxApfpvTransport::SearchCandidate> LinuxApfpvTransport::normalizeCandidates(
    std::vector<SearchCandidate> candidates) const
{
    std::sort(candidates.begin(),
              candidates.end(),
              [](const SearchCandidate& lhs, const SearchCandidate& rhs)
              {
                  return lhs.camera.device_id < rhs.camera.device_id;
              });
    candidates.erase(
        std::unique(candidates.begin(),
                    candidates.end(),
                    [](const SearchCandidate& lhs, const SearchCandidate& rhs)
                    {
                        return lhs.camera.device_id == rhs.camera.device_id;
                    }),
        candidates.end());
    return candidates;
}

//===================================================================================
//===================================================================================
// Returns the sole APFPV search candidate when exactly one camera was found.
std::optional<LinuxApfpvTransport::SearchCandidate> LinuxApfpvTransport::selectSingleSearchCandidate(
    const std::vector<SearchCandidate>& candidates) const
{
    if (candidates.size() != 1)
    {
        return std::nullopt;
    }

    return candidates.front();
}

//===================================================================================
//===================================================================================
// Returns the preferred APFPV search candidate when that exact camera is visible in the latest scan results.
std::optional<LinuxApfpvTransport::SearchCandidate> LinuxApfpvTransport::selectPreferredSearchCandidate(
    uint16_t preferred_camera_id,
    const std::vector<SearchCandidate>& candidates) const
{
    if (preferred_camera_id == 0)
    {
        return std::nullopt;
    }

    for (const SearchCandidate& candidate : candidates)
    {
        if (candidate.camera.device_id == preferred_camera_id)
        {
            return candidate;
        }
    }

    return std::nullopt;
}

//===================================================================================
//===================================================================================
// Detects the currently linked APFPV Wi-Fi camera across the configured managed interfaces.
bool LinuxApfpvTransport::detectCurrentWifiLink(std::string& interface, std::string& ssid) const
{
    const std::vector<std::string> connect_interfaces = selectApfpvConnectInterfaces(m_apfpv_interfaces);
    for (const std::string& candidate_interface : connect_interfaces)
    {
        const std::optional<std::string> current_ssid = currentCameraSsid(candidate_interface);
        if (current_ssid.has_value())
        {
            interface = candidate_interface;
            ssid = *current_ssid;
            return true;
        }
    }

    return false;
}

//===================================================================================
//===================================================================================
// Disconnects the currently linked APFPV Wi-Fi interface before a reconnect attempt.
void LinuxApfpvTransport::disconnectCurrentWifiLink()
{
    const std::string interface = !m_connected_interface.empty() ? m_connected_interface : m_target_interface;
    if (!interface.empty())
    {
        runShellCommand(fmt::format("{} dev {} disconnect", iwTool(), interface));
    }
    clearApfpvActiveCamera();
    m_connected_interface.clear();
    m_connected_ssid.clear();
}

//===================================================================================
//===================================================================================
// Resets APFPV decoder, counters, and partially assembled transmit block state.
void LinuxApfpvTransport::reset_rx_state()
{
    m_rx_decoder.reset(Clock::now());
    m_has_first_tx_payload = false;
    m_next_tx_block_index = 1;
    m_data_stats_rate = 0;
    m_data_stats_data_accumulated = 0;
    m_last_rx_decoded_bytes_total = 0;
    m_data_stats_last_tp = Clock::now();
    syncRxDecoderStats();
    setMessage("");
}

//===================================================================================
//===================================================================================
// Encodes one APFPV control payload into transport packets and sends them over UDP.
void LinuxApfpvTransport::send(const void* data, size_t size, bool /* flush */)
{
    if (data == nullptr || size == 0)
    {
        return;
    }

    std::array<uint8_t, GROUND2AIR_MAX_MTU> current_payload = {};
    const size_t copy_size = std::min(size, current_payload.size());
    std::memcpy(current_payload.data(), data, copy_size);

    if (!m_has_first_tx_payload)
    {
        m_first_tx_payload = current_payload;
        m_has_first_tx_payload = true;
        sendTransportPacket(current_payload.data(), current_payload.size(), 0);
        return;
    }

    sendTransportPacket(current_payload.data(), current_payload.size(), 1);

    if (m_tx_fec != nullptr)
    {
        std::array<uint8_t, GROUND2AIR_MAX_MTU> fec_payload = {};
        const gf* src_ptrs[2] = {
            m_first_tx_payload.data(),
            current_payload.data()
        };
        gf* fec_ptrs[1] = {
            fec_payload.data()
        };
        fec_encode(m_tx_fec,
                   src_ptrs,
                   fec_ptrs,
                   fec_block_nums() + 2,
                   1,
                   current_payload.size());
        sendTransportPacket(fec_payload.data(), fec_payload.size(), 2);
    }

    m_has_first_tx_payload = false;
    m_next_tx_block_index++;
}

//===================================================================================
//===================================================================================
// Returns the next decoded APFPV session payload if the FEC decoder has one ready.
bool LinuxApfpvTransport::receive(void* data, size_t& size, bool& restoredByFEC)
{
    return m_rx_decoder.receive(data, size, restoredByFEC);
}

//===================================================================================
//===================================================================================
// Returns the latest decoded APFPV throughput estimate in bytes per second.
size_t LinuxApfpvTransport::get_data_rate() const
{
    return m_data_stats_rate;
}

//===================================================================================
//===================================================================================
// Returns the latest APFPV RSSI estimate, which is unavailable for Linux UDP transport.
int LinuxApfpvTransport::get_input_dBm() const
{
    return m_input_dbm;
}

//===================================================================================
//===================================================================================
// Reconfigures the APFPV receive decoder when the negotiated transport settings change.
void LinuxApfpvTransport::ensureRxDecoderConfig()
{
    const bool have_live_config =
        s_runtimeCore.session.gotConfigPacket() ||
        s_runtimeCore.session.acceptConfigPacket();

    const uint8_t config_k = have_live_config ? s_runtimeCore.config_packet.dataChannel.fec_codec_k : 0;
    const uint8_t config_n = have_live_config ? s_runtimeCore.config_packet.dataChannel.fec_codec_n : 0;

    const uint8_t effective_k = config_k > 0 ? config_k : static_cast<uint8_t>(FEC_K);
    const uint8_t effective_n = config_n > 0 ? config_n : kApfpvDefaultRxCodingN;
    const uint16_t effective_mtu = AIR2GROUND_MAX_MTU;

    const FecBlockDecoder::Stats stats_before = m_rx_decoder.getStats();
    if (s_runtimeCore.rx_decoder_k == effective_k &&
        s_runtimeCore.rx_decoder_n == effective_n &&
        s_runtimeCore.rx_decoder_mtu == effective_mtu)
    {
        return;
    }

    FecBlockDecoder::Descriptor decoder_descriptor = {};
    decoder_descriptor.coding_k = effective_k;
    decoder_descriptor.coding_n = effective_n;
    decoder_descriptor.mtu = effective_mtu;
    decoder_descriptor.reset_duration = std::chrono::milliseconds(0);
    decoder_descriptor.restart_backjump_blocks = 64;
    decoder_descriptor.max_block_queue_size = 3;
    decoder_descriptor.duplicate_window = 100;
    decoder_descriptor.interface_count = 1;

    s_runtimeCore.rx_decoder_k = effective_k;
    s_runtimeCore.rx_decoder_n = effective_n;
    s_runtimeCore.rx_decoder_mtu = effective_mtu;
    m_rx_decoder.init(decoder_descriptor);
    m_last_rx_decoded_bytes_total = stats_before.decoded_bytes_total;
    syncRxDecoderStats();
}

//===================================================================================
//===================================================================================
// Copies APFPV decoder stats into the shared GS counters used by the Linux overlay.
void LinuxApfpvTransport::syncRxDecoderStats()
{
    const FecBlockDecoder::Stats stats = m_rx_decoder.getStats();
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
        m_data_stats_data_accumulated +=
            static_cast<size_t>(stats.decoded_bytes_total - m_last_rx_decoded_bytes_total);
    }
    m_last_rx_decoded_bytes_total = stats.decoded_bytes_total;
}

//===================================================================================
//===================================================================================
// Starts the Linux APFPV UDP receive backend thread if it is not already running.
bool LinuxApfpvTransport::startBackend()
{
    stopBackend();

    m_exit_requested = false;
    if (!openSocket())
    {
        return false;
    }

    m_rx_thread = std::thread([this]()
    {
        rxThreadProc();
    });
    m_backend_running = true;
    return true;
}

//===================================================================================
//===================================================================================
// Stops the Linux APFPV UDP backend thread and closes its socket.
void LinuxApfpvTransport::stopBackend()
{
    m_exit_requested = true;
    closeSocket();

    if (m_rx_thread.joinable())
    {
        m_rx_thread.join();
    }

    m_backend_running = false;
}

//===================================================================================
//===================================================================================
// Receives APFPV UDP transport packets, filters them, and pushes them into the FEC decoder.
void LinuxApfpvTransport::rxThreadProc()
{
    std::array<uint8_t, sizeof(Packet_Header) + AIR2GROUND_MAX_MTU> buffer = {};

    while (!m_exit_requested.load())
    {
        sockaddr_in from_addr = {};
        socklen_t from_len = sizeof(from_addr);
        int socket_fd = -1;
        {
            std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
            socket_fd = m_socket_fd;
        }

        if (socket_fd < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        const int len = recvfrom(socket_fd,
                                 reinterpret_cast<char*>(buffer.data()),
                                 static_cast<int>(buffer.size()),
                                 0,
                                 reinterpret_cast<sockaddr*>(&from_addr),
                                 &from_len);
        if (len <= 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                continue;
            }

            if (!m_exit_requested.load())
            {
                LOGW("Linux APFPV recvfrom failed: {}", std::strerror(errno));
            }
            continue;
        }

        if (static_cast<size_t>(len) >= sizeof(Packet_Header))
        {
            const auto* header = reinterpret_cast<const Packet_Header*>(buffer.data());
            {
                std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
                s_gs_stats.inPacketCounterAll[0]++;
                s_gs_stats.inPacketCounter[0]++;
            }
            s_runtimeCore.transport_packets_seen++;
            s_runtimeCore.transport_packets_passed_filter++;
            s_runtimeCore.last_transport_block = header->block_index;
            s_runtimeCore.last_transport_packet_index = header->packet_index;
            s_runtimeCore.last_transport_payload_size = header->size;
            s_runtimeCore.last_transport_from = header->fromDeviceId;
            s_runtimeCore.last_transport_to = header->toDeviceId;
            s_runtimeCore.transport_packets_passed_filter++;
        }
        else
        {
            s_runtimeCore.transport_packets_filtered++;
            continue;
        }

        m_rx_decoder.pushPacket(buffer.data(), static_cast<size_t>(len), 0, Clock::now());
        syncRxDecoderStats();
    }
}

//===================================================================================
//===================================================================================
// Opens and binds the Linux APFPV UDP socket on the shared APFPV port.
bool LinuxApfpvTransport::openSocket()
{
    std::lock_guard<std::mutex> socket_lock(m_socket_mutex);

    closeSocket();

    m_socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket_fd < 0)
    {
        LOGE("Linux APFPV socket create failed: {}", std::strerror(errno));
        return false;
    }

    int reuse_address = 1;
    setsockopt(m_socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));
#ifdef SO_REUSEPORT
    setsockopt(m_socket_fd, SOL_SOCKET, SO_REUSEPORT, &reuse_address, sizeof(reuse_address));
#endif

    timeval timeout = {};
    timeout.tv_sec = 0;
    timeout.tv_usec = kApfpvSocketTimeoutUs;
    setsockopt(m_socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(m_socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    int send_buffer_size = kApfpvSocketSendBufferSize;
    setsockopt(m_socket_fd, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size));

    sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(kApfpvUdpPort);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(m_socket_fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0)
    {
        LOGE("Linux APFPV bind failed: {}", std::strerror(errno));
        closeSocket();
        return false;
    }

    std::memset(&m_peer_addr, 0, sizeof(m_peer_addr));
    m_peer_addr.sin_family = AF_INET;
    m_peer_addr.sin_port = htons(kApfpvUdpPort);
    m_peer_addr.sin_addr.s_addr = inet_addr(kApfpvPeerIp);
    LOGI("Linux APFPV UDP socket ready on port {}", kApfpvUdpPort);
    return true;
}

//===================================================================================
//===================================================================================
// Closes the Linux APFPV UDP socket if it is currently open.
void LinuxApfpvTransport::closeSocket()
{
    if (m_socket_fd >= 0)
    {
        ::close(m_socket_fd);
        m_socket_fd = -1;
    }
}

//===================================================================================
//===================================================================================
// Wraps one payload into an APFPV transport packet and sends it to the fixed camera endpoint.
void LinuxApfpvTransport::sendTransportPacket(const uint8_t* payload, size_t payload_size, uint8_t packet_index)
{
    std::array<uint8_t, sizeof(Packet_Header) + GROUND2AIR_MAX_MTU> packet = {};
    auto* header = reinterpret_cast<Packet_Header*>(packet.data());
    m_packet_filter.apply_packet_header_data(header);
    header->size = static_cast<uint16_t>(GROUND2AIR_MAX_MTU);
    header->block_index = m_next_tx_block_index;
    header->packet_index = packet_index;

    const size_t bounded_size = std::min(payload_size, static_cast<size_t>(GROUND2AIR_MAX_MTU));
    std::memcpy(packet.data() + sizeof(Packet_Header), payload, bounded_size);

    int socket_fd = -1;
    sockaddr_in peer_addr = {};
    {
        std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
        socket_fd = m_socket_fd;
        peer_addr = m_peer_addr;
    }

    if (socket_fd < 0)
    {
        return;
    }

    const ssize_t sent = sendto(socket_fd,
                                packet.data(),
                                packet.size(),
                                MSG_DONTWAIT,
                                reinterpret_cast<const sockaddr*>(&peer_addr),
                                sizeof(peer_addr));
    if (sent > 0)
    {
        std::lock_guard<std::mutex> lg(s_gs_stats_mutex);
        s_gs_stats.outPacketCounter++;
        return;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
    {
        LOGW("Linux APFPV sendto failed: {}", std::strerror(errno));
    }
}
