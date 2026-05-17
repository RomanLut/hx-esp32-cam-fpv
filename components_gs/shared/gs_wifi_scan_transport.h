#pragma once

#include <atomic>
#include <vector>

#include "gs_test_transport.h"

//===================================================================================
//===================================================================================
// Shared WiFi channel scan transport.
//
// Streams a blue background JPEG at 3 FPS and overlays a bar-graph OSD that
// shows the relative airtime of 802.11 packets observed on each scanned channel.
// The transport dwells on each channel for 300ms, advances to the next, and
// cycles continuously.  The set of channels is chosen from the band configured in
// s_groundstation_config.wifiBand.
//
// Platform subclasses implement the two pure-virtual hooks:
//   setMonitorChannel(int)       – switch the real WiFi adapter to the given channel
//   consumeReceivedPacketCount() – return (and reset) raw packets seen since last call
//
// All loopback streaming, FEC encoding and OSD packet delivery is inherited from
// GSTestTransport unchanged.
class GSWifiScanTransport : public GSTestTransport
{
public:
    bool init(const gs::core::RXDescriptor& rx_descriptor,
              const gs::core::TXDescriptor& tx_descriptor) override;

protected:
    // Override: fill m_static_jpeg with the embedded blue gradient.
    bool loadStaticJpeg() override;

    // Override: advance channel, store packet count, queue bar-graph OSD packet.
    void queueStatsOsdPacket(Clock::time_point now) override;

    // Platform hook: switch the real WiFi adapter to the given channel.
    virtual void setMonitorChannel(int channel) = 0;

    // Accumulates airtime_us = frame_bytes * 16 / rate_500kbps (clamped to 5 ms)
    // into m_airtimeUs.  rate_500kbps == 0 falls back to 6 Mbps.
    void accumulateAirtime(uint32_t frame_bytes, uint32_t rate_500kbps);

    // Default implementation exchanges m_airtimeUs and returns it.
    // Platforms may override if they need additional work.
    virtual int consumeReceivedPacketCount();

    std::atomic<uint32_t> m_airtimeUs = {0};

private:
    // Build RLE-encoded OSD content into out_buf; sets enc_size to bytes written.
    void buildScanOsd(uint8_t* out_buf, uint16_t& enc_size) const;

    // Return the ordered list of channels to scan for the currently configured band.
    std::vector<int> buildChannelList() const;

    std::vector<int>   m_channels;      // ordered channel numbers for the current band
    std::vector<float> m_packetCounts;  // per-channel EMA-smoothed airtime (µs per 300 ms window)
    int                m_currentIndex = 0;   // index of the channel currently being listened to
    uint8_t            m_lastBand     = 0xff; // band used to build current channel list
};
