#include "gs_test_transport.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include "android_jni_shared.h"
#endif

#include "Log.h"
#include "crc.h"
#include "packets.h"
#include "frame_packets_debug.h"
#include "gs_shared_state.h"

namespace
{

constexpr uint16_t kTestAirDeviceId = 0x7777;
constexpr auto kTargetFramePeriod = std::chrono::microseconds(33333);

//===================================================================================
//===================================================================================
// Builds decoder callbacks that feed the shared frame-packet debug overlay.
FecBlockDecoder::Callbacks makeTestDecoderCallbacks()
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
    return callbacks;
}

//===================================================================================
//===================================================================================
// Computes the CRC for a fixed-size air packet header before transport delivery.
template <typename T>
void sealFixedHeaderPacket(T& packet)
{
    packet.crc = 0;
    packet.crc = crc8(0, &packet, sizeof(T));
}

//===================================================================================
//===================================================================================
// Loads a binary file from disk into memory for Linux-side shared test transport assets.
bool loadBinaryFile(const char* path, std::vector<uint8_t>& bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return false;
    }

    bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !bytes.empty();
}

} // namespace

//===================================================================================
//===================================================================================
// Resets the paced static-JPEG stream state so the transport can reconnect cleanly.
void GSTestTransport::resetStreamState()
{
    m_pending_packets.clear();
    m_next_frame_tp = Clock::time_point::min();
    m_next_packet_tp = Clock::time_point::min();
    m_frame_period = kTargetFramePeriod;
    m_packet_period = kTargetFramePeriod;
    m_next_frame_index = 1;
    m_data_rate = 0;
    m_config_pending = true;
    m_next_transport_block_index = 1;
    m_rx_decoder.reset(Clock::now());
}

//===================================================================================
//===================================================================================
// Releases the internal FEC codec used by the test transport loopback.
GSTestTransport::~GSTestTransport()
{
    if (m_tx_fec != nullptr)
    {
        fec_free(m_tx_fec);
        m_tx_fec = nullptr;
    }
}

//===================================================================================
//===================================================================================
// Initializes the internal FEC loopback codec and decoder used by the test transport.
bool GSTestTransport::initLoopbackCodec()
{
    if (m_tx_fec != nullptr)
    {
        fec_free(m_tx_fec);
        m_tx_fec = nullptr;
    }

    const uint8_t requested_k = static_cast<uint8_t>(
        std::clamp<uint32_t>(m_rx_descriptor.coding_k > 0 ? m_rx_descriptor.coding_k : FEC_K, 1u, 32u));
    const uint8_t requested_n = static_cast<uint8_t>(
        std::clamp<uint32_t>(m_rx_descriptor.coding_n >= requested_k ? m_rx_descriptor.coding_n : FEC_N,
                             requested_k,
                             32u));
    const uint16_t requested_mtu =
        m_rx_descriptor.mtu > 0 ? static_cast<uint16_t>(std::min<size_t>(m_rx_descriptor.mtu, AIR2GROUND_MAX_MTU))
                                : static_cast<uint16_t>(AIR2GROUND_MAX_MTU);

    m_effective_coding_k = requested_k;
    m_effective_coding_n = requested_n;
    m_transport_payload_size = requested_mtu;

    m_tx_fec = fec_new(m_effective_coding_k, m_effective_coding_n);
    if (m_tx_fec == nullptr)
    {
        LOGE("Test transport failed to allocate FEC {} / {}", m_effective_coding_k, m_effective_coding_n);
        return false;
    }

    FecBlockDecoder::Descriptor decoder_descriptor = {};
    decoder_descriptor.coding_k = m_effective_coding_k;
    decoder_descriptor.coding_n = m_effective_coding_n;
    decoder_descriptor.mtu = m_transport_payload_size;
    decoder_descriptor.reset_duration = std::chrono::milliseconds(0);
    decoder_descriptor.restart_backjump_blocks = 64;
    decoder_descriptor.max_block_queue_size = 3;
    decoder_descriptor.duplicate_window = 100;
    decoder_descriptor.interface_count = 1;
    if (!m_rx_decoder.init(decoder_descriptor))
    {
        LOGE("Test transport failed to initialize decoder {} / {} mtu={}",
             m_effective_coding_k,
             m_effective_coding_n,
             static_cast<unsigned int>(m_transport_payload_size));
        return false;
    }

    m_rx_decoder.setCallbacks(makeTestDecoderCallbacks());
    return true;
}

//===================================================================================
//===================================================================================
// Loads the shared nosignal JPEG and prepares the paced test-stream state.
bool GSTestTransport::init(const gs::core::RXDescriptor& rx_descriptor,
                           const gs::core::TXDescriptor& tx_descriptor)
{
    storeDescriptors(rx_descriptor, tx_descriptor);

    if (!initLoopbackCodec())
    {
        return false;
    }

    resetStreamState();

    if (!loadStaticJpeg())
    {
        LOGE("Test transport failed to load assets_gs/nosignal.jpg");
        return false;
    }

    LOGI("Test transport initialized with paced 30 FPS static JPEG stream and FEC {} / {}",
         m_effective_coding_k,
         m_effective_coding_n);
    return true;
}

//===================================================================================
//===================================================================================
// Resets the test stream so switching back to Test transport restarts config and frame output.
void GSTestTransport::reset_rx_state()
{
    resetStreamState();
}

//===================================================================================
//===================================================================================
// Encodes queued session packets into one or more transport FEC blocks and loops them into the decoder.
void GSTestTransport::encodePendingPackets(Clock::time_point now)
{
    if (m_tx_fec == nullptr)
    {
        return;
    }

    const size_t fec_count = static_cast<size_t>(m_effective_coding_n - m_effective_coding_k);
    while (m_pending_packets.size() >= m_effective_coding_k)
    {
        std::vector<std::array<uint8_t, sizeof(Packet_Header) + AIR2GROUND_MAX_MTU>> primary_packets(
            m_effective_coding_k);
        std::vector<std::array<uint8_t, sizeof(Packet_Header) + AIR2GROUND_MAX_MTU>> fec_packets(fec_count);
        std::vector<const gf*> src_ptrs(m_effective_coding_k, nullptr);
        std::vector<gf*> dst_ptrs(fec_count, nullptr);

        for (uint8_t packet_index = 0; packet_index < m_effective_coding_k; ++packet_index)
        {
            const std::vector<uint8_t>& session_packet = m_pending_packets[packet_index];
            auto& transport_packet = primary_packets[packet_index];
            transport_packet.fill(0);

            auto* header = reinterpret_cast<Packet_Header*>(transport_packet.data());
            header->packet_version = PACKET_VERSION;
            header->packet_signature = PACKET_SIGNATURE;
            header->fromDeviceId = kTestAirDeviceId;
            header->toDeviceId = currentGsDeviceId();
            header->size = m_transport_payload_size;
            header->block_index = m_next_transport_block_index;
            header->packet_index = packet_index;

            const size_t copy_size = std::min(session_packet.size(), static_cast<size_t>(m_transport_payload_size));
            std::memcpy(transport_packet.data() + sizeof(Packet_Header), session_packet.data(), copy_size);
            src_ptrs[packet_index] = transport_packet.data() + sizeof(Packet_Header);
        }

        for (size_t fec_index = 0; fec_index < fec_count; ++fec_index)
        {
            fec_packets[fec_index].fill(0);

            auto* header = reinterpret_cast<Packet_Header*>(fec_packets[fec_index].data());
            header->packet_version = PACKET_VERSION;
            header->packet_signature = PACKET_SIGNATURE;
            header->fromDeviceId = kTestAirDeviceId;
            header->toDeviceId = currentGsDeviceId();
            header->size = m_transport_payload_size;
            header->block_index = m_next_transport_block_index;
            header->packet_index = static_cast<uint8_t>(m_effective_coding_k + fec_index);
            dst_ptrs[fec_index] = fec_packets[fec_index].data() + sizeof(Packet_Header);
        }

        fec_encode(m_tx_fec,
                   src_ptrs.data(),
                   dst_ptrs.data(),
                   fec_block_nums() + m_effective_coding_k,
                   fec_count,
                   m_transport_payload_size);

        for (uint8_t packet_index = 0; packet_index < m_effective_coding_k; ++packet_index)
        {
            m_rx_decoder.pushPacket(primary_packets[packet_index].data(),
                                    sizeof(Packet_Header) + m_transport_payload_size,
                                    0,
                                    now);
        }

        for (size_t fec_index = 0; fec_index < fec_count; ++fec_index)
        {
            m_rx_decoder.pushPacket(fec_packets[fec_index].data(),
                                    sizeof(Packet_Header) + m_transport_payload_size,
                                    0,
                                    now);
        }

        for (uint8_t packet_index = 0; packet_index < m_effective_coding_k; ++packet_index)
        {
            m_pending_packets.pop_front();
        }

        m_next_transport_block_index++;
    }
}

//===================================================================================
//===================================================================================
// Advances internal pacing state for the shared 30 FPS static-JPEG stream.
void GSTestTransport::process()
{
    const Clock::time_point now = Clock::now();
    scheduleDuePackets(now);
    encodePendingPackets(now);
    m_rx_decoder.process(now);
}

//===================================================================================
//===================================================================================
// Drops outgoing payloads because the shared test transport is receive-driven.
void GSTestTransport::send(const void* /* data */, size_t /* size */, bool /* flush */)
{
}

//===================================================================================
//===================================================================================
// Returns the next decoded config or video packet from the paced static-JPEG transport loopback.
bool GSTestTransport::receive(void* data, size_t& size, bool& restoredByFEC)
{
    if (data == nullptr)
    {
        return false;
    }

    process();
    return m_rx_decoder.receive(data, size, restoredByFEC);
}

//===================================================================================
//===================================================================================
// Returns the approximate transport data rate generated by the paced static-JPEG stream.
size_t GSTestTransport::get_data_rate() const
{
    return m_data_rate;
}

//===================================================================================
//===================================================================================
// Loads the shared nosignal JPEG bytes from APK assets on Android or assets_gs on Linux.
bool GSTestTransport::loadStaticJpeg()
{
    if (!m_static_jpeg.empty())
    {
        return true;
    }

#ifdef __ANDROID__
    AAssetManager* asset_manager = androidGetAssetManager();
    if (asset_manager == nullptr)
    {
        return false;
    }

    AAsset* asset = AAssetManager_open(asset_manager, "nosignal.jpg", AASSET_MODE_BUFFER);
    if (asset == nullptr)
    {
        return false;
    }

    const off_t asset_size = AAsset_getLength(asset);
    if (asset_size <= 0)
    {
        AAsset_close(asset);
        return false;
    }

    m_static_jpeg.resize(static_cast<size_t>(asset_size));
    const int read_size = AAsset_read(asset, m_static_jpeg.data(), m_static_jpeg.size());
    AAsset_close(asset);
    return read_size == static_cast<int>(m_static_jpeg.size());
#else
    static constexpr const char* kCandidatePaths[] = {
        "../assets_gs/nosignal.jpg",
        "assets_gs/nosignal.jpg",
        "./assets_gs/nosignal.jpg"
    };

    for (const char* path : kCandidatePaths)
    {
        if (loadBinaryFile(path, m_static_jpeg))
        {
            return true;
        }
    }

    return false;
#endif
}

//===================================================================================
//===================================================================================
// Builds and queues the first connect-config packet for immediate session establishment.
void GSTestTransport::queueConnectConfigPacket()
{
    if (!m_config_pending)
    {
        return;
    }

    Air2Ground_Config_Packet config_packet = {};
    config_packet.type = Air2Ground_Header::Type::Config;
    config_packet.size = sizeof(config_packet);
    config_packet.pong = 0;
    config_packet.version = PACKET_VERSION;
    config_packet.airDeviceId = kTestAirDeviceId;
    config_packet.gsDeviceId = currentGsDeviceId();
    config_packet.camera.resolution = Resolution::HD;
    config_packet.dataChannel.fec_codec_mtu =
        static_cast<uint16_t>(std::min<size_t>(std::numeric_limits<uint16_t>::max(), m_rx_descriptor.mtu));
    config_packet.misc.apfpv = 0;
    sealFixedHeaderPacket(config_packet);

    std::vector<uint8_t> raw_packet(sizeof(config_packet));
    std::memcpy(raw_packet.data(), &config_packet, sizeof(config_packet));
    m_pending_packets.push_back(std::move(raw_packet));
    m_config_pending = false;
    LOGI("Test transport queued config air={} gs={} mtu={}",
         static_cast<unsigned int>(kTestAirDeviceId),
         static_cast<unsigned int>(config_packet.gsDeviceId),
         static_cast<unsigned int>(config_packet.dataChannel.fec_codec_mtu));
}

//===================================================================================
//===================================================================================
// Schedules transport packets for the next frame when the next 30 FPS slot is due.
void GSTestTransport::scheduleDuePackets(Clock::time_point now)
{
    if (m_config_pending)
    {
        queueConnectConfigPacket();
    }

    if (m_next_frame_tp == Clock::time_point::min())
    {
        m_next_frame_tp = now;
    }

    if (now >= m_next_frame_tp && m_pending_packets.size() < m_effective_coding_k)
    {
        std::vector<std::vector<uint8_t>> frame_packets = buildFramePackets(m_next_frame_index++);
        if (!frame_packets.empty())
        {
            size_t total_bytes = 0;
            for (auto& packet : frame_packets)
            {
                total_bytes += packet.size();
                m_pending_packets.push_back(std::move(packet));
            }
            m_data_rate = total_bytes * 30;

            const size_t packet_count = std::max<size_t>(1, m_pending_packets.size());
            m_packet_period = kTargetFramePeriod / packet_count;
            if (m_packet_period <= Clock::duration::zero())
            {
                m_packet_period = std::chrono::microseconds(1);
            }
        }

        m_next_frame_tp = now + kTargetFramePeriod;
        m_next_packet_tp = now;
    }
}

//===================================================================================
//===================================================================================
// Builds the raw transport packets that represent a single static JPEG frame.
std::vector<std::vector<uint8_t>> GSTestTransport::buildFramePackets(uint32_t frame_index) const
{
    std::vector<std::vector<uint8_t>> packets;
    if (m_static_jpeg.empty())
    {
        return packets;
    }

    const size_t payload_size = maxVideoPayloadSize();
    if (payload_size == 0)
    {
        return packets;
    }

    const size_t packet_count = (m_static_jpeg.size() + payload_size - 1) / payload_size;
    packets.reserve(packet_count);

    size_t offset = 0;
    for (size_t packet_index = 0; packet_index < packet_count; ++packet_index)
    {
        const size_t chunk_size = std::min(payload_size, m_static_jpeg.size() - offset);
        Air2Ground_Video_Packet video_packet = {};
        video_packet.type = Air2Ground_Header::Type::Video;
        video_packet.size = static_cast<uint32_t>(sizeof(video_packet) + chunk_size);
        video_packet.pong = 0;
        video_packet.version = PACKET_VERSION;
        video_packet.airDeviceId = kTestAirDeviceId;
        video_packet.gsDeviceId = currentGsDeviceId();
        video_packet.resolution = Resolution::HD;
        video_packet.part_index = static_cast<uint8_t>(packet_index);
        video_packet.last_part = (packet_index + 1 == packet_count) ? 1 : 0;
        video_packet.frame_index = frame_index;
        sealFixedHeaderPacket(video_packet);

        std::vector<uint8_t> raw_packet(sizeof(video_packet) + chunk_size);
        std::memcpy(raw_packet.data(), &video_packet, sizeof(video_packet));
        std::memcpy(raw_packet.data() + sizeof(video_packet), m_static_jpeg.data() + offset, chunk_size);
        packets.push_back(std::move(raw_packet));
        offset += chunk_size;
    }
    return packets;
}

//===================================================================================
//===================================================================================
// Returns the currently active GS device id from the transport packet filter state.
uint16_t GSTestTransport::currentGsDeviceId() const
{
    const uint16_t filtered_gs_device_id = m_packet_filter.get_filter_to_device_id();
    if (filtered_gs_device_id != 0)
    {
        return filtered_gs_device_id;
    }

    const uint16_t packet_header_gs_device_id = m_packet_filter.get_packet_header_from_device_id();
    if (packet_header_gs_device_id != 0)
    {
        return packet_header_gs_device_id;
    }

    return s_groundstation_config.deviceId;
}

//===================================================================================
//===================================================================================
// Computes the payload size available for one video packet at the current MTU.
size_t GSTestTransport::maxVideoPayloadSize() const
{
    const size_t mtu = m_rx_descriptor.mtu > 0 ? m_rx_descriptor.mtu : AIR2GROUND_MAX_MTU;
    if (mtu <= sizeof(Air2Ground_Video_Packet))
    {
        return 0;
    }

    return mtu - sizeof(Air2Ground_Video_Packet);
}
