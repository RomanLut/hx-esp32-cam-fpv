#include <jni.h>
#include <android/log.h>
#include <android/asset_manager_jni.h>
#include <android/native_window_jni.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <sys/statvfs.h>
#include <unistd.h>

#include "android_bitmap_jpeg_decoder.h"
#include "android_jni_shared.h"
#include "android_osd_font_storage.h"
#include "android_recordings_storage.h"
#include "../../../../../components/common/avi.h"
#include "android_runtime_platform_services.h"
#include "android_transport_manager.h"
#include "gs_video_renderer.h"
#include "fec.h"
#include "fec_block_decoder.h"
#include "crc.h"
#include "lodepng.h"
#include "core/gs_session_core.h"
#include "frame_packets_debug.h"
#include "core/osd_menu_common.h"
#include "core/osd_menu_controller.h"
#include "core/osd_menu_imgui_shared.h"
#include "flight_osd.h"
#include "gs_runtime_config.h"
#include "gs_runtime_core.h"
#include "gs_runtime_event_pipeline.h"
#include "gs_runtime_event_flow.h"
#include "gs_runtime_protocol.h"
#include "gs_runtime_sync.h"
#include "gs_runtime_video_flow.h"
#include "gs_runtime_osd_font_storage.h"
#include "gs_shared_runtime.h"
#include "gs_runtime_state.h"
#include "gs_stats.h"
#include "core/video_frame_assembler.h"
#include "packet_filter.h"
#include "settings_storage.h"
#include "ISerialTelemetry.h"
#include "android_serial_telemetry.h"
#include "gs_runtime_core.h"
#include "gs_udp_broadcast.h"
#include "android_udp_broadcast.h"
#include "../../../../../components_gs/mcp/gs_mcp_server.h"

static AndroidSerialTelemetry s_androidSerialTelemetry;
ISerialTelemetry* g_serialTelemetry = &s_androidSerialTelemetry;

static AndroidGSUDPBroadcast s_androidGSUDPBroadcast;
IGSUDPBroadcast* g_gsUDPBroadcast = &s_androidGSUDPBroadcast;

IRuntimePlatformServices* s_RuntimePlatformServices = nullptr;
IOSDFontStorage* s_OSDFontStorage = nullptr;
RecordingsStorage* s_recordingsStorage = nullptr;
FlightOSD s_flightOSD;

namespace
{

constexpr const char* kNativeLogTag = "AndroidNativeBridge";

constexpr uint64_t kGsSdMinFreeSpaceBytes = 20ull * 1024ull * 1024ull;

struct NativeHandle;

struct NativeHandle
{
    explicit NativeHandle(uint16_t gs_device_id_value)
        : renderer(),
          jpeg_decoder(renderer)
    {
        s_RuntimePlatformServices = &getAndroidRuntimePlatformServices();
        bindAndroidRuntimeRenderer(&renderer);
        s_OSDFontStorage = &getAndroidOsdFontStorage();
        s_recordingsStorage = &getAndroidRecordingsStorage();
        s_runtimeCore.resetState(gs_device_id_value);
        loadSharedSettings(s_runtimeCore.gs_device_id);
        prepAviBuffers();
        s_recordingsStorage->refreshGroundStorageStatus();
        s_ground2air_config_packet = s_runtimeCore.session.copyConfigPacket();
        s_runtimeCore.gs_device_id = s_groundstation_config.deviceId;
        s_transportManager = &transport_manager;
        {
            gs::core::RXDescriptor rx_descriptor;
            rx_descriptor.coding_k = FEC_K;
            rx_descriptor.coding_n = FEC_N;
            rx_descriptor.mtu = AIR2GROUND_MAX_MTU;

            gs::core::TXDescriptor tx_descriptor;
            tx_descriptor.coding_k = 2;
            tx_descriptor.coding_n = 3;
            tx_descriptor.mtu = GROUND2AIR_MAX_MTU;

            transport_manager.init(s_groundstation_config.transportKind, rx_descriptor, tx_descriptor);
        }
        s_runtimeCore.resetTransportRuntime(*s_transport, Clock::now());
        last_control_packet_tp = Clock::now();
        renderer.setMenuBinding(&gs::menu::g_osdMenuController, &s_runtimeCore.config_packet, &mutex);
        gs::mcp::GsMcpServer::instance().start();
    }

    ~NativeHandle()
    {
        stop_background.store(true);
        if (background_runtime_thread && background_runtime_thread->joinable())
        {
            background_runtime_thread->join();
        }
        bindAndroidRuntimeRenderer(nullptr);
        stopUdpClient();
        transport_manager.rawBroadcastTransport().stopUsbAdapter();
        gs::mcp::GsMcpServer::instance().stop();
    }

    void stopUdpClient()
    {
        transport_manager.apfpvTransport().stopUdpClient();
        std::lock_guard<std::mutex> lock(mutex);
        transport_manager.apfpvTransport().setUdpRunning(false);
    }

    mutable std::mutex mutex;
    AndroidTransportManager transport_manager;
    GsVideoRenderer renderer;
    AndroidBitmapJpegDecoder jpeg_decoder;
    Clock::time_point last_control_packet_tp = Clock::now();
    int gs_thermal_status = 0;
    int gs_battery_percent = -1;
    std::atomic<bool> stop_background = false;
    std::unique_ptr<std::thread> background_runtime_thread;
};

//===================================================================================
//===================================================================================
// Feeds one already-decoded session packet through the shared Android runtime pipeline.
void processSessionPacketLocked(NativeHandle& handle,
                                const uint8_t* packet_data,
                                size_t packet_size,
                                bool restored_by_fec);

//===================================================================================
//===================================================================================
// Polls the active transport directly for session packets used by shared in-process transports.
void pollActiveTransportPacketsLocked(NativeHandle& handle);

//===================================================================================
//===================================================================================
// Periodically sends the shared connect/config control packet on in-process transports.
void pumpSharedControlPacketLocked(NativeHandle& handle, Clock::time_point now);

//===================================================================================
//===================================================================================
// Builds the outgoing APFPV control transport packets for the active session state.
std::vector<std::vector<uint8_t>> buildControlTransportPacketsLocked(NativeHandle& handle);

//===================================================================================
//===================================================================================
// Injects one APFPV transport packet into the Android runtime decoder pipeline.
int processTransportPacket(NativeHandle& handle,
                           const uint8_t* data,
                           size_t size,
                           bool restored_by_fec,
                           int input_dbm);

//===================================================================================
//===================================================================================
// Starts or stops the APFPV UDP backend to match the currently active transport.
void syncApfpvUdpClient(NativeHandle& handle);

//===================================================================================
//===================================================================================
// Applies any deferred Android renderer invalidation requested by shared runtime code.
void applyPendingRendererInvalidation(NativeHandle& handle);

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

std::string buildInfoString()
{
    std::ostringstream out;
    out << "Native core loaded"
        << " | cfg=" << sizeof(Ground2Air_Config_Packet)
        << " | video=" << sizeof(Air2Ground_Video_Packet)
        << " | telemetry_max=" << GROUND2AIR_DATA_MAX_PAYLOAD_SIZE;
    return out.str();
}

//===================================================================================
//===================================================================================
// Builds a transport-agnostic runtime status summary for the JNI debug surface.
std::string describeHandleString(const NativeHandle& handle)
{
    std::ostringstream out;
    out << "Session ready"
        << " | gs_id=" << s_runtimeCore.gs_device_id
        << " | transport=" << static_cast<int>(handle.transport_manager.activeKind())
        << " | connected_air=" << s_runtimeCore.session.connectedAirDeviceId()
        << " | frame_index=" << s_runtimeCore.assembler.currentFrameIndex()
        << " | got_config=" << (s_runtimeCore.session.gotConfigPacket() ? "yes" : "no")
        << " | last_event=" << static_cast<int>(s_runtimeCore.last_event_kind)
        << " | buffered_frame=" << s_runtimeCore.last_completed_frame.size()
        << " | rx_transport=" << s_runtimeCore.transport_packets_seen
        << " | rx_pass=" << s_runtimeCore.transport_packets_passed_filter
        << " | rx_drop=" << s_runtimeCore.transport_packets_filtered
        << " | rx_decoded=" << s_runtimeCore.decoded_packets_seen
        << " | dec_type=" << s_runtimeCore.last_decoded_type
        << " | dec_size=" << s_runtimeCore.last_decoded_size
        << " | dec_air=" << s_runtimeCore.last_decoded_air
        << " | dec_gs=" << s_runtimeCore.last_decoded_gs
        << " | last_block=" << s_runtimeCore.last_transport_block
        << " | last_pkt=" << s_runtimeCore.last_transport_packet_index
        << " | last_payload=" << s_runtimeCore.last_transport_payload_size
        << " | last_from=" << s_runtimeCore.last_transport_from
        << " | last_to=" << s_runtimeCore.last_transport_to
        << " | vid_frame=" << s_runtimeCore.last_video_frame_index
        << " | vid_part=" << s_runtimeCore.last_video_part_index
        << " | vid_last=" << s_runtimeCore.last_video_last_part
        << " | vid_payload=" << s_runtimeCore.last_video_payload_size
        << " | air_rssi=" << -static_cast<int>(s_runtimeCore.session.lastAirStats().rssiDbm)
        << " | queue_max=" << static_cast<int>(s_runtimeCore.session.lastAirStats().wifi_queue_max)
        << " | wifi_ovf=" << static_cast<int>(s_runtimeCore.session.lastAirStats().wifi_ovf)
        << " | air_record=" << static_cast<int>(s_runtimeCore.session.lastAirStats().air_record_state)
        << " | resolution=" << gs::menu::getResolutionSummary(s_runtimeCore.config_packet, s_runtimeCore.session.lastAirStats().isOV5640 != 0);
    return out.str();
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

    const uint16_t to_device_id = s_runtimeCore.session.connectedAirDeviceId();
    std::vector<std::vector<uint8_t>> packets;

    if (!s_runtimeCore.tx_block_has_first_packet)
    {
        s_runtimeCore.tx_first_packet_payload = current_payload;
        s_runtimeCore.tx_block_has_first_packet = true;
        packets.push_back(makeTransportPacket(s_runtimeCore.gs_device_id,
                                              to_device_id,
                                              s_runtimeCore.next_tx_block_index,
                                              0,
                                              current_payload.data(),
                                              transport_payload_size));
        return packets;
    }

    packets.push_back(makeTransportPacket(s_runtimeCore.gs_device_id,
                                          to_device_id,
                                          s_runtimeCore.next_tx_block_index,
                                          1,
                                          current_payload.data(),
                                          transport_payload_size));

    if (s_runtimeCore.tx_fec)
    {
        std::array<uint8_t, transport_payload_size> fec_payload = {};
        const gf* src_ptrs[2] = {
            s_runtimeCore.tx_first_packet_payload.data(),
            current_payload.data()
        };
        gf* fec_ptrs[1] = { fec_payload.data() };
        fec_encode(s_runtimeCore.tx_fec, src_ptrs, fec_ptrs, fec_block_nums() + 2, 1, transport_payload_size);

        packets.push_back(makeTransportPacket(s_runtimeCore.gs_device_id,
                                              to_device_id,
                                              s_runtimeCore.next_tx_block_index,
                                              2,
                                              fec_payload.data(),
                                              transport_payload_size));
    }

    s_runtimeCore.tx_block_has_first_packet = false;
    s_runtimeCore.next_tx_block_index++;
    return packets;
}

void syncRxDecoderStatsLocked(NativeHandle& handle)
{
    const FecBlockDecoder::Stats stats = s_runtimeCore.rx_decoder.getStats();
    s_runtimeCore.gs_stats.lastPacketIndex = stats.last_packet_index;
    s_runtimeCore.gs_stats.inUniquePacketCounter = static_cast<uint16_t>(stats.unique_packet_count);
    s_runtimeCore.gs_stats.inDublicatedPacketCounter = static_cast<uint16_t>(stats.duplicate_packet_count);
    s_runtimeCore.gs_stats.FECBlocksCounter = stats.fec_blocks_count;
    s_runtimeCore.gs_stats.FECSuccPacketIndexCounter = stats.fec_success_packet_index_sum;
}

void ensureRxDecoderConfigLocked(NativeHandle& handle)
{
    const uint8_t config_k = s_runtimeCore.config_packet.dataChannel.fec_codec_k;
    const uint8_t config_n = s_runtimeCore.config_packet.dataChannel.fec_codec_n;
    const uint16_t config_mtu = s_runtimeCore.config_packet.dataChannel.fec_codec_mtu;

    const uint8_t effective_k = config_k > 0 ? config_k : FEC_K;
    const uint8_t effective_n = config_n > 0 ? config_n : static_cast<uint8_t>(8);
    const uint16_t effective_mtu = config_mtu > 0 ? config_mtu : static_cast<uint16_t>(AIR2GROUND_MAX_MTU);

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
    s_runtimeCore.rx_decoder.init(decoder_descriptor);
    syncRxDecoderStatsLocked(handle);
}

void processDecodedTransportPacketsLocked(NativeHandle& handle)
{
    s_runtimeCore.rx_decoder.process(Clock::now());
    syncRxDecoderStatsLocked(handle);

    std::array<uint8_t, sizeof(Packet_Header) + AIR2GROUND_MAX_MTU> decoded_buffer = {};
    size_t decoded_size = 0;
    bool decoded_restored_by_fec = false;
    while (s_runtimeCore.rx_decoder.receive(decoded_buffer.data(), decoded_size, decoded_restored_by_fec))
    {
        s_runtimeCore.decoded_packets_seen++;
        if (decoded_restored_by_fec)
        {
            s_runtimeCore.restored_transport_packets++;
        }
        processSessionPacketLocked(handle, decoded_buffer.data(), decoded_size, decoded_restored_by_fec);
    }
}

//===================================================================================
//===================================================================================
// Feeds one already-decoded session packet through the shared Android runtime pipeline.
void processSessionPacketLocked(NativeHandle& handle,
                                const uint8_t* packet_data,
                                size_t packet_size,
                                bool restored_by_fec)
{
    static gs::core::SessionEventKind s_last_logged_event_kind = gs::core::SessionEventKind::Ignore;
    static uint16_t s_last_logged_connected_air = 0;
    static bool s_last_logged_got_config = false;

    if (packet_size >= sizeof(Air2Ground_Header))
    {
        const auto* header = reinterpret_cast<const Air2Ground_Header*>(packet_data);
        s_runtimeCore.last_decoded_type = static_cast<uint32_t>(header->type);
        s_runtimeCore.last_decoded_size = header->size;
        s_runtimeCore.last_decoded_air = header->airDeviceId;
        s_runtimeCore.last_decoded_gs = header->gsDeviceId;
    }

    ProcessedSessionPacket processed_packet = {};
    processed_packet.processed_tp = Clock::now();
    processed_packet.event =
        s_runtimeCore.session.processReceivedPacket(packet_data,
                                                   packet_size,
                                                   s_runtimeCore.gs_device_id,
                                                   processed_packet.processed_tp,
                                                   handle.transport_manager.activeTransport());
    const gs::core::SessionEvent& session_event = processed_packet.event;

    s_runtimeCore.last_event_kind = session_event.kind;
    s_runtimeCore.last_packet_tp = processed_packet.processed_tp;
    s_last_packet_tp = processed_packet.processed_tp;

    const uint16_t connected_air = s_runtimeCore.session.connectedAirDeviceId();
    const bool got_config = s_runtimeCore.session.gotConfigPacket();
    if (session_event.kind != s_last_logged_event_kind ||
        connected_air != s_last_logged_connected_air ||
        got_config != s_last_logged_got_config)
    {
        __android_log_print(ANDROID_LOG_INFO,
                            kNativeLogTag,
                            "Session event=%d connected_air=0x%04x got_config=%d decoded_type=%u decoded_size=%u restored=%d",
                            static_cast<int>(session_event.kind),
                            static_cast<unsigned int>(connected_air),
                            got_config ? 1 : 0,
                            static_cast<unsigned int>(s_runtimeCore.last_decoded_type),
                            static_cast<unsigned int>(s_runtimeCore.last_decoded_size),
                            restored_by_fec ? 1 : 0);
        s_last_logged_event_kind = session_event.kind;
        s_last_logged_connected_air = connected_air;
        s_last_logged_got_config = got_config;
    }

    const SessionEventPipelineDispatch dispatch = {
        {},
        {},
        {},
        {
            [&](const ProcessedVideoEvent& video_event, const VideoDispatchDecision& video_decision)
            {
                s_runtimeCore.last_video_frame_index = video_event.frame_index;
                s_runtimeCore.last_video_part_index = video_event.part_index;
                s_runtimeCore.last_video_last_part = video_event.last_part;
                s_runtimeCore.last_video_payload_size = static_cast<uint32_t>(video_event.payload_size);

                if (video_decision.restored_video_part)
                {
                    s_runtimeCore.restored_video_parts++;
                }

                const CompletedVideoFrameView& completed_frame = video_decision.completed_frame;
                if (completed_frame.has_frame)
                {
                    handle.jpeg_decoder.submitJpeg(completed_frame.frame_data,
                                                   completed_frame.frame_index);
                    if (g_gsUDPBroadcast && g_gsUDPBroadcast->isOpen())
                    {
                        g_gsUDPBroadcast->sendVideoFrame(completed_frame.data,
                                                         completed_frame.size);
                    }
                }
            },
        }
    };
    processAndDispatchSessionEvent(processed_packet,
                                   s_runtimeCore.assembler,
                                   s_runtimeCore.session,
                                   restored_by_fec,
                                   dispatch);
    if (s_runtimeCore.session.syncConfigPacket(s_runtimeCore.config_packet))
    {
        pendingOsdFontReload();
    }
    ensureRxDecoderConfigLocked(handle);
}

//===================================================================================
//===================================================================================
// Polls the active transport directly for session packets used by shared in-process transports.
void pollActiveTransportPacketsLocked(NativeHandle& handle)
{
    gs::core::ITransport& transport = handle.transport_manager.activeTransport();
    transport.process();

    std::array<uint8_t, AIR2GROUND_MAX_MTU> packet_buffer = {};
    while (true)
    {
        size_t packet_size = packet_buffer.size();
        bool restored_by_fec = false;
        if (!transport.receive(packet_buffer.data(), packet_size, restored_by_fec))
        {
            break;
        }

        processSessionPacketLocked(handle, packet_buffer.data(), packet_size, restored_by_fec);
    }
    transport.contributeGroundStats(s_runtimeCore.gs_stats);
}

//===================================================================================
//===================================================================================
// Periodically sends the shared connect/config control packet on in-process transports.
void pumpSharedControlPacketLocked(NativeHandle& handle, Clock::time_point now)
{
    static uint32_t s_control_send_count = 0;

    if (handle.transport_manager.activeKind() == gs::core::TransportKind::APFPV)
    {
        return;
    }

    if (now - handle.last_control_packet_tp < std::chrono::milliseconds(500))
    {
        return;
    }

    std::vector<uint8_t> control_payload;
    if (!tryBuildControlPacketPayload(s_runtimeCore.gs_device_id, control_payload))
    {
        handle.last_control_packet_tp = now;
        return;
    }

    handle.transport_manager.activeTransport().send(control_payload.data(), control_payload.size(), true);
    s_runtimeCore.session.addSentPackets(1);
    s_runtimeCore.session.onPingSent(now);
    handle.last_control_packet_tp = now;
    s_control_send_count++;
    if ((s_control_send_count % 4U) == 1U)
    {
        __android_log_print(ANDROID_LOG_INFO,
                            kNativeLogTag,
                            "Sent control packet count=%u size=%zu connected_air=0x%04x got_config=%d",
                            static_cast<unsigned int>(s_control_send_count),
                            control_payload.size(),
                            static_cast<unsigned int>(s_runtimeCore.session.connectedAirDeviceId()),
                            s_runtimeCore.session.gotConfigPacket() ? 1 : 0);
    }
}

//===================================================================================
//===================================================================================
// Starts or stops the APFPV UDP backend to match the currently active transport.
void syncApfpvUdpClient(NativeHandle& handle)
{
    bool should_stop = false;
    bool should_start = false;
    bool apfpv_active = false;
    bool udp_running = false;
    bool has_joinable_thread = false;

    {
        std::lock_guard<std::mutex> lock(handle.mutex);
        AndroidAPFPVTransport& apfpv_transport = handle.transport_manager.apfpvTransport();
        apfpv_active = handle.transport_manager.activeKind() == gs::core::TransportKind::APFPV;
        udp_running = apfpv_transport.isUdpRunning();
        has_joinable_thread = apfpv_transport.hasJoinableUdpThread();

        should_stop = (!apfpv_active && (udp_running || has_joinable_thread)) ||
                      (apfpv_active && !udp_running && has_joinable_thread);
        should_start = apfpv_active && !udp_running && !has_joinable_thread;
    }

    if (should_stop)
    {
        __android_log_print(ANDROID_LOG_INFO,
                            kNativeLogTag,
                            "syncApfpvUdpClient stop active=%d running=%d joinable=%d",
                            apfpv_active ? 1 : 0,
                            udp_running ? 1 : 0,
                            has_joinable_thread ? 1 : 0);
        handle.stopUdpClient();
        return;
    }

    if (!should_start)
    {
        return;
    }

    const bool started = handle.transport_manager.apfpvTransport().startUdpClient({
        [&handle]()
        {
            std::lock_guard<std::mutex> lock(handle.mutex);
            return buildControlTransportPacketsLocked(handle);
        },
        [&handle](const uint8_t* data, size_t size)
        {
            std::lock_guard<std::mutex> lock(handle.mutex);
            processTransportPacket(handle, data, size, false, 0);
        },
        [&handle]()
        {
            std::lock_guard<std::mutex> lock(handle.mutex);
            return handle.jpeg_decoder.submittedFrameCount();
        }
    });

    if (started)
    {
        __android_log_print(ANDROID_LOG_INFO,
                            kNativeLogTag,
                            "APFPV UDP backend auto-started active=%d running=%d joinable=%d",
                            apfpv_active ? 1 : 0,
                            udp_running ? 1 : 0,
                            has_joinable_thread ? 1 : 0);
    }
    else
    {
        __android_log_print(ANDROID_LOG_WARN,
                            kNativeLogTag,
                            "APFPV UDP backend auto-start failed active=%d running=%d joinable=%d",
                            apfpv_active ? 1 : 0,
                            udp_running ? 1 : 0,
                            has_joinable_thread ? 1 : 0);
    }
}

//===================================================================================
//===================================================================================
// Applies any deferred Android renderer invalidation requested by shared runtime code.
void applyPendingRendererInvalidation(NativeHandle& handle)
{
    if (!consumeAndroidRendererInvalidateRequest())
    {
        return;
    }

    handle.renderer.invalidateDisplayedFrame();
}

int processTransportPacket(NativeHandle& handle,
                           const uint8_t* data,
                           size_t size,
                           bool /* restored_by_fec */,
                           int input_dbm)
{
    static uint32_t s_transport_process_log_count = 0;

    if (data == nullptr || size == 0)
    {
        s_runtimeCore.last_event_kind = gs::core::SessionEventKind::Ignore;
        return static_cast<int>(s_runtimeCore.last_event_kind);
    }

    handle.transport_manager.apfpvTransport().setInputDbm(input_dbm);
    s_runtimeCore.last_event_kind = gs::core::SessionEventKind::Ignore;

    if (size >= sizeof(Packet_Header))
    {
        const auto* header = reinterpret_cast<const Packet_Header*>(data);
        s_runtimeCore.transport_packets_seen++;
        s_runtimeCore.gs_stats.inPacketCounterAll[0]++;
        s_runtimeCore.gs_stats.inPacketCounter[0]++;
        s_runtimeCore.last_transport_block = header->block_index;
        s_runtimeCore.last_transport_packet_index = header->packet_index;
        s_runtimeCore.last_transport_payload_size = header->size;
        s_runtimeCore.last_transport_from = header->fromDeviceId;
        s_runtimeCore.last_transport_to = header->toDeviceId;
    }

    s_runtimeCore.transport_packets_passed_filter++;
    s_runtimeCore.rx_decoder.pushPacket(data, size, 0, Clock::now());
    // Android APFPV uses a dedicated UDP RX thread. Drain the decoder and dispatch the
    // resulting session/video events here so completed frames reach the renderer queue
    // immediately instead of waiting for the next render/UI sync tick.
    processDecodedTransportPacketsLocked(handle);
    s_transport_process_log_count++;
    if ((s_transport_process_log_count % 200U) == 1U)
    {
        const FecBlockDecoder::Stats stats = s_runtimeCore.rx_decoder.getStats();
        __android_log_print(ANDROID_LOG_INFO,
                            kNativeLogTag,
                            "Transport packets=%u decoded=%u unique=%u duplicate=%u fec_blocks=%u runtime_last_block=%u last_packet=%u",
                            static_cast<unsigned int>(s_transport_process_log_count),
                            static_cast<unsigned int>(s_runtimeCore.decoded_packets_seen),
                            static_cast<unsigned int>(stats.unique_packet_count),
                            static_cast<unsigned int>(stats.duplicate_packet_count),
                            static_cast<unsigned int>(stats.fec_blocks_count),
                            static_cast<unsigned int>(s_runtimeCore.last_transport_block),
                            static_cast<unsigned int>(stats.last_packet_index));
    }
    return static_cast<int>(s_runtimeCore.last_event_kind);
}

std::vector<std::vector<uint8_t>> buildControlTransportPacketsLocked(NativeHandle& handle)
{
    static uint8_t s_last_logged_config_channel = 0;
    static bool s_last_logged_config_channel_valid = false;

    std::vector<uint8_t> payload;
    if (!tryBuildControlPacketPayload(s_runtimeCore.gs_device_id, payload))
    {
        return {};
    }

    if (payload.size() == sizeof(Ground2Air_Config_Packet))
    {
        const auto* config_packet = reinterpret_cast<const Ground2Air_Config_Packet*>(payload.data());
        const uint8_t channel = config_packet->dataChannel.wifi_channel;
        if (!s_last_logged_config_channel_valid || s_last_logged_config_channel != channel)
        {
            s_last_logged_config_channel = channel;
            s_last_logged_config_channel_valid = true;
            __android_log_print(ANDROID_LOG_INFO,
                                kNativeLogTag,
                                "APFPV control config channel=%u air=%u gs=%u apfpv=%d",
                                static_cast<unsigned int>(channel),
                                static_cast<unsigned int>(config_packet->airDeviceId),
                                static_cast<unsigned int>(config_packet->gsDeviceId),
                                static_cast<int>(config_packet->misc.apfpv));
        }
    }

    auto packets = buildTransportPacketsForControl(handle, payload.data(), payload.size());
    s_runtimeCore.session.addSentPackets(packets.size());
    return packets;
}

bool handleTapLocked(NativeHandle& handle,
                     float tap_x,
                     float tap_y,
                     float view_width,
                     float view_height)
{
    if (view_width <= 0.0f || view_height <= 0.0f)
    {
        return false;
    }

    if (!gs::menu::g_osdMenuController.isVisible())
    {
        __android_log_print(ANDROID_LOG_INFO,
                            kNativeLogTag,
                            "handleTapLocked menu not visible tap=(%.1f,%.1f) view=(%.1f,%.1f)",
                            tap_x,
                            tap_y,
                            view_width,
                            view_height);
        return false;
    }

    const GsVideoRenderer::MenuAction action = handle.renderer.dispatchTap(tap_x, tap_y);
    __android_log_print(ANDROID_LOG_INFO,
                        kNativeLogTag,
                        "handleTapLocked tap=(%.1f,%.1f) action=%d item=%d",
                        tap_x,
                        tap_y,
                        static_cast<int>(action.kind),
                        action.item_index);
    switch (action.kind)
    {
    case GsVideoRenderer::MenuActionKind::Up:
        handle.renderer.queueKeyPress(ImGuiKey_UpArrow);
        return false;
    case GsVideoRenderer::MenuActionKind::Down:
        handle.renderer.queueKeyPress(ImGuiKey_DownArrow);
        return false;
    case GsVideoRenderer::MenuActionKind::Back:
        handle.renderer.queueKeyPress(ImGuiKey_LeftArrow);
        return false;
    case GsVideoRenderer::MenuActionKind::Activate:
        handle.renderer.queueKeyPress(ImGuiKey_RightArrow);
        return false;
    case GsVideoRenderer::MenuActionKind::GsRec:
        if (s_recordingsStorage != nullptr)
        {
            s_recordingsStorage->toggleRecording(0, 0, "touch_btn");
        }
        return false;
    case GsVideoRenderer::MenuActionKind::AirRec:
        s_ground2air_config_packet.misc.air_record_btn++;
        return false;
    default:
        return true;
    }
}

bool handleKeyToImGui(bool menu_visible, int key_code, ImGuiKey* queued_key)
{
    constexpr int kKeycodeBack = 4;
    constexpr int kKeycodeDpadUp = 19;
    constexpr int kKeycodeDpadDown = 20;
    constexpr int kKeycodeDpadLeft = 21;
    constexpr int kKeycodeDpadRight = 22;
    constexpr int kKeycodeDpadCenter = 23;
    constexpr int kKeycodeEnter = 66;
    constexpr int kKeycodeMenu = 82;

    if (!menu_visible)
    {
        return key_code == kKeycodeMenu || key_code == kKeycodeDpadCenter || key_code == kKeycodeEnter;
    }

    switch (key_code)
    {
    case kKeycodeDpadUp:
        if (queued_key != nullptr) *queued_key = ImGuiKey_UpArrow;
        return true;
    case kKeycodeDpadDown:
        if (queued_key != nullptr) *queued_key = ImGuiKey_DownArrow;
        return true;
    case kKeycodeDpadLeft:
    case kKeycodeBack:
        if (queued_key != nullptr) *queued_key = ImGuiKey_LeftArrow;
        return true;
    case kKeycodeDpadRight:
    case kKeycodeDpadCenter:
    case kKeycodeEnter:
        if (queued_key != nullptr) *queued_key = ImGuiKey_RightArrow;
        return true;
    case kKeycodeMenu:
        return true;
    default:
        return false;
    }
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_getBuildInfo(JNIEnv* env, jobject /* thiz */)
{
    return newJavaString(env, buildInfoString());
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_setAssetManager(JNIEnv* env,
                                                          jobject /* thiz */,
                                                          jobject asset_manager)
{
    androidSetAssetManager(asset_manager == nullptr ? nullptr : AAssetManager_fromJava(env, asset_manager));
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_setSettingsPath(JNIEnv* env,
                                                          jobject /* thiz */,
                                                          jstring path)
{
    const std::string settings_path = fromJString(env, path);
    if (settings_path.empty())
    {
        return;
    }

    s_settingsStorage.setPath(settings_path);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_createHandle(JNIEnv* /* env */,
                                                       jobject /* thiz */,
                                                       jint gsDeviceId)
{
    auto* handle = new NativeHandle(static_cast<uint16_t>(gsDeviceId));
    handle->transport_manager.rawBroadcastTransport().setTransportPacketCallback(
        [handle](const uint8_t* data, size_t size, int input_dbm)
        {
            if (handle == nullptr || data == nullptr || size == 0)
            {
                return;
            }

            std::lock_guard<std::mutex> lock(handle->mutex);
            processTransportPacket(*handle, data, size, false, input_dbm);
        });
    handle->background_runtime_thread = std::make_unique<std::thread>(
        [handle]()
        {
            while (!handle->stop_background.load())
            {
                {
                    std::lock_guard<std::mutex> lock(handle->mutex);
                    if (handle->transport_manager.activeKind() == gs::core::TransportKind::RawBroadcast &&
                        handle->transport_manager.rawBroadcastTransport().isUsbAdapterRunning())
                    {
                        pumpSharedControlPacketLocked(*handle, Clock::now());
                        processPendingRawBroadcastChannelChange(
                            handle->transport_manager.rawBroadcastTransport());
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        });
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
Java_com_esp32camfpv_androidgs_NativeCore_getActiveTransportKind(JNIEnv* /* env */,
                                                                 jobject /* thiz */,
                                                                 jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return static_cast<jint>(gs::core::TransportKind::RawBroadcast);
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return static_cast<jint>(native_handle->transport_manager.activeKind());
}

//===================================================================================
//===================================================================================
// Returns the persisted APFPV preferred camera id used by shared camera selection logic.
extern "C" JNIEXPORT jint JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_getPreferredApfpvCameraId(JNIEnv* /* env */,
                                                                    jobject /* thiz */,
                                                                    jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return static_cast<jint>(s_groundstation_config.apfpvPreferredCameraId);
}

//===================================================================================
//===================================================================================
// Stores the selected APFPV preferred camera id used by shared menu and reconnect logic.
extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_setPreferredApfpvCameraId(JNIEnv* /* env */,
                                                                    jobject /* thiz */,
                                                                    jlong handle,
                                                                    jint device_id)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    s_groundstation_config.apfpvPreferredCameraId = static_cast<uint16_t>(device_id);
    setApfpvPreferredCameraId(s_groundstation_config.apfpvPreferredCameraId);
    s_settingsStorage.saveGroundStationConfig();
}

//===================================================================================
//===================================================================================
// Returns whether the shared Android APFPV menu search flow is currently active.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_isApfpvMenuSearchActive(JNIEnv* /* env */,
                                                                  jobject /* thiz */,
                                                                  jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return JNI_FALSE;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return native_handle->transport_manager.apfpvTransport().isMenuSearchActive() ? JNI_TRUE : JNI_FALSE;
}

//===================================================================================
//===================================================================================
// Returns and clears one queued Android APFPV reconnect request for the Wi-Fi controller.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_consumeApfpvReconnectRequest(JNIEnv* /* env */,
                                                                       jobject /* thiz */,
                                                                       jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return JNI_FALSE;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return native_handle->transport_manager.apfpvTransport().consumeReconnectRequest() ? JNI_TRUE : JNI_FALSE;
}

//===================================================================================
//===================================================================================
// Returns whether Android APFPV has already received any UDP packets from the camera.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_hasSeenApfpvUdpPackets(JNIEnv* /* env */,
                                                                 jobject /* thiz */,
                                                                 jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return JNI_FALSE;
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    return native_handle->transport_manager.apfpvTransport().hasSeenUdpPackets() ? JNI_TRUE : JNI_FALSE;
}

//===================================================================================
//===================================================================================
// Synchronizes discovered and active APFPV camera SSIDs from the Android Wi-Fi controller.
extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_syncApfpvCameraState(JNIEnv* env,
                                                               jobject /* thiz */,
                                                               jlong handle,
                                                               jobjectArray discovered_ssids,
                                                               jstring active_ssid,
                                                               jint gs_rssi_dbm,
                                                               jstring connecting_ssid)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    std::vector<ApfpvCameraDescriptor> discovered_cameras;
    if (discovered_ssids != nullptr)
    {
        const jsize count = env->GetArrayLength(discovered_ssids);
        for (jsize i = 0; i < count; ++i)
        {
            auto* ssid_value = static_cast<jstring>(env->GetObjectArrayElement(discovered_ssids, i));
            const std::string ssid = fromJString(env, ssid_value);
            if (ssid_value != nullptr)
            {
                env->DeleteLocalRef(ssid_value);
            }

            const uint16_t device_id = parseApfpvCameraIdFromSsid(ssid);
            if (device_id == 0)
            {
                continue;
            }
            discovered_cameras.push_back({device_id, ssid});
        }
    }

    std::lock_guard<std::mutex> lock(native_handle->mutex);
    updateApfpvDiscoveredCameras(discovered_cameras);

    const std::string active_ssid_value = fromJString(env, active_ssid);
    if (!active_ssid_value.empty())
    {
        setApfpvActiveCamera(active_ssid_value);
        s_runtimeCore.gs_stats.rssiDbm[0] = static_cast<int8_t>(std::clamp(static_cast<int>(gs_rssi_dbm), -127, 0));
    }
    else
    {
        clearApfpvActiveCamera();
        s_runtimeCore.gs_stats.rssiDbm[0] = 0;
    }
    s_runtimeCore.gs_stats.rssiDbm[1] = 0;
    s_runtimeCore.gs_stats.noiseFloorDbm = 0;

    const std::string connecting_ssid_value = fromJString(env, connecting_ssid);
    native_handle->transport_manager.apfpvTransport().syncCameraState(
        discovered_cameras.size(),
        !active_ssid_value.empty(),
        connecting_ssid_value);
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
        s_runtimeCore.last_event_kind = gs::core::SessionEventKind::Ignore;
        return static_cast<jint>(s_runtimeCore.last_event_kind);
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
    if (s_runtimeCore.last_completed_frame.empty())
    {
        return nullptr;
    }

    std::vector<uint8_t> frame = std::move(s_runtimeCore.last_completed_frame);
    s_runtimeCore.last_completed_frame.clear();
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
        if (native_handle->transport_manager.activeKind() != gs::core::TransportKind::APFPV)
        {
            return JNI_FALSE;
        }
        if (native_handle->transport_manager.apfpvTransport().isUdpRunning())
        {
            return JNI_FALSE;
        }
        join_stale_thread = native_handle->transport_manager.apfpvTransport().hasJoinableUdpThread();
    }

    if (join_stale_thread)
    {
        native_handle->stopUdpClient();
    }

    {
        std::lock_guard<std::mutex> lock(native_handle->mutex);
        AndroidAPFPVTransport& apfpv_transport = native_handle->transport_manager.apfpvTransport();
        if (apfpv_transport.isUdpRunning() || apfpv_transport.hasJoinableUdpThread())
        {
            return JNI_FALSE;
        }
        apfpv_transport.configureUdpEndpoint(peer, static_cast<int>(peer_port), static_cast<int>(local_port));
        apfpv_transport.clearUdpError();
    }

    const bool started = native_handle->transport_manager.apfpvTransport().startUdpClient({
        [native_handle]()
        {
            std::lock_guard<std::mutex> lock(native_handle->mutex);
            return buildControlTransportPacketsLocked(*native_handle);
        },
        [native_handle](const uint8_t* data, size_t size)
        {
            std::lock_guard<std::mutex> lock(native_handle->mutex);
            processTransportPacket(*native_handle, data, size, false, 0);
        },
        [native_handle]()
        {
            std::lock_guard<std::mutex> lock(native_handle->mutex);
            return native_handle->jpeg_decoder.submittedFrameCount();
        }
    });
    if (!started)
    {
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO,
                        kNativeLogTag,
                        "startUdpClient requested peer=%s:%d local=%d",
                        peer.c_str(),
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
Java_com_esp32camfpv_androidgs_NativeCore_startRawBroadcastUsb(JNIEnv* /* env */,
                                                               jobject /* thiz */,
                                                               jlong handle,
                                                               jint fd)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return JNI_FALSE;
    }

    bool should_start = false;
    {
        std::lock_guard<std::mutex> lock(native_handle->mutex);
        should_start =
            native_handle->transport_manager.activeKind() == gs::core::TransportKind::RawBroadcast;
    }

    if (!should_start)
    {
        return JNI_FALSE;
    }

    return native_handle->transport_manager.rawBroadcastTransport().startUsbAdapter(fd) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_stopRawBroadcastUsb(JNIEnv* /* env */,
                                                              jobject /* thiz */,
                                                              jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    native_handle->transport_manager.rawBroadcastTransport().stopUsbAdapter();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_isRawBroadcastUsbRunning(JNIEnv* /* env */,
                                                                   jobject /* thiz */,
                                                                   jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return JNI_FALSE;
    }

    return native_handle->transport_manager.rawBroadcastTransport().isUsbAdapterRunning() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_startWifiScanUsb(JNIEnv* /* env */,
                                                           jobject /* thiz */,
                                                           jlong handle,
                                                           jint fd)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return JNI_FALSE;
    }

    bool should_start = false;
    {
        std::lock_guard<std::mutex> lock(native_handle->mutex);
        should_start =
            native_handle->transport_manager.activeKind() == gs::core::TransportKind::WifiChannelScan;
    }

    if (!should_start)
    {
        return JNI_FALSE;
    }

    return native_handle->transport_manager.wifiScanTransport().startUsbAdapter(fd) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_stopWifiScanUsb(JNIEnv* /* env */,
                                                          jobject /* thiz */,
                                                          jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    native_handle->transport_manager.wifiScanTransport().stopUsbAdapter();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_isWifiScanUsbRunning(JNIEnv* /* env */,
                                                               jobject /* thiz */,
                                                               jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return JNI_FALSE;
    }

    return native_handle->transport_manager.wifiScanTransport().isUsbAdapterRunning() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_setVideoUdpOutput(JNIEnv* env,
                                                            jobject /* thiz */,
                                                            jlong /* handle */,
                                                            jstring addr,
                                                            jint port)
{
    const std::string addr_str = fromJString(env, addr);
    if (addr_str.empty() || port <= 0)
    {
        return JNI_FALSE;
    }

    const bool ok = g_gsUDPBroadcast->init(addr_str, static_cast<int>(port));
    __android_log_print(ANDROID_LOG_INFO,
                        kNativeLogTag,
                        "setVideoUdpOutput addr=%s port=%d ok=%d",
                        addr_str.c_str(),
                        static_cast<int>(port),
                        ok ? 1 : 0);
    return ok ? JNI_TRUE : JNI_FALSE;
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
    return native_handle->transport_manager.apfpvTransport().isUdpRunning() ? JNI_TRUE : JNI_FALSE;
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
    return static_cast<jint>(s_runtimeCore.last_event_kind);
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
    return s_runtimeCore.groundstation_config.screenAspectRatio == ScreenAspectRatio::STRETCH ? 0 : 1;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_isVrModeEnabled(JNIEnv* /* env */,
                                                          jobject /* thiz */,
                                                          jlong handle)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return JNI_FALSE;
    }

    return s_runtimeCore.groundstation_config.vrMode ? JNI_TRUE : JNI_FALSE;
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
Java_com_esp32camfpv_androidgs_NativeCore_setRendererVrMode(JNIEnv* /* env */,
                                                            jobject /* thiz */,
                                                            jlong handle,
                                                            jboolean enabled)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    native_handle->renderer.setVrMode(enabled == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_syncRendererOverlay(JNIEnv* env,
                                                              jobject /* thiz */,
                                                              jlong handle,
                                                              jstring build_info)
{
    static uint32_t s_sync_overlay_call_count = 0;

    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    const std::string info = fromJString(env, build_info);
    RuntimeSyncState sync_state;
    gs::imgui::TopOverlayData overlay_input = {};
    s_sync_overlay_call_count++;
    if ((s_sync_overlay_call_count % 60U) == 1U)
    {
        __android_log_print(ANDROID_LOG_INFO,
                            kNativeLogTag,
                            "syncRendererOverlay calls=%u transport=%d raw_usb=%d",
                            static_cast<unsigned int>(s_sync_overlay_call_count),
                            static_cast<int>(native_handle->transport_manager.activeKind()),
                            native_handle->transport_manager.rawBroadcastTransport().isUsbAdapterRunning() ? 1 : 0);
    }
    syncApfpvUdpClient(*native_handle);
    applyPendingRendererInvalidation(*native_handle);
    {
        std::lock_guard<std::mutex> lock(native_handle->mutex);
        pumpSharedControlPacketLocked(*native_handle, Clock::now());
        if (native_handle->transport_manager.activeKind() != gs::core::TransportKind::APFPV)
        {
            pollActiveTransportPacketsLocked(*native_handle);
        }
        const AndroidBitmapJpegDecoder::DecodeStats decode_stats = native_handle->jpeg_decoder.consumeStats();
        RuntimeSyncParams sync_params = {};
        sync_params.decode_stats.input_submitted_count = decode_stats.input_submitted_count;
        sync_params.decode_stats.decoded_count = decode_stats.decoded_count;
        sync_params.decode_stats.overwritten_pending_count = decode_stats.overwritten_pending_count;
        sync_params.decode_stats.broken_frames = decode_stats.broken_frames;
        sync_params.decode_stats.decoded_total_ms = decode_stats.decoded_total_ms;
        sync_params.decode_stats.decoded_min_ms = decode_stats.decoded_min_ms;
        sync_params.decode_stats.decoded_max_ms = decode_stats.decoded_max_ms;
        const GsVideoRenderer::RendererStats renderer_stats = native_handle->renderer.consumeStats();
        sync_params.renderer_stats.upload_count = renderer_stats.upload_count;
        sync_params.renderer_stats.upload_total_ms = renderer_stats.upload_total_ms;
        sync_params.renderer_stats.upload_min_ms = renderer_stats.upload_min_ms;
        sync_params.renderer_stats.upload_max_ms = renderer_stats.upload_max_ms;
        sync_params.renderer_stats.swap_count = renderer_stats.swap_count;
        sync_params.renderer_stats.swap_total_ms = renderer_stats.swap_total_ms;
        sync_params.renderer_stats.swap_min_ms = renderer_stats.swap_min_ms;
        sync_params.renderer_stats.swap_max_ms = renderer_stats.swap_max_ms;
        sync_params.renderer_stats.discarded_pending_count = renderer_stats.discarded_pending_count;
        sync_params.build_info = info;
        sync_params.osd_font_name = s_flightOSD.currentFontName();
        const AndroidAPFPVTransport& apfpv_transport = native_handle->transport_manager.apfpvTransport();
        if (native_handle->transport_manager.activeKind() == gs::core::TransportKind::APFPV)
        {
            s_runtimeCore.session.setCompletedFrameCounts(
                apfpv_transport.receivedCompletedFrames(),
                apfpv_transport.restoredCompletedFrames());
        }
        sync_params.is_dual = false;
        sync_params.osd_font_error = s_flightOSD.isFontError();
        sync_state = collectRuntimeSyncState(s_runtimeCore, sync_params, overlay_input);
        processPendingSelectedTransportReconnect();
    }
    overlay_input.transport_message =
        native_handle->transport_manager.activeTransport().getTransportMessage();
    overlay_input.gs_thermal_status = native_handle->gs_thermal_status;
    overlay_input.battery_percent = native_handle->gs_battery_percent;
    native_handle->renderer.setFlightOsdFont(sync_state.osd_font_name);
    processPendingOsdFontReload(s_runtimeCore.config_packet);
    native_handle->renderer.setOverlayInput(overlay_input);
    native_handle->renderer.setFrameUiState(sync_state.frame_ui_state);
    native_handle->renderer.setOverlayStatsSnapshot(sync_state.overlay_stats_snapshot);
}

//===================================================================================
//===================================================================================
// Stores the latest Android thermal status enum for overlay rendering and legacy stats.
extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_setThermalStatus(JNIEnv* /* env */,
                                                           jobject /* thiz */,
                                                           jlong handle,
                                                           jint thermal_status)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    native_handle->gs_thermal_status = std::max(0, static_cast<int>(thermal_status));
    setAndroidThermalStatus(native_handle->gs_thermal_status);
}

//===================================================================================
//===================================================================================
// Stores the latest Android battery level for overlay rendering.
extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_setBatteryPercent(JNIEnv* /* env */,
                                                            jobject /* thiz */,
                                                            jlong handle,
                                                            jint battery_percent)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return;
    }

    native_handle->gs_battery_percent = battery_percent;
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

    const float tap_x = static_cast<float>(x);
    const float tap_y = static_cast<float>(y);
    if (static_cast<float>(view_width) <= 0.0f || static_cast<float>(view_height) <= 0.0f)
    {
        return;
    }
    if (!native_handle->renderer.isMenuVisible())
    {
        native_handle->renderer.queueMenuOpen();
        return;
    }
    if (handleTapLocked(*native_handle,
                        tap_x,
                        tap_y,
                        static_cast<float>(view_width),
                        static_cast<float>(view_height)))
    {
        native_handle->renderer.queueMouseTap(tap_x, tap_y);
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_handleKey(JNIEnv* /* env */,
                                                    jobject /* thiz */,
                                                    jlong handle,
                                                    jint key_code)
{
    NativeHandle* native_handle = fromJLong(handle);
    if (native_handle == nullptr)
    {
        return JNI_FALSE;
    }

    const bool menu_visible = native_handle->renderer.isMenuVisible();
    ImGuiKey queued_key = ImGuiKey_None;
    const bool handled = handleKeyToImGui(menu_visible, static_cast<int>(key_code), &queued_key);
    if (!handled)
    {
        return JNI_FALSE;
    }
    if (!menu_visible)
    {
        native_handle->renderer.queueMenuOpen();
        return JNI_TRUE;
    }
    if (static_cast<int>(key_code) == 82)
    {
        native_handle->renderer.queueMenuClose();
        return JNI_TRUE;
    }
    if (queued_key != ImGuiKey_None)
    {
        native_handle->renderer.queueKeyPress(queued_key);
    }
    return JNI_TRUE;
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
        static_cast<int>(stride),
        0);
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
    const bool requested = s_runtimeCore.exit_requested;
    s_runtimeCore.exit_requested = false;
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
    s_runtimeCore.resetTransportRuntime(native_handle->transport_manager.activeTransport(), Clock::now());
    native_handle->last_control_packet_tp = Clock::now();
    gs::menu::g_osdMenuController.close();
}


extern "C" JNIEXPORT void JNICALL
Java_com_esp32camfpv_androidgs_NativeCore_destroyHandle(JNIEnv* /* env */,
                                                        jobject /* thiz */,
                                                        jlong handle)
{
    delete fromJLong(handle);
}
