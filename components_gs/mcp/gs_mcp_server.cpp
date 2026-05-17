#include "gs_mcp_server.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "Log.h"
#include "core/transport_manager_base.h"
#include "frame_packets_debug.h"
#include "gs_runtime_config.h"
#include "gs_runtime_core.h"
#include "gs_runtime_state.h"

namespace gs::mcp
{

namespace
{

constexpr uint16_t kDefaultPort = 17654;
constexpr size_t kMaxRequestBytes = 64 * 1024;

struct QueuedKeyPress
{
    ImGuiKey key = ImGuiKey_None;
    bool key_down_sent = false;
};

struct InputQueueState
{
    std::mutex mutex;
    std::vector<QueuedKeyPress> pending;
};

//===================================================================================
//===================================================================================
// Stores the MCP listener thread state and the currently served client socket.
struct GsMcpServerImpl
{
    std::mutex mutex;
    std::thread thread;
    std::atomic<bool> running = false;
    int listen_fd = -1;
    int active_client_fd = -1;
    uint16_t port = kDefaultPort;
};

InputQueueState g_input_queue;
GsMcpServerImpl g_server_impl;

void closeSocketFd(int& fd);
bool setFdCloseOnExec(int fd);
std::string jsonEscape(const std::string& value);
std::string trimCopy(const std::string& value);
size_t skipJsonWhitespace(const std::string& json, size_t pos);
size_t findMatchingJsonDelimiter(const std::string& json, size_t start, char open_ch, char close_ch);
std::string extractTopLevelJsonValue(const std::string& json, const std::string& key);
std::string parseJsonStringLiteral(const std::string& raw);
std::vector<std::string> parseJsonStringArray(const std::string& raw);
std::string jsonStringArray(const std::vector<std::string>& values);
ImGuiKey parseInjectedKeyName(const std::string& key_name);
const char* linkStateLabel(LinkState state);
const char* transportKindLabel(gs::core::TransportKind kind);
std::string buildMenuBufferJson();
std::string buildSnapshotJson();
std::string makeJsonRpcResult(const std::string& id_raw, const std::string& result_json);
std::string makeJsonRpcError(const std::string& id_raw, int code, const std::string& message);
std::string buildInitializeResult();
std::string buildToolsListResult();
std::string makeToolCallSuccess(const std::string& payload_json);
bool sendAll(int fd, const std::string& response);
std::string handleToolCall(const std::string& id_raw, const std::string& params_raw);
std::string handleRequestLine(const std::string& request_line);
void serveClient(int client_fd);
void serverThreadProc();

} // namespace

GsMcpServer& GsMcpServer::instance()
{
    static GsMcpServer server;
    return server;
}

void GsMcpServer::start(uint16_t port)
{
    if (g_server_impl.running.load())
    {
        return;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0)
    {
        LOGE("Failed to create MCP socket: {}", std::strerror(errno));
        return;
    }

    if (!setFdCloseOnExec(listen_fd))
    {
        LOGW("Failed to mark MCP listen socket close-on-exec");
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port == 0 ? kDefaultPort : port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        LOGE("Failed to bind MCP socket on {}: {}", port, std::strerror(errno));
        closeSocketFd(listen_fd);
        return;
    }

    if (listen(listen_fd, 1) != 0)
    {
        LOGE("Failed to listen on MCP socket: {}", std::strerror(errno));
        closeSocketFd(listen_fd);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_server_impl.mutex);
        g_server_impl.listen_fd = listen_fd;
        g_server_impl.port = port == 0 ? kDefaultPort : port;
        g_server_impl.running = true;
        g_server_impl.thread = std::thread(serverThreadProc);
    }

    LOGI("GS MCP server listening on 0.0.0.0:{}", g_server_impl.port);
}

void GsMcpServer::stop()
{
    if (!g_server_impl.running.exchange(false))
    {
        return;
    }

    int listen_fd = -1;
    int active_client_fd = -1;
    {
        std::lock_guard<std::mutex> lock(g_server_impl.mutex);
        listen_fd = g_server_impl.listen_fd;
        g_server_impl.listen_fd = -1;
        active_client_fd = g_server_impl.active_client_fd;
    }
    // Exit To Shell can be triggered through MCP while the server thread is still
    // waiting in recv() on that client. Shutdown wakes the thread so Activity
    // destruction is not left stuck in GsMcpServer::stop(). The server thread
    // remains responsible for closing the client fd it owns.
    if (active_client_fd >= 0)
    {
        shutdown(active_client_fd, SHUT_RDWR);
    }
    // Android can leave accept() blocked when another thread only closes the
    // listening descriptor. Shutdown first so the server thread observes the
    // stop request and NativeHandle destruction can complete.
    if (listen_fd >= 0)
    {
        shutdown(listen_fd, SHUT_RDWR);
    }
    closeSocketFd(listen_fd);

    if (g_server_impl.thread.joinable())
    {
        g_server_impl.thread.join();
    }
}

bool GsMcpServer::isRunning() const
{
    return g_server_impl.running.load();
}

uint16_t GsMcpServer::port() const
{
    return g_server_impl.port;
}

void queueInjectedKeyPress(ImGuiKey key)
{
    if (key == ImGuiKey_None)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_input_queue.mutex);
    g_input_queue.pending.push_back({key});
}

void queueInjectedKeyPresses(const std::vector<ImGuiKey>& keys)
{
    std::lock_guard<std::mutex> lock(g_input_queue.mutex);
    for (const ImGuiKey key : keys)
    {
        if (key == ImGuiKey_None)
        {
            continue;
        }
        g_input_queue.pending.push_back({key});
    }
}

void drainInjectedKeysToImGui()
{
    InjectedKeyTransition transition = {};
    if (!popInjectedKeyTransition(transition))
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(transition.key, transition.down);
}

bool popInjectedKeyTransition(InjectedKeyTransition& transition)
{
    std::lock_guard<std::mutex> lock(g_input_queue.mutex);
    if (g_input_queue.pending.empty())
    {
        return false;
    }

    QueuedKeyPress& key_press = g_input_queue.pending.front();
    transition.key = key_press.key;
    if (!key_press.key_down_sent)
    {
        key_press.key_down_sent = true;
        transition.down = true;
        return true;
    }

    transition.down = false;
    g_input_queue.pending.erase(g_input_queue.pending.begin());
    return true;
}

namespace
{

void closeSocketFd(int& fd)
{
    if (fd >= 0)
    {
        ::close(fd);
        fd = -1;
    }
}

//===================================================================================
//===================================================================================
// Marks a socket file descriptor close-on-exec to avoid inheritance by child processes.
bool setFdCloseOnExec(int fd)
{
    if (fd < 0)
    {
        return false;
    }

    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0)
    {
        return false;
    }

    return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

std::string jsonEscape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size() + 16);

    for (const char ch : value)
    {
        switch (ch)
        {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(ch));
                escaped += buffer;
            }
            else
            {
                escaped += ch;
            }
            break;
        }
    }

    return escaped;
}

std::string trimCopy(const std::string& value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
    {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
    {
        --end;
    }

    return value.substr(begin, end - begin);
}

size_t skipJsonWhitespace(const std::string& json, size_t pos)
{
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])) != 0)
    {
        ++pos;
    }
    return pos;
}

size_t findMatchingJsonDelimiter(const std::string& json, size_t start, char open_ch, char close_ch)
{
    int depth = 0;
    bool in_string = false;
    bool escape = false;

    for (size_t i = start; i < json.size(); ++i)
    {
        const char ch = json[i];
        if (in_string)
        {
            if (escape)
            {
                escape = false;
            }
            else if (ch == '\\')
            {
                escape = true;
            }
            else if (ch == '"')
            {
                in_string = false;
            }
            continue;
        }

        if (ch == '"')
        {
            in_string = true;
            continue;
        }
        if (ch == open_ch)
        {
            ++depth;
            continue;
        }
        if (ch == close_ch)
        {
            --depth;
            if (depth == 0)
            {
                return i;
            }
        }
    }

    return std::string::npos;
}

std::string extractTopLevelJsonValue(const std::string& json, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    bool in_string = false;
    bool escape = false;
    int object_depth = 0;

    for (size_t i = 0; i < json.size(); ++i)
    {
        const char ch = json[i];
        if (in_string)
        {
            if (escape)
            {
                escape = false;
            }
            else if (ch == '\\')
            {
                escape = true;
            }
            else if (ch == '"')
            {
                in_string = false;
            }
            continue;
        }

        if (object_depth == 1 && i + needle.size() <= json.size() && json.compare(i, needle.size(), needle) == 0)
        {
            size_t colon = skipJsonWhitespace(json, i + needle.size());
            if (colon >= json.size() || json[colon] != ':')
            {
                continue;
            }
            size_t value_begin = skipJsonWhitespace(json, colon + 1);
            if (value_begin >= json.size())
            {
                return {};
            }

            const char first = json[value_begin];
            if (first == '"')
            {
                size_t end = value_begin + 1;
                bool local_escape = false;
                while (end < json.size())
                {
                    if (local_escape)
                    {
                        local_escape = false;
                    }
                    else if (json[end] == '\\')
                    {
                        local_escape = true;
                    }
                    else if (json[end] == '"')
                    {
                        return json.substr(value_begin, end - value_begin + 1);
                    }
                    ++end;
                }
                return {};
            }

            if (first == '{')
            {
                const size_t end = findMatchingJsonDelimiter(json, value_begin, '{', '}');
                return end == std::string::npos ? std::string() : json.substr(value_begin, end - value_begin + 1);
            }

            if (first == '[')
            {
                const size_t end = findMatchingJsonDelimiter(json, value_begin, '[', ']');
                return end == std::string::npos ? std::string() : json.substr(value_begin, end - value_begin + 1);
            }

            size_t end = value_begin;
            while (end < json.size() && json[end] != ',' && json[end] != '}')
            {
                ++end;
            }
            return trimCopy(json.substr(value_begin, end - value_begin));
        }

        if (ch == '"')
        {
            in_string = true;
            continue;
        }
        if (ch == '{')
        {
            ++object_depth;
            continue;
        }
        if (ch == '}')
        {
            --object_depth;
            continue;
        }
    }

    return {};
}

std::string parseJsonStringLiteral(const std::string& raw)
{
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"')
    {
        return {};
    }

    std::string value;
    value.reserve(raw.size() - 2);
    for (size_t i = 1; i + 1 < raw.size(); ++i)
    {
        const char ch = raw[i];
        if (ch != '\\')
        {
            value += ch;
            continue;
        }

        ++i;
        if (i + 1 >= raw.size())
        {
            break;
        }

        switch (raw[i])
        {
        case '\\': value += '\\'; break;
        case '"': value += '"'; break;
        case '/': value += '/'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        default: value += raw[i]; break;
        }
    }

    return value;
}

std::vector<std::string> parseJsonStringArray(const std::string& raw)
{
    std::vector<std::string> values;
    if (raw.size() < 2 || raw.front() != '[' || raw.back() != ']')
    {
        return values;
    }

    size_t pos = 1;
    while (pos + 1 < raw.size())
    {
        pos = skipJsonWhitespace(raw, pos);
        if (pos >= raw.size() || raw[pos] == ']')
        {
            break;
        }
        if (raw[pos] != '"')
        {
            break;
        }

        size_t end = pos + 1;
        bool escape = false;
        while (end < raw.size())
        {
            if (escape)
            {
                escape = false;
            }
            else if (raw[end] == '\\')
            {
                escape = true;
            }
            else if (raw[end] == '"')
            {
                break;
            }
            ++end;
        }
        if (end >= raw.size())
        {
            break;
        }

        values.push_back(parseJsonStringLiteral(raw.substr(pos, end - pos + 1)));
        pos = skipJsonWhitespace(raw, end + 1);
        if (pos < raw.size() && raw[pos] == ',')
        {
            ++pos;
        }
    }

    return values;
}

std::string jsonStringArray(const std::vector<std::string>& values)
{
    std::ostringstream out;
    out << '[';
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i != 0)
        {
            out << ',';
        }
        out << '"' << jsonEscape(values[i]) << '"';
    }
    out << ']';
    return out.str();
}

ImGuiKey parseInjectedKeyName(const std::string& key_name)
{
    std::string upper = key_name;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch)
    {
        return static_cast<char>(std::toupper(ch));
    });

    if (upper == "UP") return ImGuiKey_UpArrow;
    if (upper == "DOWN") return ImGuiKey_DownArrow;
    if (upper == "LEFT") return ImGuiKey_LeftArrow;
    if (upper == "RIGHT") return ImGuiKey_RightArrow;
    if (upper == "ENTER") return ImGuiKey_Enter;
    if (upper == "KEYPADENTER") return ImGuiKey_KeypadEnter;
    if (upper == "ESC" || upper == "ESCAPE") return ImGuiKey_Escape;
    if (upper == "R") return ImGuiKey_R;
    if (upper == "G") return ImGuiKey_G;
    if (upper == "S") return ImGuiKey_S;
    if (upper == "SPACE") return ImGuiKey_Space;
    return ImGuiKey_None;
}

const char* linkStateLabel(LinkState state)
{
    switch (state)
    {
    case LinkState::None: return "None";
    case LinkState::LookingForWifiNetwork: return "LookingForWifiNetwork";
    case LinkState::ConnectingToWifiNetwork: return "ConnectingToWifiNetwork";
    case LinkState::ConnectingToStream: return "ConnectingToStream";
    }

    return "Unknown";
}

const char* transportKindLabel(gs::core::TransportKind kind)
{
    return gs::core::TransportManagerBase::transportModeLabel(kind);
}

std::string buildMenuBufferJson()
{
    const gs::menu::CapturedMenuBuffer menu = gs::menu::g_osdMenuController.copyCapturedMenuBuffer();

    std::ostringstream out;
    out << '{'
        << "\"visible\":" << (menu.visible ? "true" : "false") << ','
        << "\"menu_id\":" << static_cast<int>(menu.menu_id) << ','
        << "\"selected_item\":" << menu.selected_item << ','
        << "\"title\":\"" << jsonEscape(menu.title) << "\","
        << "\"lines\":" << jsonStringArray(menu.lines)
        << '}';
    return out.str();
}

std::string buildSnapshotJson()
{
    const ApfpvCameraStateSnapshot apfpv_state = copyApfpvCameraState();
    GSStats last_gs_stats = {};
    {
        std::lock_guard<std::mutex> lock(s_gs_stats_mutex);
        last_gs_stats = s_runtimeCore.last_ground_stats;
    }

    std::ostringstream discovered_cameras;
    discovered_cameras << '[';
    for (size_t i = 0; i < apfpv_state.discovered_cameras.size(); ++i)
    {
        if (i != 0)
        {
            discovered_cameras << ',';
        }
        const ApfpvCameraDescriptor& camera = apfpv_state.discovered_cameras[i];
        discovered_cameras << '{'
                           << "\"device_id\":" << camera.device_id << ','
                           << "\"device_hex\":\"" << jsonEscape(formatApfpvCameraId(camera.device_id)) << "\","
                           << "\"ssid\":\"" << jsonEscape(camera.ssid) << "\""
                           << '}';
    }
    discovered_cameras << ']';

    std::ostringstream out;
    out << '{'
        << "\"link_state\":\"" << linkStateLabel(getLinkState()) << "\","
        << "\"transport_kind\":\"" << jsonEscape(transportKindLabel(currentTransportKind())) << "\","
        << "\"menu\":" << buildMenuBufferJson() << ','
        << "\"apfpv\":{"
        << "\"preferred_camera_id\":" << apfpv_state.preferred_camera_id << ','
        << "\"preferred_camera_hex\":\"" << jsonEscape(formatApfpvCameraId(apfpv_state.preferred_camera_id)) << "\","
        << "\"active_camera_id\":" << apfpv_state.active_camera_id << ','
        << "\"active_camera_hex\":\"" << jsonEscape(formatApfpvCameraId(apfpv_state.active_camera_id)) << "\","
        << "\"discovered_cameras\":" << discovered_cameras.str()
        << "},"
        << "\"session\":{"
        << "\"gs_device_id\":" << s_runtimeCore.gs_device_id << ','
        << "\"connected_air_device_id\":" << s_runtimeCore.session.connectedAirDeviceId() << ','
        << "\"got_config_packet\":" << (s_runtimeCore.session.gotConfigPacket() ? "true" : "false") << ','
        << "\"accept_config_packet\":" << (s_runtimeCore.session.acceptConfigPacket() ? "true" : "false")
        << "},"
        << "\"transport_debug\":{"
        << "\"transport_packets_seen\":" << s_runtimeCore.transport_packets_seen << ','
        << "\"transport_packets_passed_filter\":" << s_runtimeCore.transport_packets_passed_filter << ','
        << "\"transport_packets_filtered\":" << s_runtimeCore.transport_packets_filtered << ','
        << "\"decoded_packets_seen\":" << s_runtimeCore.decoded_packets_seen << ','
        << "\"last_transport_block\":" << s_runtimeCore.last_transport_block << ','
        << "\"last_transport_packet_index\":" << s_runtimeCore.last_transport_packet_index << ','
        << "\"last_transport_payload_size\":" << s_runtimeCore.last_transport_payload_size << ','
        << "\"last_transport_from\":" << s_runtimeCore.last_transport_from << ','
        << "\"last_transport_to\":" << s_runtimeCore.last_transport_to
        << "},"
        << "\"ground_stats\":{"
        << "\"discarded_frames_assembler_pool_overflow\":" << last_gs_stats.discardedFramesAssemblerPoolOverflow << ','
        << "\"discarded_frames_decoder_input\":" << last_gs_stats.discardedFramesDecoderInput << ','
        << "\"discarded_frames_decoded_output\":" << last_gs_stats.discardedFramesDecodedOutput << ','
        << "\"decoded_jpeg_count\":" << last_gs_stats.decodedJpegCount << ','
        << "\"decoded_jpeg_time_min_ms\":" << last_gs_stats.decodedJpegTimeMinMS << ','
        << "\"decoded_jpeg_time_max_ms\":" << last_gs_stats.decodedJpegTimeMaxMS << ','
        << "\"texture_upload_count\":" << last_gs_stats.textureUploadCount << ','
        << "\"texture_upload_time_min_ms\":" << last_gs_stats.textureUploadTimeMinMS << ','
        << "\"texture_upload_time_max_ms\":" << last_gs_stats.textureUploadTimeMaxMS << ','
        << "\"received_completed_frames\":" << last_gs_stats.receivedCompletedFrames << ','
        << "\"restored_completed_frames\":" << last_gs_stats.restoredCompletedFrames
        << "},"
        << "\"frame_debug\":{"
        << "\"enabled\":" << (g_framePacketsDebug.isOn() ? "true" : "false") << ','
        << "\"visible\":" << (g_framePacketsDebug.isVisible() ? "true" : "false")
        << '}'
        << '}';
    return out.str();
}

std::string makeJsonRpcResult(const std::string& id_raw, const std::string& result_json)
{
    const std::string id_value = id_raw.empty() ? "null" : id_raw;
    return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + id_value + ",\"result\":" + result_json + "}\n";
}

std::string makeJsonRpcError(const std::string& id_raw, int code, const std::string& message)
{
    const std::string id_value = id_raw.empty() ? "null" : id_raw;
    std::ostringstream out;
    out << "{\"jsonrpc\":\"2.0\",\"id\":" << id_value << ",\"error\":{\"code\":" << code
        << ",\"message\":\"" << jsonEscape(message) << "\"}}\n";
    return out.str();
}

std::string buildInitializeResult()
{
    return "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
           "\"serverInfo\":{\"name\":\"esp32-cam-fpv-gs\",\"version\":\"1\"}}";
}

std::string buildToolsListResult()
{
    return
        "{\"tools\":["
        "{\"name\":\"gs_get_snapshot\",\"description\":\"Return the current GS runtime snapshot and captured menu buffer.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
        "{\"name\":\"gs_get_menu_buffer\",\"description\":\"Return the last captured OSD menu buffer with [*] marking the selected item.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
        "{\"name\":\"gs_press_key\",\"description\":\"Inject one synthetic key press into the GS menu input path.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"}},\"required\":[\"key\"],\"additionalProperties\":false}},"
        "{\"name\":\"gs_press_keys\",\"description\":\"Inject multiple synthetic key presses into the GS menu input path in order.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"keys\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"keys\"],\"additionalProperties\":false}}"
        "]}";
}

std::string makeToolCallSuccess(const std::string& payload_json)
{
    std::ostringstream out;
    out << '{'
        << "\"content\":[{\"type\":\"text\",\"text\":\"" << jsonEscape(payload_json) << "\"}],"
        << "\"structuredContent\":" << payload_json
        << '}';
    return out.str();
}

bool sendAll(int fd, const std::string& response)
{
    size_t sent = 0;
    while (sent < response.size())
    {
        const ssize_t rc = ::send(fd, response.data() + sent, response.size() - sent, 0);
        if (rc <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(rc);
    }
    return true;
}

std::string handleToolCall(const std::string& id_raw, const std::string& params_raw)
{
    const std::string tool_name = parseJsonStringLiteral(extractTopLevelJsonValue(params_raw, "name"));
    const std::string arguments_raw = extractTopLevelJsonValue(params_raw, "arguments");

    if (tool_name == "gs_get_snapshot")
    {
        return makeJsonRpcResult(id_raw, makeToolCallSuccess(buildSnapshotJson()));
    }
    if (tool_name == "gs_get_menu_buffer")
    {
        return makeJsonRpcResult(id_raw, makeToolCallSuccess(buildMenuBufferJson()));
    }
    if (tool_name == "gs_press_key")
    {
        const std::string key_name = parseJsonStringLiteral(extractTopLevelJsonValue(arguments_raw, "key"));
        const ImGuiKey key = parseInjectedKeyName(key_name);
        if (key == ImGuiKey_None)
        {
            return makeJsonRpcError(id_raw, -32602, "Unknown key name");
        }
        queueInjectedKeyPress(key);
        return makeJsonRpcResult(id_raw, makeToolCallSuccess(std::string("{\"queued\":true,\"count\":1,\"key\":\"") + jsonEscape(key_name) + "\"}"));
    }
    if (tool_name == "gs_press_keys")
    {
        const std::vector<std::string> key_names = parseJsonStringArray(extractTopLevelJsonValue(arguments_raw, "keys"));
        std::vector<ImGuiKey> keys;
        keys.reserve(key_names.size());
        for (const std::string& key_name : key_names)
        {
            const ImGuiKey key = parseInjectedKeyName(key_name);
            if (key == ImGuiKey_None)
            {
                return makeJsonRpcError(id_raw, -32602, "Unknown key name in keys array");
            }
            keys.push_back(key);
        }
        queueInjectedKeyPresses(keys);
        return makeJsonRpcResult(id_raw, makeToolCallSuccess(std::string("{\"queued\":true,\"count\":") + std::to_string(keys.size()) + "}"));
    }

    return makeJsonRpcError(id_raw, -32601, "Unknown tool name");
}

std::string handleRequestLine(const std::string& request_line)
{
    const std::string method = parseJsonStringLiteral(extractTopLevelJsonValue(request_line, "method"));
    const std::string id_raw = extractTopLevelJsonValue(request_line, "id");

    if (method.empty())
    {
        return makeJsonRpcError(id_raw, -32600, "Missing method");
    }

    if (method == "initialize")
    {
        return makeJsonRpcResult(id_raw, buildInitializeResult());
    }

    if (method == "ping")
    {
        return makeJsonRpcResult(id_raw, "{}");
    }

    if (method == "tools/list")
    {
        return makeJsonRpcResult(id_raw, buildToolsListResult());
    }

    if (method == "tools/call")
    {
        return handleToolCall(id_raw, extractTopLevelJsonValue(request_line, "params"));
    }

    if (method == "notifications/initialized")
    {
        return {};
    }

    return makeJsonRpcError(id_raw, -32601, "Unknown method");
}

void serveClient(int client_fd)
{
    std::string buffer;
    buffer.reserve(4096);
    char recv_buffer[1024];

    while (g_server_impl.running.load())
    {
        const ssize_t received = ::recv(client_fd, recv_buffer, sizeof(recv_buffer), 0);
        if (received <= 0)
        {
            break;
        }

        buffer.append(recv_buffer, recv_buffer + received);
        if (buffer.size() > kMaxRequestBytes)
        {
            LOGW("MCP client request exceeded {} bytes, closing connection", kMaxRequestBytes);
            break;
        }

        size_t newline_pos = 0;
        while ((newline_pos = buffer.find('\n')) != std::string::npos)
        {
            const std::string request = trimCopy(buffer.substr(0, newline_pos));
            buffer.erase(0, newline_pos + 1);
            if (request.empty())
            {
                continue;
            }

            const std::string response = handleRequestLine(request);
            if (!response.empty() && !sendAll(client_fd, response))
            {
                return;
            }
        }
    }
}

void serverThreadProc()
{
    int local_listen_fd = -1;
    {
        std::lock_guard<std::mutex> lock(g_server_impl.mutex);
        local_listen_fd = g_server_impl.listen_fd;
    }

    while (g_server_impl.running.load())
    {
        sockaddr_in client_addr = {};
        socklen_t client_len = sizeof(client_addr);
        const int client_fd = ::accept(local_listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0)
        {
            if (!g_server_impl.running.load())
            {
                break;
            }
            if (errno == EINTR)
            {
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (!setFdCloseOnExec(client_fd))
        {
            LOGW("Failed to mark MCP client socket close-on-exec");
        }

        {
            std::lock_guard<std::mutex> lock(g_server_impl.mutex);
            g_server_impl.active_client_fd = client_fd;
        }
        serveClient(client_fd);
        {
            std::lock_guard<std::mutex> lock(g_server_impl.mutex);
            if (g_server_impl.active_client_fd == client_fd)
            {
                g_server_impl.active_client_fd = -1;
            }
        }
        int fd_to_close = client_fd;
        closeSocketFd(fd_to_close);
    }
}

} // namespace

} // namespace gs::mcp
