#include "gs_wifi_scan_transport.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "gs_asset_loader.h"
#include "Log.h"
#include "crc.h"
#include "packets.h"
#include "gs_shared_state.h"
#include "gs_runtime_core.h"
#include "wifi_channels.h"

namespace
{

// Must match kTestAirDeviceId in gs_test_transport.cpp so session accepts our OSD packets.
constexpr uint16_t kScanAirDeviceId = 0x7777;

// OSD grid dimensions (from packets.h)
constexpr int kOsdRows = OSD_ROWS; // 20
constexpr int kOsdCols = OSD_COLS; // 53

// Bar graph layout (rows within the 20-row OSD grid).
// Rows 0-1 are top margin. Row 19 is always bottom margin.
// 2.4 GHz:      rows 18-19 are bottom margin; label uses 2 rows (16-17).
// 5.8 / dual:   row 19 is the only bottom margin; label uses 3 rows (16-18).
constexpr int kTitleRow    = 2;   // "SCANNING WIFI CHANNEL: N"
constexpr int kTitleLineRow = 3;  // solid horizontal line below title (top of box)
constexpr int kBarTop      = 4;   // top of bar area (one row shorter than before)
constexpr int kBarBottom   = 14;  // last bar row  (11 bar rows total: 4..14)
constexpr int kLineRow     = 15;  // solid horizontal line / bottom of box
constexpr int kDigitRow0   = 16;  // 2.4: tens digit; 5.8: hundreds digit (shown at transition)
constexpr int kDigitRow1   = 17;  // 2.4: units digit; 5.8: tens digit    (shown at transition)
constexpr int kDigitRow2   = 18;  // 5.8 only: units digit (shown at transition); 2.4: margin

constexpr int kBarRows     = kBarBottom - kBarTop + 1; // 12
constexpr float kBarFullAirtimeUs = 300000.f; // 300 ms of airtime = full bar height

//===================================================================================
//===================================================================================
// Writes one character into a flat OSD row buffer.
void osdPutChar(uint8_t grid[kOsdRows][kOsdCols], int row, int col, char c)
{
    if (row < 0 || row >= kOsdRows || col < 0 || col >= kOsdCols)
    {
        return;
    }
    grid[row][col] = static_cast<uint8_t>(c);
}

//===================================================================================
//===================================================================================
// Writes a null-terminated string into the OSD grid starting at (row, col).
void osdPutStr(uint8_t grid[kOsdRows][kOsdCols], int row, int col, const char* s)
{
    while (*s && col < kOsdCols)
    {
        osdPutChar(grid, row, col++, *s++);
    }
}

//===================================================================================
//===================================================================================
// RLE-encodes the flat character grid into out_buf.
// Returns the number of bytes written.
//
// Encoding (matches FlightOSD::update decode logic):
//   byte 1-254           → single character, high_bank unchanged
//   0x00, count, char    → run of 'count' chars (count bits 0-6); bit 7 of count
//                          toggles high_bank before writing
//   0x00, 0x00           → end-of-stream marker
//   0xFF, char           → toggle high_bank, then single char  (decode path)
//
// We only ever emit the 0x00-prefix run form for runs and raw bytes otherwise,
// which is the simplest safe subset of the encoding that the decoder accepts.
uint16_t rleEncode(const uint8_t grid[kOsdRows][kOsdCols], uint8_t* out, uint16_t max_size)
{
    uint16_t pos = 0;
    const int total = kOsdRows * kOsdCols;
    int src = 0;

    auto emit = [&](uint8_t b) -> bool
    {
        if (pos >= max_size)
        {
            return false;
        }
        out[pos++] = b;
        return true;
    };

    while (src < total)
    {
        const uint8_t c = reinterpret_cast<const uint8_t*>(grid)[src];

        // Count run length (max 127 to fit in 7 bits of the count byte)
        int run = 1;
        while (run < 127 && src + run < total &&
               reinterpret_cast<const uint8_t*>(grid)[src + run] == c)
        {
            run++;
        }

        if (run > 1)
        {
            // Emit run: 0x00, count (no bank toggle), char
            if (!emit(0x00)) { break; }
            if (!emit(static_cast<uint8_t>(run))) { break; }
            if (!emit(c)) { break; }
        }
        else
        {
            // Single character — emit raw unless it is a control byte (0x00 or 0xFF)
            if (c == 0x00 || c == 0xFF)
            {
                // Use run form with count=1
                if (!emit(0x00)) { break; }
                if (!emit(0x01)) { break; }
                if (!emit(c)) { break; }
            }
            else
            {
                if (!emit(c)) { break; }
            }
        }

        src += run;
    }

    // End-of-stream
    if (pos + 1 < max_size)
    {
        out[pos++] = 0x00;
        out[pos++] = 0x00;
    }

    return pos;
}

} // namespace

//===================================================================================
//===================================================================================
// Loads the blue scan-mode background JPEG from the GS asset bundle (wifi_scan_bg.jpg).
bool GSWifiScanTransport::loadStaticJpeg()
{
    return loadAssetJpeg("wifi_scan_bg.jpg", m_static_jpeg);
}

//===================================================================================
//===================================================================================
// Computes airtime_us from frame size and rate, clamps to 5 ms, and accumulates.
// rate_500kbps == 0 falls back to 6 Mbps (= 12 × 500 kb/s).
void GSWifiScanTransport::accumulateAirtime(uint32_t frame_bytes, uint32_t rate_500kbps)
{
    if (rate_500kbps == 0) { rate_500kbps = 12; }
    uint32_t airtime_us = (frame_bytes * 16 + rate_500kbps / 2) / rate_500kbps;
    if (airtime_us > 5000) { airtime_us = 5000; }
    m_airtimeUs.fetch_add(airtime_us, std::memory_order_relaxed);
}

//===================================================================================
//===================================================================================
int GSWifiScanTransport::consumeReceivedPacketCount()
{
    return static_cast<int>(m_airtimeUs.exchange(0, std::memory_order_relaxed));
}

//===================================================================================
//===================================================================================
// Returns the ordered list of WiFi channels to scan for the configured band.
std::vector<int> GSWifiScanTransport::buildChannelList() const
{
    const uint8_t band = s_groundstation_config.wifiBand;
    std::vector<int> channels;
    channels.reserve(WIFI_CHANNELS_COUNT);
    for (int i = 0; i < WIFI_CHANNELS_COUNT; ++i)
    {
        const int ch = WIFI_CHANNELS_BY_INDEX[i];
        if (isWifiChannelAllowedByBand(ch, band))
        {
            channels.push_back(ch);
        }
    }
    return channels;
}

//===================================================================================
//===================================================================================
// Initialises the scan transport: builds the channel list and starts on channel 0.
bool GSWifiScanTransport::init(const gs::core::RXDescriptor& rx_descriptor,
                               const gs::core::TXDescriptor& tx_descriptor)
{
    m_target_frame_period = std::chrono::milliseconds(300);

    if (!GSTestTransport::init(rx_descriptor, tx_descriptor))
    {
        return false;
    }

    m_lastBand     = s_groundstation_config.wifiBand;
    m_channels     = buildChannelList();
    m_packetCounts.assign(m_channels.size(), 0.f);
    m_currentIndex = 0;

    if (!m_channels.empty())
    {
        setMonitorChannel(m_channels[0]);
        LOGI("WifiScanTransport: scanning {} channels, starting on ch {}",
             m_channels.size(), m_channels[0]);
    }
    else
    {
        LOGW("WifiScanTransport: no channels in configured band");
    }

    return true;
}

//===================================================================================
//===================================================================================
// Called once per second by the base scheduler.
// Records packet count for the current channel, advances to the next, and queues
// an OSD packet with the updated bar graph.
void GSWifiScanTransport::queueStatsOsdPacket(Clock::time_point now)
{
    // Called every 300 ms (m_next_stats_tp is set below, aligned with m_target_frame_period).
    m_next_stats_tp = now + std::chrono::milliseconds(300);

    // --- rebuild channel list if the configured band has changed ---
    const uint8_t currentBand = s_groundstation_config.wifiBand;
    if (currentBand != m_lastBand)
    {
        m_lastBand     = currentBand;
        m_channels     = buildChannelList();
        m_packetCounts.assign(m_channels.size(), 0.f);
        m_currentIndex = 0;
        consumeReceivedPacketCount(); // discard stale counts from previous band
        LOGI("WifiScanTransport: band changed, rebuilding channel list ({} channels)", m_channels.size());
        if (!m_channels.empty())
        {
            setMonitorChannel(m_channels[0]);
        }
    }

    // --- record packets seen on the current channel and hop ---
    if (!m_channels.empty())
    {
        const float sample = static_cast<float>(consumeReceivedPacketCount());
        m_packetCounts[m_currentIndex] = m_packetCounts[m_currentIndex] * 0.5f + sample * 0.5f;

        // Advance channel
        m_currentIndex = (m_currentIndex + 1) % static_cast<int>(m_channels.size());
        setMonitorChannel(m_channels[m_currentIndex]);
    }

    // --- build and queue the OSD packet ---
    // Maximum encoded OSD payload size
    constexpr size_t kMaxOsdPayload = MAX_OSD_PAYLOAD_SIZE;

    // We need a flat buffer large enough for the packet header + encoded OSD
    const size_t packet_buf_size = sizeof(Air2Ground_OSD_Packet) + kMaxOsdPayload;
    std::vector<uint8_t> raw_packet(packet_buf_size, 0);

    auto* osd_packet = reinterpret_cast<Air2Ground_OSD_Packet*>(raw_packet.data());
    osd_packet->type       = Air2Ground_Header::Type::OSD;
    osd_packet->pong       = 0;
    osd_packet->version    = PACKET_VERSION;
    osd_packet->airDeviceId = kScanAirDeviceId;
    osd_packet->gsDeviceId = s_groundstation_config.deviceId;

    // Build encoded OSD into the bytes immediately following osd_enc_start
    uint8_t* enc_ptr = &osd_packet->osd_enc_start;
    uint16_t enc_size = 0;
    buildScanOsd(enc_ptr, enc_size);

    // Total packet size = fixed header up to (and including) osd_enc_start + encoded bytes
    // sizeof(Air2Ground_OSD_Packet) - 1 == byte offset of osd_enc_start (it is the last 1-byte field).
    // Avoids offsetof on a non-standard-layout type.
    const uint32_t total_size = static_cast<uint32_t>(
        sizeof(Air2Ground_OSD_Packet) - 1 + enc_size);
    osd_packet->size = total_size;

    // Seal CRC over the fixed header only (same as GSTestTransport)
    osd_packet->crc = 0;
    osd_packet->crc = crc8(0, osd_packet, sizeof(Air2Ground_OSD_Packet));

    raw_packet.resize(total_size);
    m_pending_packets.push_back(std::move(raw_packet));

    // Pad with copies of the OSD packet to complete the current FEC block so it
    // encodes immediately rather than waiting for k packets to accumulate.
    if (m_effective_coding_k > 1)
    {
        const size_t remainder = m_pending_packets.size() % m_effective_coding_k;
        if (remainder != 0)
        {
            const size_t needed = m_effective_coding_k - remainder;
            for (size_t i = 0; i < needed; ++i)
            {
                m_pending_packets.push_back(m_pending_packets.back());
            }
        }
    }
}

//===================================================================================
//===================================================================================
// Fills out_buf with the RLE-encoded bar graph OSD and sets enc_size.
void GSWifiScanTransport::buildScanOsd(uint8_t* out_buf, uint16_t& enc_size) const
{
    // Build the 20×53 character grid (space-filled)
    uint8_t grid[kOsdRows][kOsdCols];
    std::memset(grid, ' ', sizeof(grid));

    const int n = static_cast<int>(m_channels.size());

    // --- title row ---
    const int current_channel = m_channels.empty() ? 0 : m_channels[m_currentIndex];
    char title[kOsdCols + 1];
    snprintf(title, sizeof(title), "SCANNING WIFI CHANNEL: %d", current_channel);
    const int title_len = static_cast<int>(strlen(title));
    const int title_col = (kOsdCols - title_len) / 2;
    osdPutStr(grid, kTitleRow, title_col, title);

    if (n > 0)
    {
        const bool only24 = (s_groundstation_config.wifiBand == GS_WIFI_BAND_2_4_GHZ);
        // 2.4 GHz only: space bars out with one empty column before and after each bar.
        // Other bands: pack bars tightly (one column per channel).
        const int col_stride = only24 ? 3 : 1; // columns per channel slot
        const int col_offset = only24 ? 1 : 0; // bar within its slot

        // Reserve 1 column on each side for the '!' vertical borders.
        const int bar_cols    = std::min(n, (kOsdCols - 2) / col_stride);
        const int total_width = bar_cols * col_stride;
        const int start_col   = (kOsdCols - total_width) / 2;
        const int left_col    = start_col - 1;
        const int right_col   = start_col + total_width;

        // --- box: top horizontal line (title underline) ---
        for (int c = left_col; c <= right_col; ++c)
            osdPutChar(grid, kTitleLineRow, c, '-');

        // --- box: vertical borders along the bar area ---
        for (int r = kBarTop; r <= kBarBottom; ++r)
        {
            osdPutChar(grid, r, left_col,  '!');
            osdPutChar(grid, r, right_col, '!');
        }

        // --- bars ---
        for (int i = 0; i < bar_cols; ++i)
        {
            const int col   = start_col + i * col_stride + col_offset;
            const float sample = m_packetCounts[i];
            const float ratio = std::min(1.f, sample / kBarFullAirtimeUs);
            int height  = static_cast<int>(std::pow(ratio, 0.5f) * kBarRows);
            // Sparse beacon/control traffic may be real but below one row after
            // airtime scaling; keep nonzero samples visible in scan mode.
            if (sample > 0.f && height == 0)
            {
                height = 1;
            }
            for (int h = 0; h < height; ++h)
            {
                osdPutChar(grid, kBarBottom - h, col, '^');
            }
        }

        // --- box: bottom horizontal line (separator) ---
        // Solid line with '*' marking the currently-scanned channel in all modes.
        for (int c = left_col; c <= right_col; ++c)
            osdPutChar(grid, kLineRow, c, '-');
        {
            const int cur_col = start_col + m_currentIndex * col_stride + col_offset;
            osdPutChar(grid, kLineRow, cur_col, '*');
        }

        // --- channel numbers ---
        for (int i = 0; i < bar_cols; ++i)
        {
            const int ch      = m_channels[i];
            const int col     = start_col + i * col_stride + col_offset;

            if (only24)
            {
                // Single row: two-digit number (e.g. "06", "13")
                osdPutChar(grid, kDigitRow1, col - 1, static_cast<char>('0' + ch / 10));
                osdPutChar(grid, kDigitRow1, col,     static_cast<char>('0' + ch % 10));
            }
            else
            {
                // Three rows: hundreds / tens / units — each digit shown only at transition.
                // Row 19 is the single bottom margin in this mode.
                const int prev_ch = (i > 0) ? m_channels[i - 1] : -1;

                const int h      = ch / 100;
                const int t      = (ch % 100) / 10;
                const int u      = ch % 10;
                const int prev_h = (prev_ch >= 0) ? prev_ch / 100        : -1;
                const int prev_t = (prev_ch >= 0) ? (prev_ch % 100) / 10 : -1;
                const int prev_u = (prev_ch >= 0) ? prev_ch % 10          : -1;

                // Hundreds: only show non-zero digit at the boundary (e.g. '1' at ch 100)
                if (h > 0 && h != prev_h)
                    osdPutChar(grid, kDigitRow0, col, static_cast<char>('0' + h));

                // Tens: show at every decade boundary, or when hundreds rolled over
                if (t != prev_t || h != prev_h)
                    osdPutChar(grid, kDigitRow1, col, static_cast<char>('0' + t));

                // Units: show whenever any higher digit changed, or units itself changed
                if (u != prev_u || t != prev_t || h != prev_h)
                    osdPutChar(grid, kDigitRow2, col, static_cast<char>('0' + u));
            }
        }
    }

    enc_size = rleEncode(grid, out_buf, static_cast<uint16_t>(MAX_OSD_PAYLOAD_SIZE));
}
