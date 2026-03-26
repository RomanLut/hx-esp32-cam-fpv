#include <jni.h>
#include <android/native_window_jni.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <array>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <sys/statvfs.h>
#include <unistd.h>

#include "android_jpeg_decoder.h"
#include "android_video_renderer.h"
#include "fec.h"
#include "crc.h"
#include "core/gs_session_core.h"
#include "core/osd_menu_common.h"
#include "core/transport.h"
#include "core/video_frame_assembler.h"
#include "packet_filter.h"

namespace
{

constexpr int kMinTxPower = 5;
constexpr int kDefaultTxPower = 45;
constexpr int kMaxTxPower = 63;
constexpr uint64_t kGsSdMinFreeSpaceBytes = 20ull * 1024ull * 1024ull;
constexpr float kLinuxMenuRefWidth = 1280.0f;
constexpr float kLinuxMenuRefHeight = 720.0f;
constexpr float kLinuxMenuWindowWidth = 500.0f;
constexpr float kLinuxMenuWindowHeight = 600.0f;
constexpr float kLinuxMenuButtonWidth = 442.0f;
constexpr float kLinuxMenuButtonHeight = 35.0f;
constexpr float kLinuxMenuItemInset = 29.0f;
constexpr float kLinuxMenuCenterYOffset = 120.0f;
constexpr float kLinuxMenuGapLarge = 20.0f;
constexpr float kAndroidNavButtonSize = 72.0f;
constexpr float kAndroidNavGap = 10.0f;
constexpr float kAndroidNavMargin = 18.0f;

float linuxMenuScaleForSurface(float view_width, float view_height)
{
    if (view_width <= 0.0f || view_height <= 0.0f)
    {
        return 1.0f;
    }

    const float width_scale = view_width / kLinuxMenuRefWidth;
    const float height_scale = view_height / kLinuxMenuRefHeight;
    return std::min(width_scale, height_scale);
}

struct NavPadLayout
{
    float size = 0.0f;
    float gap = 0.0f;
    float margin = 0.0f;
    float left_x = 0.0f;
    float right_x = 0.0f;
    float center_x = 0.0f;
    float up_y = 0.0f;
    float mid_y = 0.0f;
    float down_y = 0.0f;
};

enum class AndroidMenuNavAction
{
    None,
    Up,
    Down,
    Back,
    Activate,
};

NavPadLayout buildNavPadLayout(float view_width, float view_height)
{
    NavPadLayout layout;
    const float ui_scale = std::min(view_width / 1280.0f, view_height / 720.0f);
    const float control_scale = std::max(0.85f, ui_scale);
    layout.size = kAndroidNavButtonSize * control_scale;
    layout.gap = kAndroidNavGap * control_scale;
    layout.margin = kAndroidNavMargin * control_scale;
    layout.right_x = view_width - layout.margin - layout.size;
    layout.left_x = layout.right_x - layout.size - layout.gap - layout.size;
    layout.center_x = layout.left_x + layout.size + layout.gap;
    layout.down_y = view_height - layout.margin - layout.size;
    layout.mid_y = layout.down_y - layout.gap - layout.size;
    layout.up_y = layout.mid_y - layout.gap - layout.size;
    return layout;
}

bool pointInRect(float x, float y, float rx, float ry, float rw, float rh)
{
    return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}

enum class AndroidMenuId
{
    Main,
    Search,
    GSSettings,
    GSScreen,
    Letterbox,
    GSWifiSettings,
    GSTxInterface,
    GSTxPower,
    Debug,
    CameraSettings,
    OSDFont,
    CameraStopCH,
    ExitToShell,
    Resolution,
    Image,
    Brightness,
    Contrast,
    Exposure,
    Saturation,
    Sharpness,
    WifiRate,
    WifiChannel,
    FEC
};

class AndroidTransport final : public gs::core::ITransport
{
public:
    bool init(const gs::core::RXDescriptor& rx_descriptor,
              const gs::core::TXDescriptor& /* tx_descriptor */) override
    {
        m_rx_descriptor = rx_descriptor;
        return true;
    }

    void process() override {}
    void reset_rx_state() override {}

    void send(const void* data, size_t size, bool /* flush */) override
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        m_last_sent_packet.assign(bytes, bytes + size);
    }

    bool receive(void* /* data */, size_t& /* size */, bool& /* restoredByFEC */) override
    {
        return false;
    }

    void setChannel(int /* ch */) override {}
    void setTxPower(int /* txPower */) override {}
    void setMonitorMode(const std::vector<std::string> /* interfaces */) override {}
    void setTxInterface(const std::string& /* interface */) override {}

    const gs::core::RXDescriptor& getRXDescriptor() const override
    {
        return m_rx_descriptor;
    }

    size_t get_data_rate() const override
    {
        return 0;
    }

    int get_input_dBm() const override
    {
        return m_input_dbm;
    }

    PacketFilter& getPacketFilter() override
    {
        return m_packet_filter;
    }

    void setInputDbm(int input_dbm)
    {
        m_input_dbm = input_dbm;
    }

private:
    gs::core::RXDescriptor m_rx_descriptor = {};
    PacketFilter m_packet_filter = {};
    std::vector<uint8_t> m_last_sent_packet;
    int m_input_dbm = 0;
};

class GroundToAirTransportDecoder
{
public:
    GroundToAirTransportDecoder()
    {
        static bool fec_initialized = false;
        if (!fec_initialized)
        {
            init_fec();
            fec_initialized = true;
        }
        setDescriptor(FEC_K, 8, AIR2GROUND_MAX_MTU);
    }

    ~GroundToAirTransportDecoder()
    {
        if (m_fec)
        {
            fec_free(m_fec);
            m_fec = nullptr;
        }
    }

    void reset()
    {
        m_has_block = false;
        m_current_block_index = 0;
        m_block_mtu = 0;
        m_present.assign(m_n, false);
        m_processed.assign(m_n, false);
    }

    void setDescriptor(uint8_t k, uint8_t n, uint16_t mtu)
    {
        if (k == 0 || n == 0 || k >= n || n > 12 || mtu == 0 || mtu > AIR2GROUND_MAX_MTU)
        {
            return;
        }

        m_k = k;
        m_n = n;
        m_configured_mtu = mtu;
        m_packets.assign(m_n, {});
        m_present.assign(m_n, false);
        m_processed.assign(m_n, false);
        reset();

        if (m_fec)
        {
            fec_free(m_fec);
            m_fec = nullptr;
        }
        m_fec = fec_new(m_k, m_n);
    }

    template <typename DispatchFn>
    void push(const uint8_t* data,
              size_t size,
              PacketFilter& packet_filter,
              bool use_filter,
              DispatchFn&& dispatch)
    {
        if (!data || size < sizeof(Packet_Header))
        {
            return;
        }

        if (use_filter &&
            packet_filter.filter_packet(data, size, AIR2GROUND_MAX_MTU) != PacketFilter::PacketFilterResult::Pass)
        {
            return;
        }

        const auto* header = reinterpret_cast<const Packet_Header*>(data);
        if (header->packet_index >= m_n)
        {
            return;
        }

        if (!m_has_block || header->block_index > m_current_block_index)
        {
            reset();
            m_has_block = true;
            m_current_block_index = header->block_index;
            m_block_mtu = header->size;
        }
        else if (header->block_index < m_current_block_index)
        {
            return;
        }

        if (m_block_mtu != header->size || header->size == 0 || header->size > m_configured_mtu)
        {
            reset();
            return;
        }

        const size_t payload_size = std::min<size_t>(header->size, size - sizeof(Packet_Header));
        auto& packet = m_packets[header->packet_index];
        packet.fill(0);
        std::memcpy(packet.data(), data + sizeof(Packet_Header), payload_size);
        m_present[header->packet_index] = true;

        if (header->packet_index < m_k && !m_processed[header->packet_index])
        {
            dispatch(m_packets[header->packet_index].data(), m_block_mtu);
            m_processed[header->packet_index] = true;
        }

        size_t primary_count = 0;
        size_t total_count = 0;
        for (uint8_t i = 0; i < m_n; ++i)
        {
            if (m_present[i])
            {
                total_count++;
                if (i < m_k)
                {
                    primary_count++;
                }
            }
        }

        if (total_count < m_k || !m_fec)
        {
            return;
        }

        std::vector<const gf*> sources(m_k, nullptr);
        std::vector<unsigned> indices(m_k, 0);
        std::vector<std::array<uint8_t, AIR2GROUND_MAX_MTU>> recovered;
        recovered.resize(m_k - primary_count);
        std::vector<gf*> outputs;
        outputs.reserve(recovered.size());
        for (auto& packet_buf : recovered)
        {
            outputs.push_back(packet_buf.data());
        }

        size_t source_index = 0;
        for (uint8_t i = 0; i < m_n && source_index < m_k; ++i)
        {
            if (!m_present[i])
            {
                continue;
            }
            sources[source_index] = m_packets[i].data();
            indices[source_index] = i;
            source_index++;
        }

        if (source_index < m_k)
        {
            return;
        }

        fec_decode(m_fec, sources.data(), outputs.data(), indices.data(), m_block_mtu);

        size_t recovered_index = 0;
        for (uint8_t i = 0; i < m_k; ++i)
        {
            if (!m_present[i])
            {
                std::memcpy(m_packets[i].data(), recovered[recovered_index++].data(), m_block_mtu);
                m_present[i] = true;
            }
        }

        for (uint8_t i = 0; i < m_k; ++i)
        {
            if (!m_processed[i])
            {
                dispatch(m_packets[i].data(), m_block_mtu);
                m_processed[i] = true;
            }
        }
    }

private:
    fec_t* m_fec = nullptr;
    uint8_t m_k = FEC_K;
    uint8_t m_n = 8;
    uint16_t m_configured_mtu = AIR2GROUND_MAX_MTU;
    bool m_has_block = false;
    uint32_t m_current_block_index = 0;
    uint16_t m_block_mtu = 0;
    std::vector<std::array<uint8_t, AIR2GROUND_MAX_MTU>> m_packets;
    std::vector<bool> m_present;
    std::vector<bool> m_processed;
};

struct NativeHandle
{
    explicit NativeHandle(uint16_t gs_device_id_value)
        : gs_device_id(gs_device_id_value),
          jpeg_decoder(renderer)
    {
        init_crc8_table();
        init_fec();
        tx_fec = fec_new(2, 3);
        session.resetPairing(gs_device_id, transport, Clock::now());
        transport.getPacketFilter().set_packet_filtering(0, 0);
    }

    ~NativeHandle()
    {
        stopUdpClient();
        if (tx_fec)
        {
            fec_free(tx_fec);
            tx_fec = nullptr;
        }
    }

    void stopUdpClient()
    {
        udp_stop_requested.store(true);
        if (udp_thread.joinable())
        {
            udp_thread.join();
        }
        std::lock_guard<std::mutex> lock(mutex);
        udp_running = false;
    }

    uint16_t gs_device_id = 1;
    mutable std::mutex mutex;
    gs::core::GsSessionCore session;
    gs::core::VideoFrameAssembler assembler;
    AndroidTransport transport;
    GroundToAirTransportDecoder rx_decoder;
    Ground2Air_Config_Packet config_packet = {};
    gs::core::SessionEventKind last_event_kind = gs::core::SessionEventKind::Ignore;
    std::vector<uint8_t> last_completed_frame;
    uint32_t next_tx_block_index = 1;
    fec_t* tx_fec = nullptr;
    bool tx_block_has_first_packet = false;
    std::array<uint8_t, GROUND2AIR_MAX_MTU> tx_first_packet_payload = {};
    uint32_t transport_packets_seen = 0;
    uint32_t transport_packets_passed_filter = 0;
    uint32_t transport_packets_filtered = 0;
    uint32_t decoded_packets_seen = 0;
    uint32_t last_decoded_type = 0;
    uint32_t last_decoded_size = 0;
    uint32_t last_decoded_air = 0;
    uint32_t last_decoded_gs = 0;
    uint32_t last_transport_block = 0;
    uint32_t last_transport_packet_index = 0;
    uint32_t last_transport_payload_size = 0;
    uint32_t last_transport_from = 0;
    uint32_t last_transport_to = 0;
    uint32_t last_video_frame_index = 0;
    uint32_t last_video_part_index = 0;
    uint32_t last_video_last_part = 0;
    uint32_t last_video_payload_size = 0;
    uint64_t udp_packets_received = 0;
    float udp_throughput_mbps = 0.0f;
    float udp_video_fps = 0.0f;
    std::string udp_peer_host = "192.168.4.1";
    int udp_peer_port = 5600;
    int udp_local_port = 5600;
    std::string udp_last_error;
    std::atomic<bool> udp_stop_requested = false;
    bool udp_running = false;
    std::thread udp_thread;
    bool menu_visible = false;
    AndroidMenuId menu_id = AndroidMenuId::Main;
    int menu_selected_item = 0;
    std::vector<AndroidMenuId> menu_back_ids;
    std::vector<int> menu_back_items;
    bool show_stats = true;
    int gs_screen_aspect_ratio = 1;
    bool gs_vsync = true;
    AndroidVideoRenderer renderer;
    AndroidJpegDecoder jpeg_decoder;
    int gs_wifi_band = DEFAULT_GS_WIFI_BAND;
    int gs_tx_power = kDefaultTxPower;
    std::string gs_tx_interface = "auto";
    int gs_gpio_keys_layout = 0;
    int gs_packet_debug_mode = 0;
    int gs_osd_font_index = 0;
    bool exit_requested = false;
};

struct AndroidMenuScreen
{
    std::string title;
    std::vector<std::string> items;
    std::vector<std::string> statuses;
    std::vector<std::string> status_lines;
    int selected_item = 0;
    bool can_go_back = false;
};

void commitMenuConfig(NativeHandle& handle)
{
    handle.session.setConfigPacket(handle.config_packet);
}

void menuGoForward(NativeHandle& handle, AndroidMenuId new_menu_id, int new_item)
{
    handle.menu_back_ids.push_back(handle.menu_id);
    handle.menu_back_items.push_back(handle.menu_selected_item);
    handle.menu_id = new_menu_id;
    handle.menu_selected_item = new_item;
    handle.menu_visible = true;
}

void menuGoBack(NativeHandle& handle)
{
    if (handle.menu_back_ids.empty())
    {
        handle.menu_visible = false;
        return;
    }

    handle.menu_id = handle.menu_back_ids.back();
    handle.menu_back_ids.pop_back();
    handle.menu_selected_item = handle.menu_back_items.back();
    handle.menu_back_items.pop_back();
}

std::string formatStorageLine(const char* label, bool ok, double free_gb, double total_gb, const char* suffix = "")
{
    std::ostringstream out;
    out << label << ": " << (ok ? "Ok" : "?");
    if (suffix != nullptr && suffix[0] != 0)
    {
        out << suffix;
    }
    out << ' ' << std::fixed << std::setprecision(2) << free_gb << "GB/" << total_gb << "GB";
    return out.str();
}

std::string buildAirSdStatus(const NativeHandle& handle)
{
    const auto& air = handle.session.airStatus();
    const char* suffix = air.sd_error ? " Error" : (air.sd_slow ? " Slow" : "");
    return formatStorageLine(
        "AIR SD",
        air.sd_detected && !air.sd_error,
        air.sd_free_space_gb16 / 16.0,
        air.sd_total_space_gb16 / 16.0,
        suffix);
}

std::string buildGsSdStatus()
{
    struct statvfs fs = {};
    if (statvfs("/data", &fs) != 0)
    {
        return "GS SD: ?";
    }

    const uint64_t total_bytes = static_cast<uint64_t>(fs.f_blocks) * static_cast<uint64_t>(fs.f_frsize);
    const uint64_t free_bytes = static_cast<uint64_t>(fs.f_bavail) * static_cast<uint64_t>(fs.f_frsize);
    return formatStorageLine(
        "GS SD",
        free_bytes >= kGsSdMinFreeSpaceBytes,
        static_cast<double>(free_bytes) / (1024.0 * 1024.0 * 1024.0),
        static_cast<double>(total_bytes) / (1024.0 * 1024.0 * 1024.0),
        free_bytes >= kGsSdMinFreeSpaceBytes ? "" : " Low space");
}

std::string getWifiChannelLabel(int channel)
{
    char buf[64];
    if (channel >= 36 && channel <= 48)
    {
        std::snprintf(buf, sizeof(buf), "%d  (5.8GHz,ETSI,FCC)", channel);
    }
    else if (channel >= 52 && channel <= 144)
    {
        std::snprintf(buf, sizeof(buf), "%d  (5.8GHz,ETSI,FCC,DFS)", channel);
    }
    else if (channel >= 149 && channel <= 165)
    {
        std::snprintf(buf, sizeof(buf), "%d  (5.8GHz,FCC)", channel);
    }
    else if (channel == 12 || channel == 13)
    {
        std::snprintf(buf, sizeof(buf), "%d  (2.4GHz,ETSI)", channel);
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "%d  (2.4GHz,ETSI,FCC)", channel);
    }
    return buf;
}

AndroidMenuScreen buildAndroidMenuScreen(const NativeHandle& handle)
{
    AndroidMenuScreen screen;
    screen.selected_item = handle.menu_selected_item;
    screen.can_go_back = !handle.menu_back_ids.empty();
    const auto& config = handle.config_packet;
    constexpr std::array<const char*, 3> kScreenModes = {
        "Stretch",
        "Letterbox",
        "Zoom to Fill"
    };
    constexpr std::array<const char*, 3> kWifiBands = {
        "2.4GHz",
        "5.8GHz",
        "2.4GHz & 5.8GHz"
    };
    constexpr std::array<const char*, 2> kGpioLayouts = {
        "DIY VRX",
        "Runcam VRX"
    };
    constexpr std::array<const char*, 1> kOsdFonts = {
        "Built-in"
    };

    switch (handle.menu_id)
    {
    case AndroidMenuId::Main:
        screen.title = "ESP32-CAM-FPV";
        screen.items.push_back("Search & Connect...");
        screen.items.push_back("Resolution: " + gs::menu::getResolutionSummary(config, handle.session.airStatus().is_ov5640));
        screen.items.push_back("Wifi Channel: " + std::to_string(config.dataChannel.wifi_channel));
        screen.items.push_back("Wifi Rate: " + gs::menu::getWifiRateSummary(config));
        screen.items.push_back("FEC: " + gs::menu::getFecSummary(config));
        screen.items.push_back("Camera Settings...");
        screen.items.push_back("Ground Station Settings...");
        screen.status_lines.push_back(buildAirSdStatus(handle));
        screen.status_lines.push_back(buildGsSdStatus());
        break;

    case AndroidMenuId::Search:
        screen.title = std::string("Menu -> Search (") + kWifiBands[std::clamp(handle.gs_wifi_band, 0, 2)] + ")...";
        if (handle.session.connectedAirDeviceId() != 0)
        {
            screen.status_lines.push_back("Found Channel: " + std::to_string(config.dataChannel.wifi_channel));
        }
        else
        {
            screen.status_lines.push_back("Searching: Channel " + std::to_string(config.dataChannel.wifi_channel) + "...");
        }
        break;

    case AndroidMenuId::GSSettings:
        screen.title = "Menu -> GS Settings";
        screen.items.push_back("Screen Settings...");
        screen.items.push_back("Wifi Settings...");
        screen.items.push_back("Debuging...");
        screen.items.push_back("GPIO Keys Layout: " + std::string(kGpioLayouts[std::clamp(handle.gs_gpio_keys_layout, 0, 1)]));
        screen.items.push_back("Exit To Shell");
        screen.status_lines.push_back("IP: 192.168.4.2");
        break;

    case AndroidMenuId::GSScreen:
        screen.title = "Menu -> GS Screen";
        screen.items.push_back("Screen Mode: " + std::string(kScreenModes[std::clamp(handle.gs_screen_aspect_ratio, 0, 2)]));
        break;

    case AndroidMenuId::Letterbox:
        screen.title = "GS Settings -> Screen Mode";
        for (const char* label : kScreenModes)
        {
            screen.items.push_back(label);
        }
        break;

    case AndroidMenuId::GSWifiSettings:
        screen.title = "Menu -> GS Wifi Settings";
        screen.items.push_back("Band: " + std::string(kWifiBands[std::clamp(handle.gs_wifi_band, 0, 2)]));
        screen.items.push_back("TX Interface: " + handle.gs_tx_interface);
        screen.items.push_back("TX Power: " + std::to_string(handle.gs_tx_power));
        if (handle.transport.getRXDescriptor().interfaces.empty())
        {
            screen.status_lines.push_back("No network interfaces detected");
        }
        else
        {
            for (const auto& iface : handle.transport.getRXDescriptor().interfaces)
            {
                screen.status_lines.push_back(iface);
            }
        }
        break;

    case AndroidMenuId::GSTxInterface:
        screen.title = "GS Settings -> TX Interface";
        screen.items.push_back("auto");
        for (const auto& iface : handle.transport.getRXDescriptor().interfaces)
        {
            screen.items.push_back(iface);
        }
        break;

    case AndroidMenuId::GSTxPower:
        screen.title = "Menu -> Tx Power";
        for (int power = kMinTxPower; power <= kMaxTxPower; ++power)
        {
            screen.items.push_back(std::to_string(power));
        }
        break;

    case AndroidMenuId::Debug:
        screen.title = "Menu -> Debugging";
        screen.items.push_back("Toggle Statistics");
        screen.items.push_back("Draw packets");
        screen.items.push_back("Draw packets till loss");
        screen.items.push_back("Hide packets");
        break;

    case AndroidMenuId::CameraSettings:
        screen.title = "Menu -> Camera Settings";
        screen.items.push_back("Image Settings...");
        screen.items.push_back("OSD Font: " + std::string(kOsdFonts[std::clamp(handle.gs_osd_font_index, 0, static_cast<int>(kOsdFonts.size() - 1))]));
        screen.items.push_back(std::string("Autostart recording: ") + (config.misc.autostartRecord ? "On" : "Off"));
        screen.items.push_back(std::string("Camera Off RC Channel: ") + (config.misc.cameraStopChannel == 0 ? "None" : std::to_string(config.misc.cameraStopChannel)));
        screen.items.push_back(std::string("Mavlink2 to Msp RC: ") + (config.misc.mavlink2mspRC ? "On" : "Off"));
        screen.items.push_back("Air to GS MTU: " + std::to_string(config.dataChannel.fec_codec_mtu));
        break;

    case AndroidMenuId::OSDFont:
        screen.title = "GS -> Displayport OSD Font";
        for (const char* font : kOsdFonts)
        {
            screen.items.push_back(font);
        }
        break;

    case AndroidMenuId::Image:
        screen.title = "Menu -> Image Settings";
        screen.items.push_back("Brightness: " + std::to_string(config.camera.brightness));
        screen.items.push_back("Contrast: " + std::to_string(config.camera.contrast));
        screen.items.push_back("Exposure: " + std::to_string(config.camera.ae_level));
        screen.items.push_back("Saturation: " + std::to_string(config.camera.saturation));
        screen.items.push_back("Sharpness: " + std::string((std::array<const char*, 5>{"Blur more", "Blur", "Normal", "Sharpen", "Sharpen more"})[std::clamp<int>(config.camera.sharpness, -2, 2) + 2]));
        if (!handle.session.airStatus().is_ov5640)
        {
            screen.items.push_back(std::string("Vertical Flip: ") + (config.camera.vflip ? "Enabled" : "Disabled"));
            screen.items.push_back(std::string("40FPS (overclock): ") + (config.camera.ov2640HighFPS ? "Enabled" : "Disabled"));
        }
        else
        {
            screen.items.push_back(std::string("50fps Modes: ") + (config.camera.ov5640HighFPS ? "Enabled" : "Disabled"));
        }
        break;

    case AndroidMenuId::Brightness:
    case AndroidMenuId::Contrast:
    case AndroidMenuId::Exposure:
    case AndroidMenuId::Saturation:
        screen.title = handle.menu_id == AndroidMenuId::Brightness ? "Camera Settings -> Brightness" :
                       handle.menu_id == AndroidMenuId::Contrast ? "Camera Settings -> Contrast" :
                       handle.menu_id == AndroidMenuId::Exposure ? "Camera Settings -> Exposure" :
                                                                    "Camera Settings -> Saturation";
        for (int value = -2; value <= 2; ++value)
        {
            screen.items.push_back(std::to_string(value));
        }
        break;

    case AndroidMenuId::Sharpness:
        screen.title = "Camera Settings -> Sharpness";
        screen.items = {"Blue more", "Blur", "Normal", "Sharpen", "Sharpen more"};
        break;

    case AndroidMenuId::Resolution:
        screen.title = "Menu -> Resolution";
        for (int i = 0; i < 6; ++i)
        {
            screen.items.push_back(gs::menu::getResolutionOptionLabel(config, handle.session.airStatus().is_ov5640, i, true));
        }
        break;

    case AndroidMenuId::WifiRate:
        screen.title = "Menu -> Wifi Rate";
        for (int i = 0; i < 6; ++i)
        {
            screen.items.push_back(gs::menu::getWifiRateOptionLabel(i));
        }
        break;

    case AndroidMenuId::WifiChannel:
        screen.title = "Menu -> Wifi Channel";
        for (int i = 0; i < WIFI_CHANNELS_COUNT; ++i)
        {
            const int channel = WIFI_CHANNELS_BY_INDEX[i];
            if (!isWifiChannelAllowedByBand(channel, handle.gs_wifi_band))
            {
                continue;
            }
            screen.items.push_back(getWifiChannelLabel(channel));
        }
        break;

    case AndroidMenuId::CameraStopCH:
        screen.title = "Menu -> Camera Stop Channel";
        screen.items.push_back("None");
        for (int i = 1; i <= 18; ++i)
        {
            screen.items.push_back(std::to_string(i));
        }
        break;

    case AndroidMenuId::ExitToShell:
        screen.title = "Exit To Shell ?";
        screen.items.push_back("Exit");
        screen.items.push_back("Cancel");
        break;

    case AndroidMenuId::FEC:
        screen.title = "Menu -> FEC";
        for (int i = 0; i < 3; ++i)
        {
            screen.items.push_back(gs::menu::getFecOptionLabel(i));
        }
        break;
    }

    return screen;
}

void handleAndroidMenuSelection(NativeHandle& handle, int item_index)
{
    handle.menu_selected_item = item_index;

    switch (handle.menu_id)
    {
    case AndroidMenuId::Main:
        switch (item_index)
        {
        case 0:
            handle.session.resetPairing(handle.gs_device_id, handle.transport, Clock::now());
            handle.transport.getPacketFilter().set_packet_filtering(0, 0);
            menuGoForward(handle, AndroidMenuId::Search, 0);
            break;
        case 1: menuGoForward(handle, AndroidMenuId::Resolution, gs::menu::getResolutionMenuIndex(handle.config_packet.camera.resolution)); break;
        case 2: menuGoForward(handle, AndroidMenuId::WifiChannel, 0); break;
        case 3: menuGoForward(handle, AndroidMenuId::WifiRate, gs::menu::getWifiRateMenuIndex(handle.config_packet.dataChannel.wifi_rate)); break;
        case 4: menuGoForward(handle, AndroidMenuId::FEC, gs::menu::getFecMenuIndex(handle.config_packet)); break;
        case 5: menuGoForward(handle, AndroidMenuId::CameraSettings, 0); break;
        case 6: menuGoForward(handle, AndroidMenuId::GSSettings, 0); break;
        default: break;
        }
        break;

    case AndroidMenuId::Search:
        break;

    case AndroidMenuId::GSSettings:
        if (item_index == 0)
        {
            menuGoForward(handle, AndroidMenuId::GSScreen, 0);
        }
        else if (item_index == 1)
        {
            menuGoForward(handle, AndroidMenuId::GSWifiSettings, 0);
        }
        else if (item_index == 2)
        {
            menuGoForward(handle, AndroidMenuId::Debug, 0);
        }
        else if (item_index == 3)
        {
            handle.gs_gpio_keys_layout = (handle.gs_gpio_keys_layout + 1) % 2;
        }
        else if (item_index == 4)
        {
            menuGoForward(handle, AndroidMenuId::ExitToShell, 0);
        }
        break;

    case AndroidMenuId::GSScreen:
        if (item_index == 0)
        {
            menuGoForward(handle,
                          AndroidMenuId::Letterbox,
                          std::clamp(handle.gs_screen_aspect_ratio, 0, 2));
        }
        break;

    case AndroidMenuId::Letterbox:
        handle.gs_screen_aspect_ratio = std::clamp(item_index, 0, 2);
        menuGoBack(handle);
        break;

    case AndroidMenuId::GSWifiSettings:
        if (item_index == 0)
        {
            handle.gs_wifi_band = (handle.gs_wifi_band + 1) % 3;
        }
        else if (item_index == 1)
        {
            int selected_index = 0;
            const auto& interfaces = handle.transport.getRXDescriptor().interfaces;
            for (size_t i = 0; i < interfaces.size(); ++i)
            {
                if (interfaces[i] == handle.gs_tx_interface)
                {
                    selected_index = static_cast<int>(i) + 1;
                    break;
                }
            }
            menuGoForward(handle, AndroidMenuId::GSTxInterface, selected_index);
        }
        else if (item_index == 2)
        {
            menuGoForward(handle, AndroidMenuId::GSTxPower, handle.gs_tx_power - kMinTxPower);
        }
        break;

    case AndroidMenuId::GSTxInterface:
        if (item_index <= 0)
        {
            handle.gs_tx_interface = "auto";
        }
        else
        {
            const auto& interfaces = handle.transport.getRXDescriptor().interfaces;
            const size_t interface_index = static_cast<size_t>(item_index - 1);
            if (interface_index < interfaces.size())
            {
                handle.gs_tx_interface = interfaces[interface_index];
            }
        }
        menuGoBack(handle);
        break;

    case AndroidMenuId::GSTxPower:
        handle.gs_tx_power = std::clamp(item_index + kMinTxPower, kMinTxPower, kMaxTxPower);
        menuGoBack(handle);
        break;

    case AndroidMenuId::Debug:
        if (item_index == 0)
        {
            handle.show_stats = !handle.show_stats;
        }
        else if (item_index == 1)
        {
            handle.gs_packet_debug_mode = 1;
        }
        else if (item_index == 2)
        {
            handle.gs_packet_debug_mode = 2;
        }
        else if (item_index == 3)
        {
            handle.gs_packet_debug_mode = 0;
        }
        break;

    case AndroidMenuId::CameraSettings:
        if (item_index == 0)
        {
            menuGoForward(handle, AndroidMenuId::Image, 0);
        }
        else if (item_index == 1)
        {
            menuGoForward(handle, AndroidMenuId::OSDFont, handle.gs_osd_font_index);
        }
        else if (item_index == 2)
        {
            handle.config_packet.misc.autostartRecord ^= 1;
            commitMenuConfig(handle);
        }
        else if (item_index == 3)
        {
            menuGoForward(handle, AndroidMenuId::CameraStopCH, handle.config_packet.misc.cameraStopChannel);
        }
        else if (item_index == 4)
        {
            handle.config_packet.misc.mavlink2mspRC ^= 1;
            commitMenuConfig(handle);
        }
        else if (item_index == 5)
        {
            handle.config_packet.dataChannel.fec_codec_mtu =
                handle.config_packet.dataChannel.fec_codec_mtu == AIR2GROUND_MAX_MTU ? AIR2GROUND_MIN_MTU : AIR2GROUND_MAX_MTU;
            commitMenuConfig(handle);
        }
        break;

    case AndroidMenuId::OSDFont:
        handle.gs_osd_font_index = std::clamp(item_index, 0, 0);
        menuGoBack(handle);
        break;

    case AndroidMenuId::Image:
        if (item_index == 0) menuGoForward(handle, AndroidMenuId::Brightness, handle.config_packet.camera.brightness + 2);
        else if (item_index == 1) menuGoForward(handle, AndroidMenuId::Contrast, handle.config_packet.camera.contrast + 2);
        else if (item_index == 2) menuGoForward(handle, AndroidMenuId::Exposure, handle.config_packet.camera.ae_level + 2);
        else if (item_index == 3) menuGoForward(handle, AndroidMenuId::Saturation, handle.config_packet.camera.saturation + 2);
        else if (item_index == 4) menuGoForward(handle, AndroidMenuId::Sharpness, handle.config_packet.camera.sharpness + 2);
        else if (!handle.session.airStatus().is_ov5640 && item_index == 5)
        {
            handle.config_packet.camera.vflip = !handle.config_packet.camera.vflip;
            handle.config_packet.camera.hmirror = handle.config_packet.camera.vflip;
            commitMenuConfig(handle);
        }
        else if (!handle.session.airStatus().is_ov5640 && item_index == 6)
        {
            handle.config_packet.camera.ov2640HighFPS = !handle.config_packet.camera.ov2640HighFPS;
            commitMenuConfig(handle);
        }
        else if (handle.session.airStatus().is_ov5640 && item_index == 5)
        {
            handle.config_packet.camera.ov5640HighFPS = !handle.config_packet.camera.ov5640HighFPS;
            commitMenuConfig(handle);
        }
        break;

    case AndroidMenuId::Brightness:
        handle.config_packet.camera.brightness = static_cast<int8_t>(item_index - 2);
        commitMenuConfig(handle);
        menuGoBack(handle);
        break;

    case AndroidMenuId::Contrast:
        handle.config_packet.camera.contrast = static_cast<int8_t>(item_index - 2);
        commitMenuConfig(handle);
        menuGoBack(handle);
        break;

    case AndroidMenuId::Exposure:
        handle.config_packet.camera.ae_level = static_cast<int8_t>(item_index - 2);
        commitMenuConfig(handle);
        menuGoBack(handle);
        break;

    case AndroidMenuId::Saturation:
        handle.config_packet.camera.saturation = static_cast<int8_t>(item_index - 2);
        commitMenuConfig(handle);
        menuGoBack(handle);
        break;

    case AndroidMenuId::Sharpness:
        handle.config_packet.camera.sharpness = static_cast<int8_t>(item_index - 2);
        commitMenuConfig(handle);
        menuGoBack(handle);
        break;

    case AndroidMenuId::Resolution:
        handle.config_packet.camera.resolution = gs::menu::getResolutionForMenuIndex(item_index);
        commitMenuConfig(handle);
        menuGoBack(handle);
        break;

    case AndroidMenuId::WifiRate:
        handle.config_packet.dataChannel.wifi_rate = gs::menu::getWifiRateForMenuIndex(item_index);
        commitMenuConfig(handle);
        menuGoBack(handle);
        break;

    case AndroidMenuId::WifiChannel:
        {
            int visible_index = 0;
            for (int i = 0; i < WIFI_CHANNELS_COUNT; ++i)
            {
                const int channel = WIFI_CHANNELS_BY_INDEX[i];
                if (!isWifiChannelAllowedByBand(channel, handle.gs_wifi_band))
                {
                    continue;
                }
                if (visible_index == item_index)
                {
                    handle.config_packet.dataChannel.wifi_channel = channel;
                    commitMenuConfig(handle);
                    break;
                }
                visible_index++;
            }
        }
        menuGoBack(handle);
        break;

    case AndroidMenuId::CameraStopCH:
        if (item_index >= 0 && item_index <= 18)
        {
            handle.config_packet.misc.cameraStopChannel = static_cast<uint8_t>(item_index);
            commitMenuConfig(handle);
        }
        menuGoBack(handle);
        break;

    case AndroidMenuId::ExitToShell:
        if (item_index == 0)
        {
            handle.exit_requested = true;
        }
        menuGoBack(handle);
        break;

    case AndroidMenuId::FEC:
        handle.config_packet.dataChannel.fec_codec_k = FEC_K;
        handle.config_packet.dataChannel.fec_codec_n = gs::menu::getFecNForMenuIndex(item_index);
        commitMenuConfig(handle);
        menuGoBack(handle);
        break;
    }
}

void moveAndroidMenuSelection(NativeHandle& handle, int delta)
{
    const AndroidMenuScreen screen = buildAndroidMenuScreen(handle);
    if (screen.items.empty())
    {
        return;
    }

    const int max_index = static_cast<int>(screen.items.size()) - 1;
    handle.menu_selected_item = std::clamp(handle.menu_selected_item + delta, 0, max_index);
}

AndroidMenuNavAction hitTestAndroidMenuNav(float x, float y, float view_width, float view_height)
{
    const NavPadLayout nav = buildNavPadLayout(view_width, view_height);
    if (pointInRect(x, y, nav.center_x, nav.up_y, nav.size, nav.size))
    {
        return AndroidMenuNavAction::Up;
    }
    if (pointInRect(x, y, nav.left_x, nav.mid_y, nav.size, nav.size))
    {
        return AndroidMenuNavAction::Back;
    }
    if (pointInRect(x, y, nav.right_x, nav.mid_y, nav.size, nav.size))
    {
        return AndroidMenuNavAction::Activate;
    }
    if (pointInRect(x, y, nav.center_x, nav.down_y, nav.size, nav.size))
    {
        return AndroidMenuNavAction::Down;
    }
    return AndroidMenuNavAction::None;
}

jlong toJLong(NativeHandle* handle)
{
    return reinterpret_cast<jlong>(handle);
}

NativeHandle* fromJLong(jlong handle)
{
    return reinterpret_cast<NativeHandle*>(handle);
}

jstring newJavaString(JNIEnv* env, const std::string& value)
{
    return env->NewStringUTF(value.c_str());
}

std::string fromJString(JNIEnv* env, jstring value)
{
    if (env == nullptr || value == nullptr)
    {
        return {};
    }

    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr)
    {
        return {};
    }

    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

jbyteArray toJByteArray(JNIEnv* env, const std::vector<uint8_t>& bytes)
{
    jbyteArray array = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (array == nullptr || bytes.empty())
    {
        return array;
    }

    env->SetByteArrayRegion(
        array,
        0,
        static_cast<jsize>(bytes.size()),
        reinterpret_cast<const jbyte*>(bytes.data()));
    return array;
}

std::vector<uint8_t> fromJByteArray(JNIEnv* env, jbyteArray data)
{
    std::vector<uint8_t> bytes;
    if (data == nullptr)
    {
        return bytes;
    }

    const jsize size = env->GetArrayLength(data);
    if (size <= 0)
    {
        return bytes;
    }

    bytes.resize(static_cast<size_t>(size));
    env->GetByteArrayRegion(data, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
    return bytes;
}

jobjectArray toJByteArrayArray(JNIEnv* env, const std::vector<std::vector<uint8_t>>& packets)
{
    jclass byte_array_class = env->FindClass("[B");
    if (byte_array_class == nullptr)
    {
        return nullptr;
    }

    jobjectArray result = env->NewObjectArray(static_cast<jsize>(packets.size()), byte_array_class, nullptr);
    if (result == nullptr)
    {
        return nullptr;
    }

    for (size_t i = 0; i < packets.size(); ++i)
    {
        jbyteArray packet_array = toJByteArray(env, packets[i]);
        env->SetObjectArrayElement(result, static_cast<jsize>(i), packet_array);
        env->DeleteLocalRef(packet_array);
    }

    return result;
}

jobjectArray toJStringArray(JNIEnv* env, const std::vector<std::string>& items)
{
    jclass string_class = env->FindClass("java/lang/String");
    if (string_class == nullptr)
    {
        return nullptr;
    }

    jobjectArray result = env->NewObjectArray(static_cast<jsize>(items.size()), string_class, nullptr);
    if (result == nullptr)
    {
        return nullptr;
    }

    for (size_t i = 0; i < items.size(); ++i)
    {
        jstring value = env->NewStringUTF(items[i].c_str());
        env->SetObjectArrayElement(result, static_cast<jsize>(i), value);
        env->DeleteLocalRef(value);
    }

    return result;
}

std::string buildInfoString()
{
    std::ostringstream out;
    out << "Native core loaded"
        << " | cfg=" << sizeof(Ground2Air_Config_Packet)
        << " | video=" << sizeof(Air2Ground_Video_Packet)
        << " | telemetry_max=" << GROUND2AIR_DATA_MAX_PAYLOAD_SIZE;
    return out.str();
}

std::string sanitizeStatusValue(std::string value);

std::string describeHandleString(const NativeHandle& handle)
{
    std::ostringstream out;
    out << "Session ready"
        << " | gs_id=" << handle.gs_device_id
        << " | connected_air=" << handle.session.connectedAirDeviceId()
        << " | frame_index=" << handle.assembler.currentFrameIndex()
        << " | got_config=" << (handle.session.gotConfigPacket() ? "yes" : "no")
        << " | last_event=" << static_cast<int>(handle.last_event_kind)
        << " | buffered_frame=" << handle.last_completed_frame.size()
        << " | rx_transport=" << handle.transport_packets_seen
        << " | rx_pass=" << handle.transport_packets_passed_filter
        << " | rx_drop=" << handle.transport_packets_filtered
        << " | rx_decoded=" << handle.decoded_packets_seen
        << " | dec_type=" << handle.last_decoded_type
        << " | dec_size=" << handle.last_decoded_size
        << " | dec_air=" << handle.last_decoded_air
        << " | dec_gs=" << handle.last_decoded_gs
        << " | last_block=" << handle.last_transport_block
        << " | last_pkt=" << handle.last_transport_packet_index
        << " | last_payload=" << handle.last_transport_payload_size
        << " | last_from=" << handle.last_transport_from
        << " | last_to=" << handle.last_transport_to
        << " | vid_frame=" << handle.last_video_frame_index
        << " | vid_part=" << handle.last_video_part_index
        << " | vid_last=" << handle.last_video_last_part
        << " | vid_payload=" << handle.last_video_payload_size
        << " | udp_running=" << (handle.udp_running ? 1 : 0)
        << " | udp_peer=" << sanitizeStatusValue(handle.udp_peer_host + ":" + std::to_string(handle.udp_peer_port))
        << " | udp_packets=" << handle.udp_packets_received
        << " | udp_mbps=" << std::fixed << std::setprecision(2) << handle.udp_throughput_mbps
        << " | udp_fps=" << std::fixed << std::setprecision(2) << handle.udp_video_fps
        << " | udp_error=" << sanitizeStatusValue(handle.udp_last_error)
        << " | air_rssi=" << -static_cast<int>(handle.session.lastAirStats().rssiDbm)
        << " | queue_max=" << handle.session.airStatus().wifi_queue_max
        << " | wifi_ovf=" << (handle.session.airStatus().wifi_ovf ? 1 : 0)
        << " | air_record=" << (handle.session.airStatus().air_record ? 1 : 0)
        << " | resolution=" << gs::menu::getResolutionSummary(handle.config_packet, handle.session.airStatus().is_ov5640);
    return out.str();
}

int processVideoPacket(NativeHandle& handle,
                       const gs::core::SessionEvent& session_event,
                       bool restored_by_fec)
{
    const auto& video_view = session_event.video;
    auto result = handle.assembler.pushPacket(
        *video_view.packet,
        video_view.payload,
        video_view.payload_size,
        restored_by_fec);

    if (result.completedFrame && result.frameData != nullptr)
    {
        handle.jpeg_decoder.submitJpeg(result.frameData->data(), result.frameData->size());
    }

    return static_cast<int>(session_event.kind);
}

uint16_t readLe16(const uint8_t* data)
{
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLe32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

std::string sanitizeStatusValue(std::string value)
{
    std::replace(value.begin(), value.end(), '|', '/');
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value;
}

std::string buildAndroidMenuTitle(const NativeHandle& handle, const AndroidMenuScreen& screen)
{
    if (screen.title == "ESP32-CAM-FPV")
    {
        return std::string("ESP32-CAM-FPV v0.5.3 ") +
               (handle.session.airStatus().is_ov5640 ? "OV5640" : "OV2640");
    }
    return screen.title;
}

bool tryParseVideoPacketWire(const uint8_t* packet_data,
                             size_t packet_size,
                             Air2Ground_Video_Packet& out_packet,
                             const uint8_t*& out_payload,
                             size_t& out_payload_size)
{
    out_packet = {};
    out_payload = nullptr;
    out_payload_size = 0;

    if (packet_data == nullptr || packet_size < sizeof(Air2Ground_Video_Packet))
    {
        return false;
    }

    const uint32_t declared_size = readLe32(packet_data + 1);
    if (declared_size < sizeof(Air2Ground_Video_Packet))
    {
        return false;
    }

    const size_t bounded_size = std::min<size_t>(declared_size, packet_size);
    if (bounded_size < sizeof(Air2Ground_Video_Packet))
    {
        return false;
    }

    out_packet.type = static_cast<Air2Ground_Header::Type>(packet_data[0]);
    out_packet.size = declared_size;
    out_packet.pong = packet_data[5];
    out_packet.version = packet_data[6];
    out_packet.crc = packet_data[7];
    out_packet.airDeviceId = readLe16(packet_data + 8);
    out_packet.gsDeviceId = readLe16(packet_data + 10);
    out_packet.resolution = static_cast<Resolution>(packet_data[12]);
    out_packet.part_index = packet_data[13] & 0x7Fu;
    out_packet.last_part = (packet_data[13] >> 7) & 0x01u;
    out_packet.frame_index = readLe32(packet_data + 14);

    out_payload = packet_data + sizeof(Air2Ground_Video_Packet);
    out_payload_size = bounded_size - sizeof(Air2Ground_Video_Packet);
    return true;
}

std::vector<uint8_t> makeTransportPacket(uint16_t from_device_id,
                                         uint16_t to_device_id,
                                         uint32_t block_index,
                                         uint8_t packet_index,
                                         const uint8_t* payload,
                                         size_t payload_size)
{
    constexpr size_t transport_payload_size = GROUND2AIR_MAX_MTU;
    std::vector<uint8_t> packet(sizeof(Packet_Header) + transport_payload_size, 0);
    auto* header = reinterpret_cast<Packet_Header*>(packet.data());
    header->packet_version = PACKET_VERSION;
    header->packet_signature = PACKET_SIGNATURE;
    header->fromDeviceId = from_device_id;
    header->toDeviceId = to_device_id;
    header->size = transport_payload_size;
    header->block_index = block_index;
    header->packet_index = packet_index;

    const size_t copy_size = std::min(payload_size, transport_payload_size);
    std::memcpy(packet.data() + sizeof(Packet_Header), payload, copy_size);
    return packet;
}

std::vector<std::vector<uint8_t>> buildTransportPacketsForControl(NativeHandle& handle,
                                                                  const uint8_t* payload,
                                                                  size_t payload_size)
{
    constexpr size_t transport_payload_size = GROUND2AIR_MAX_MTU;
    std::array<uint8_t, transport_payload_size> current_payload = {};
    const size_t copy_size = std::min(payload_size, transport_payload_size);
    std::memcpy(current_payload.data(), payload, copy_size);

    const uint16_t to_device_id = handle.session.connectedAirDeviceId();
    std::vector<std::vector<uint8_t>> packets;

    if (!handle.tx_block_has_first_packet)
    {
        handle.tx_first_packet_payload = current_payload;
        handle.tx_block_has_first_packet = true;
        packets.push_back(makeTransportPacket(handle.gs_device_id,
                                              to_device_id,
                                              handle.next_tx_block_index,
                                              0,
                                              current_payload.data(),
                                              transport_payload_size));
        return packets;
    }

    packets.push_back(makeTransportPacket(handle.gs_device_id,
                                          to_device_id,
                                          handle.next_tx_block_index,
                                          1,
                                          current_payload.data(),
                                          transport_payload_size));

    if (handle.tx_fec)
    {
        std::array<uint8_t, transport_payload_size> fec_payload = {};
        const gf* src_ptrs[2] = {
            handle.tx_first_packet_payload.data(),
            current_payload.data()
        };
        gf* fec_ptrs[1] = { fec_payload.data() };
        const unsigned block_nums[1] = { 2 };
        fec_encode(handle.tx_fec, src_ptrs, fec_ptrs, block_nums, 1, transport_payload_size);

        packets.push_back(makeTransportPacket(handle.gs_device_id,
                                              to_device_id,
                                              handle.next_tx_block_index,
                                              2,
                                              fec_payload.data(),
                                              transport_payload_size));
    }

    handle.tx_block_has_first_packet = false;
    handle.next_tx_block_index++;
    return packets;
}

int processTransportPacket(NativeHandle& handle,
                           const uint8_t* data,
                           size_t size,
                           bool restored_by_fec,
                           int input_dbm)
{
    if (data == nullptr || size == 0)
    {
        handle.last_event_kind = gs::core::SessionEventKind::Ignore;
        return static_cast<int>(handle.last_event_kind);
    }

    handle.transport.setInputDbm(input_dbm);
    handle.last_event_kind = gs::core::SessionEventKind::Ignore;

    if (size >= sizeof(Packet_Header))
    {
        const auto* header = reinterpret_cast<const Packet_Header*>(data);
        handle.transport_packets_seen++;
        handle.last_transport_block = header->block_index;
        handle.last_transport_packet_index = header->packet_index;
        handle.last_transport_payload_size = header->size;
        handle.last_transport_from = header->fromDeviceId;
        handle.last_transport_to = header->toDeviceId;
    }

    const bool use_transport_filter = false;
    const auto filter_result = PacketFilter::PacketFilterResult::Pass;
    if (filter_result != PacketFilter::PacketFilterResult::Pass)
    {
        handle.transport_packets_filtered++;
        return static_cast<int>(handle.last_event_kind);
    }

    handle.transport_packets_passed_filter++;
    handle.rx_decoder.push(
        data,
        size,
        handle.transport.getPacketFilter(),
        use_transport_filter,
        [&](const uint8_t* decoded_payload, size_t decoded_size)
        {
            handle.decoded_packets_seen++;
            const uint8_t* packet_data = decoded_payload;
            size_t packet_size = decoded_size;

            if (packet_size >= sizeof(Air2Ground_Header))
            {
                const auto* header = reinterpret_cast<const Air2Ground_Header*>(packet_data);
                handle.last_decoded_type = static_cast<uint32_t>(header->type);
                handle.last_decoded_size = header->size;
                handle.last_decoded_air = header->airDeviceId;
                handle.last_decoded_gs = header->gsDeviceId;
            }

            const gs::core::SessionEvent session_event = handle.session.processReceivedPacket(
                packet_data,
                packet_size,
                handle.gs_device_id,
                Clock::now(),
                handle.transport);

            handle.last_event_kind = session_event.kind;

            if (session_event.kind == gs::core::SessionEventKind::VideoPacket)
            {
                Air2Ground_Video_Packet video_packet = {};
                const uint8_t* video_payload = nullptr;
                size_t video_payload_size = 0;
                if (!tryParseVideoPacketWire(packet_data,
                                             packet_size,
                                             video_packet,
                                             video_payload,
                                             video_payload_size))
                {
                    handle.last_event_kind = gs::core::SessionEventKind::InvalidVideoPacket;
                }
                else
                {
                    handle.last_video_frame_index = video_packet.frame_index;
                    handle.last_video_part_index = video_packet.part_index;
                    handle.last_video_last_part = video_packet.last_part;
                    handle.last_video_payload_size = static_cast<uint32_t>(video_payload_size);

                    auto result = handle.assembler.pushPacket(
                        video_packet,
                        video_payload,
                        video_payload_size,
                        restored_by_fec);

                    if (result.completedFrame && result.frameData != nullptr)
                    {
                        handle.jpeg_decoder.submitJpeg(result.frameData->data(), result.frameData->size());
                    }
                }
            }

            handle.session.syncConfigPacket(handle.config_packet);
            handle.rx_decoder.setDescriptor(
                handle.config_packet.dataChannel.fec_codec_k,
                handle.config_packet.dataChannel.fec_codec_n,
                handle.config_packet.dataChannel.fec_codec_mtu);
        });

    return static_cast<int>(handle.last_event_kind);
}

std::vector<std::vector<uint8_t>> buildControlTransportPacketsLocked(NativeHandle& handle)
{
    const gs::core::ControlPacketView view = handle.session.buildControlPacket(handle.gs_device_id);
    std::vector<uint8_t> payload;
    switch (view.type)
    {
    case gs::core::ControlPacketType::Connect:
        payload.resize(sizeof(view.connect_packet));
        std::memcpy(payload.data(), &view.connect_packet, sizeof(view.connect_packet));
        break;
    case gs::core::ControlPacketType::Config:
        payload.resize(sizeof(view.config_packet));
        std::memcpy(payload.data(), &view.config_packet, sizeof(view.config_packet));
        break;
    case gs::core::ControlPacketType::None:
    default:
        return {};
    }

    return buildTransportPacketsForControl(handle, payload.data(), payload.size());
}

void updateUdpStatsLocked(NativeHandle& handle,
                          uint64_t packets_received,
                          float throughput_mbps,
                          float video_fps)
{
    handle.udp_packets_received = packets_received;
    handle.udp_throughput_mbps = throughput_mbps;
    handle.udp_video_fps = video_fps;
}

void syncRendererOverlayLocked(NativeHandle& handle, const std::string& build_info)
{
    std::vector<AndroidVideoRenderer::OverlayChip> chips;
    chips.push_back({"AIR:" + std::to_string(-static_cast<int>(handle.session.lastAirStats().rssiDbm)), false});
    chips.push_back({"GS:UDP", false});
    chips.push_back({std::to_string(handle.session.airStatus().wifi_queue_max) + "%", false});

    std::ostringstream throughput_text;
    throughput_text << std::fixed << std::setprecision(1) << handle.udp_throughput_mbps << "Mb";
    chips.push_back({throughput_text.str(), false});
    chips.push_back({gs::menu::getResolutionSummary(handle.config_packet, handle.session.airStatus().is_ov5640), false});
    chips.push_back({std::to_string(static_cast<int>(std::round(handle.udp_video_fps))), false});
    if (handle.session.airStatus().wifi_ovf)
    {
        chips.push_back({"OVF", true});
    }
    if (handle.session.airStatus().air_record)
    {
        chips.push_back({"AIR", true});
    }

    AndroidVideoRenderer::OverlayMenuState menu_state;
    if (handle.menu_visible)
    {
        const AndroidMenuScreen screen = buildAndroidMenuScreen(handle);
        menu_state.visible = true;
        menu_state.title = buildAndroidMenuTitle(handle, screen);
        menu_state.items = screen.items;
        menu_state.statuses = screen.statuses;
        menu_state.status_lines = screen.status_lines;
        menu_state.selected_index = screen.selected_item;
        menu_state.footer = build_info;
    }

    handle.renderer.setOverlayState(chips, menu_state);
}

void handleTapLocked(NativeHandle& handle,
                     float x,
                     float y,
                     float view_width,
                     float view_height)
{
    if (view_width <= 0.0f || view_height <= 0.0f)
    {
        return;
    }

    if (!handle.menu_visible)
    {
        handle.menu_visible = true;
        handle.menu_id = AndroidMenuId::Main;
        handle.menu_selected_item = 0;
        handle.menu_back_ids.clear();
        handle.menu_back_items.clear();
        return;
    }

    const AndroidMenuScreen screen = buildAndroidMenuScreen(handle);
    switch (hitTestAndroidMenuNav(x, y, view_width, view_height))
    {
    case AndroidMenuNavAction::Up:
        moveAndroidMenuSelection(handle, -1);
        return;
    case AndroidMenuNavAction::Down:
        moveAndroidMenuSelection(handle, 1);
        return;
    case AndroidMenuNavAction::Back:
        menuGoBack(handle);
        return;
    case AndroidMenuNavAction::Activate:
        if (!screen.items.empty())
        {
            handleAndroidMenuSelection(handle, std::clamp(handle.menu_selected_item, 0, static_cast<int>(screen.items.size()) - 1));
        }
        return;
    case AndroidMenuNavAction::None:
        break;
    }

    const float scale = linuxMenuScaleForSurface(view_width, view_height);
    const float menu_width = kLinuxMenuWindowWidth * scale;
    const float menu_height = kLinuxMenuWindowHeight * scale;
    const float menu_x = std::floor((view_width - menu_width) * 0.5f);
    const float menu_offset_y = (std::lround(view_height) == 576) ? 100.0f : kLinuxMenuCenterYOffset;
    const float menu_y = std::floor((view_height - menu_height) * 0.5f + menu_offset_y * scale);

    if (x < menu_x || x > menu_x + menu_width || y < menu_y || y > menu_y + menu_height)
    {
        menuGoBack(handle);
        return;
    }

    const float button_height = kLinuxMenuButtonHeight * scale;
    const float button_width = kLinuxMenuButtonWidth * scale;
    const float item_x = menu_x + kLinuxMenuItemInset * scale;
    const float large_gap = (view_height > 480.0f) ? kLinuxMenuGapLarge * scale : 0.0f;
    float cursor_y = menu_y + button_height + large_gap;

    for (size_t index = 0; index < screen.items.size(); ++index)
    {
        const float row_y = cursor_y;
        if (y >= row_y && y <= row_y + button_height &&
            x >= item_x && x <= item_x + button_width)
        {
            handleAndroidMenuSelection(handle, static_cast<int>(index));
            return;
        }
        cursor_y += button_height;
    }
}

void runUdpClient(NativeHandle& handle,
                  std::string peer_host,
                  int peer_port,
                  int local_port)
{
    using ClockType = std::chrono::steady_clock;

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* addrinfo_result = nullptr;
    const std::string peer_port_string = std::to_string(peer_port);
    if (getaddrinfo(peer_host.c_str(), peer_port_string.c_str(), &hints, &addrinfo_result) != 0 ||
        addrinfo_result == nullptr)
    {
        std::lock_guard<std::mutex> lock(handle.mutex);
        handle.udp_running = false;
        handle.udp_last_error = "peer resolve failed";
        return;
    }

    const int socket_fd = socket(addrinfo_result->ai_family, addrinfo_result->ai_socktype, addrinfo_result->ai_protocol);
    if (socket_fd < 0)
    {
        freeaddrinfo(addrinfo_result);
        std::lock_guard<std::mutex> lock(handle.mutex);
        handle.udp_running = false;
        handle.udp_last_error = "socket create failed";
        return;
    }

    sockaddr_in local_address = {};
    local_address.sin_family = AF_INET;
    local_address.sin_port = htons(static_cast<uint16_t>(local_port));
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, reinterpret_cast<const sockaddr*>(&local_address), sizeof(local_address)) != 0)
    {
        close(socket_fd);
        freeaddrinfo(addrinfo_result);
        std::lock_guard<std::mutex> lock(handle.mutex);
        handle.udp_running = false;
        handle.udp_last_error = "bind failed";
        return;
    }

    timeval timeout = {};
    timeout.tv_usec = 250000;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    {
        std::lock_guard<std::mutex> lock(handle.mutex);
        handle.udp_running = true;
        handle.udp_last_error.clear();
        updateUdpStatsLocked(handle, 0, 0.0f, 0.0f);
    }

    std::array<uint8_t, 2048> rx_buffer = {};
    uint64_t packets_received = 0;
    uint64_t bytes_window = 0;
    uint64_t frame_count_start = handle.jpeg_decoder.submittedFrameCount();
    auto stats_window_start = ClockType::now();
    auto next_control_send = ClockType::now();

    while (!handle.udp_stop_requested.load())
    {
        const auto now = ClockType::now();
        if (now >= next_control_send)
        {
            std::vector<std::vector<uint8_t>> control_packets;
            {
                std::lock_guard<std::mutex> lock(handle.mutex);
                control_packets = buildControlTransportPacketsLocked(handle);
            }
            for (const auto& packet : control_packets)
            {
                if (!packet.empty())
                {
                    sendto(socket_fd,
                           packet.data(),
                           packet.size(),
                           0,
                           addrinfo_result->ai_addr,
                           static_cast<socklen_t>(addrinfo_result->ai_addrlen));
                }
            }
            next_control_send = now + std::chrono::milliseconds(250);
        }

        ssize_t received = recvfrom(socket_fd, rx_buffer.data(), rx_buffer.size(), 0, nullptr, nullptr);
        if (received > 0)
        {
            packets_received++;
            bytes_window += static_cast<uint64_t>(received);
            std::lock_guard<std::mutex> lock(handle.mutex);
            processTransportPacket(handle, rx_buffer.data(), static_cast<size_t>(received), false, 0);
        }
        else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            std::lock_guard<std::mutex> lock(handle.mutex);
            handle.udp_last_error = std::string("recv failed: ") + std::strerror(errno);
            break;
        }

        const auto stats_now = ClockType::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(stats_now - stats_window_start).count();
        if (elapsed_ms >= 1000)
        {
            const uint64_t frame_count_now = handle.jpeg_decoder.submittedFrameCount();
            const float throughput_mbps = static_cast<float>(bytes_window * 8.0) /
                                          static_cast<float>(elapsed_ms * 1000.0);
            const float video_fps = static_cast<float>((frame_count_now - frame_count_start) * 1000.0) /
                                    static_cast<float>(elapsed_ms);

            {
                std::lock_guard<std::mutex> lock(handle.mutex);
                updateUdpStatsLocked(handle, packets_received, throughput_mbps, video_fps);
            }

            bytes_window = 0;
            frame_count_start = frame_count_now;
            stats_window_start = stats_now;
        }
    }

    close(socket_fd);
    freeaddrinfo(addrinfo_result);

    std::lock_guard<std::mutex> lock(handle.mutex);
    handle.udp_running = false;
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_getBuildInfo(JNIEnv* env, jobject /* thiz */)
{
    return newJavaString(env, buildInfoString());
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_createHandle(JNIEnv* /* env */,
                                                       jobject /* thiz */,
                                                       jint gsDeviceId)
{
    auto* handle = new NativeHandle(static_cast<uint16_t>(gsDeviceId));
    return toJLong(handle);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_describeHandle(JNIEnv* env,
                                                         jobject /* thiz */,
                                                         jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return newJavaString(env, "Session handle is null");
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return newJavaString(env, describeHandleString(*native_handle));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_pushPacket(JNIEnv* env,
                                                     jobject /* thiz */,
                                                     jlong handle,
                                                     jbyteArray data,
                                                     jboolean restoredByFec,
                                                     jint inputDbm)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return static_cast<jint>(gs::core::SessionEventKind::Ignore);
    }

    std::vector<uint8_t> bytes = fromJByteArray(env, data);
    if (bytes.empty())
    {
        std::lock_guard<std::mutex> lock(native_handle->mutex);
        native_handle->last_event_kind = gs::core::SessionEventKind::Ignore;
        return static_cast<jint>(native_handle->last_event_kind);
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return static_cast<jint>(processTransportPacket(
        *native_handle,
        bytes.data(),
        bytes.size(),
        restoredByFec == JNI_TRUE,
        static_cast<int>(inputDbm)));
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_buildControlTransportPackets(JNIEnv* env,
                                                                       jobject /* thiz */,
                                                                       jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    auto packets = buildControlTransportPacketsLocked(*native_handle);
    if (packets.empty())
    {
        return nullptr;
    }
    return toJByteArrayArray(env, packets);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_takeCompletedFrame(JNIEnv* env,
                                                             jobject /* thiz */,
                                                             jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    if (native_handle->last_completed_frame.empty())
    {
        return nullptr;
    }

    std::vector<uint8_t> frame = std::move(native_handle->last_completed_frame);
    native_handle->last_completed_frame.clear();
    return toJByteArray(env, frame);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_startUdpClient(JNIEnv* env,
                                                         jobject /* thiz */,
                                                         jlong handle,
                                                         jstring peer_host,
                                                         jint peer_port,
                                                         jint local_port)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return JNI_FALSE;
    }

    const std::string peer = fromJString(env, peer_host);
    if (peer.empty())
    {
        return JNI_FALSE;
    }

    bool join_stale_thread = false;
    {
        std::lock_guard<std::mutex> lock(native_handle->mutex);
        if (native_handle->udp_running)
        {
            return JNI_FALSE;
        }
        join_stale_thread = native_handle->udp_thread.joinable();
    }

    if (join_stale_thread)
    {
        native_handle->stopUdpClient();
    }

    {
        std::lock_guard<std::mutex> lock(native_handle->mutex);
        if (native_handle->udp_running || native_handle->udp_thread.joinable())
        {
            return JNI_FALSE;
        }
        native_handle->udp_peer_host = peer;
        native_handle->udp_peer_port = static_cast<int>(peer_port);
        native_handle->udp_local_port = static_cast<int>(local_port);
        native_handle->udp_last_error.clear();
    }

    native_handle->udp_stop_requested.store(false);
    native_handle->udp_thread = std::thread(
        runUdpClient,
        std::ref(*native_handle),
        peer,
        static_cast<int>(peer_port),
        static_cast<int>(local_port));
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_stopUdpClient(JNIEnv* /* env */,
                                                        jobject /* thiz */,
                                                        jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    native_handle->stopUdpClient();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_isUdpClientRunning(JNIEnv* /* env */,
                                                             jobject /* thiz */,
                                                             jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return JNI_FALSE;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return native_handle->udp_running ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_getLastEventKind(JNIEnv* /* env */,
                                                           jobject /* thiz */,
                                                           jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return static_cast<jint>(gs::core::SessionEventKind::Ignore);
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return static_cast<jint>(native_handle->last_event_kind);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_getScreenAspectRatio(JNIEnv* /* env */,
                                                               jobject /* thiz */,
                                                               jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return static_cast<jint>(std::clamp(native_handle->gs_screen_aspect_ratio, 0, 2));
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_setRendererScreenMode(JNIEnv* /* env */,
                                                                jobject /* thiz */,
                                                                jlong handle,
                                                                jint screen_mode)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    native_handle->renderer.setScreenMode(std::clamp(static_cast<int>(screen_mode), 0, 2));
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_setFontAtlasPng(JNIEnv* env,
                                                          jobject /* thiz */,
                                                          jlong handle,
                                                          jbyteArray png_bytes)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr || png_bytes == nullptr)
    {
        return;
    }

    std::vector<uint8_t> bytes = fromJByteArray(env, png_bytes);
    if (bytes.empty())
    {
        return;
    }

    native_handle->renderer.setFontAtlasPng(bytes.data(), bytes.size());
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_setMenuFontTtf(JNIEnv* env,
                                                         jobject /* thiz */,
                                                         jlong handle,
                                                         jbyteArray ttf_bytes)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr || ttf_bytes == nullptr)
    {
        return;
    }

    std::vector<uint8_t> bytes = fromJByteArray(env, ttf_bytes);
    if (bytes.empty())
    {
        return;
    }

    native_handle->renderer.setMenuFontTtf(bytes.data(), bytes.size());
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_syncRendererOverlay(JNIEnv* env,
                                                              jobject /* thiz */,
                                                              jlong handle,
                                                              jstring build_info)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    const std::string info = fromJString(env, build_info);
    std::lock_guard<std::mutex> lock(native_handle->mutex);
    syncRendererOverlayLocked(*native_handle, info);
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_handleTap(JNIEnv* /* env */,
                                                    jobject /* thiz */,
                                                    jlong handle,
                                                    jfloat x,
                                                    jfloat y,
                                                    jfloat view_width,
                                                    jfloat view_height)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    handleTapLocked(*native_handle,
                    static_cast<float>(x),
                    static_cast<float>(y),
                    static_cast<float>(view_width),
                    static_cast<float>(view_height));
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_setRenderSurface(JNIEnv* env,
                                                           jobject /* thiz */,
                                                           jlong handle,
                                                           jobject surface)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr || surface == nullptr)
    {
        return;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    native_handle->renderer.setSurface(window);
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_clearRenderSurface(JNIEnv* /* env */,
                                                             jobject /* thiz */,
                                                             jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    native_handle->renderer.clearSurface();
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_submitVideoFrame(JNIEnv* env,
                                                           jobject /* thiz */,
                                                           jlong handle,
                                                           jobject rgba,
                                                           jint width,
                                                           jint height,
                                                           jint stride)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr || rgba == nullptr)
    {
        return;
    }

    auto* data = static_cast<uint8_t*>(env->GetDirectBufferAddress(rgba));
    const jlong capacity = env->GetDirectBufferCapacity(rgba);
    if (data == nullptr || capacity <= 0)
    {
        return;
    }

    native_handle->renderer.submitFrame(
        data,
        static_cast<size_t>(capacity),
        static_cast<int>(width),
        static_cast<int>(height),
        static_cast<int>(stride));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_isMenuVisible(JNIEnv* /* env */,
                                                        jobject /* thiz */,
                                                        jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return JNI_FALSE;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return native_handle->menu_visible ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_setMenuVisible(JNIEnv* /* env */,
                                                         jobject /* thiz */,
                                                         jlong handle,
                                                         jboolean visible)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    native_handle->menu_visible = visible == JNI_TRUE;
    if (native_handle->menu_visible)
    {
        native_handle->menu_id = AndroidMenuId::Main;
        native_handle->menu_selected_item = 0;
        native_handle->menu_back_ids.clear();
        native_handle->menu_back_items.clear();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_menuGoBack(JNIEnv* /* env */,
                                                     jobject /* thiz */,
                                                     jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    menuGoBack(*native_handle);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_getMenuSelectedIndex(JNIEnv* /* env */,
                                                               jobject /* thiz */,
                                                               jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return buildAndroidMenuScreen(*native_handle).selected_item;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_getMenuTitle(JNIEnv* env,
                                                       jobject /* thiz */,
                                                       jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return env->NewStringUTF("");
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    const AndroidMenuScreen screen = buildAndroidMenuScreen(*native_handle);
    return env->NewStringUTF(screen.title.c_str());
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_getMenuItems(JNIEnv* env,
                                                       jobject /* thiz */,
                                                       jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return toJStringArray(env, buildAndroidMenuScreen(*native_handle).items);
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_getMenuStatuses(JNIEnv* env,
                                                          jobject /* thiz */,
                                                          jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return toJStringArray(env, buildAndroidMenuScreen(*native_handle).statuses);
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_getMenuStatusLines(JNIEnv* env,
                                                             jobject /* thiz */,
                                                             jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return toJStringArray(env, buildAndroidMenuScreen(*native_handle).status_lines);
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_menuSelectItem(JNIEnv* /* env */,
                                                         jobject /* thiz */,
                                                         jlong handle,
                                                         jint item_index)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    handleAndroidMenuSelection(*native_handle, static_cast<int>(item_index));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_consumeExitRequested(JNIEnv* /* env */,
                                                               jobject /* thiz */,
                                                               jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return JNI_FALSE;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    const bool requested = native_handle->exit_requested;
    native_handle->exit_requested = false;
    return requested ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_resetSession(JNIEnv* /* env */,
                                                       jobject /* thiz */,
                                                       jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    native_handle->session.resetPairing(native_handle->gs_device_id,
                                        native_handle->transport,
                                        Clock::now());
    native_handle->transport.getPacketFilter().set_packet_filtering(0, 0);
    native_handle->rx_decoder.reset();
    native_handle->last_event_kind = gs::core::SessionEventKind::Ignore;
    native_handle->last_completed_frame.clear();
    native_handle->last_video_frame_index = 0;
    native_handle->last_video_part_index = 0;
    native_handle->last_video_last_part = 0;
    native_handle->last_video_payload_size = 0;
    native_handle->menu_visible = false;
    native_handle->menu_id = AndroidMenuId::Main;
    native_handle->menu_selected_item = 0;
    native_handle->menu_back_ids.clear();
    native_handle->menu_back_items.clear();
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_destroyHandle(JNIEnv* /* env */,
                                                        jobject /* thiz */,
                                                        jlong handle)
{
    delete fromJLong(handle);
}
