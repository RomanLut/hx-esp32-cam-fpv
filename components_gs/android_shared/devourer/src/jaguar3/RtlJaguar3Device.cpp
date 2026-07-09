#include "RtlJaguar3Device.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "BeamformingSounder.h" /* generation-neutral BF self-sounding recipe */

#include "ChannelFreq.h" /* radiotap CHANNEL freq -> channel (per-packet hop) */
#include "FrameParserJaguar3.h"
#include "NhmReader.h"       /* frame-free NHM power histogram (shared) */
#include "RateDefinitions.h" /* MGN_* rate enum (shared across the family) */
#include "SignalStop.h" /* g_devourer_should_stop — set by demo signal handlers */
#include "ToneMask.h"   /* DEVOURER_RX_CSI_MASK / DEVOURER_RX_NBI knobs */

extern "C" {
#include "ieee80211_radiotap.h" /* MRateToHwRate + radiotap iterator */
}

/* 8822e TXAGC reference base (7-bit index, ~0.25 dB/step) programmed by default
 * when DEVOURER_TX_PWR is unset. Matches the efuse-calibrated HT/OFDM base the
 * kernel derives for this family (its tx_power_idx proc reports base 75 for 5G
 * HT); verified on-air that programming it lifts the MCS7 signal from ~7 dB to
 * the kernel's ~10 dB above the SDR noise floor. See InitWrite for context. */
static constexpr uint8_t JAGUAR3_TXPWR_REF_BASE = 0x4b;
/* 8822c (CU) flat reference: the value the demo previously imposed (40 = 0x28),
 * kept so the SDR-validated CU behaviour is unchanged after the demo stopped
 * setting a default. */
static constexpr uint8_t JAGUAR3_TXPWR_REF_BASE_8822C = 0x28;

RtlJaguar3Device::RtlJaguar3Device(RtlAdapter device, Logger_t logger,
                                   jaguar3::ChipVariant variant,
                                   devourer::DeviceConfig cfg)
    : _device{device}, _cfg{std::move(cfg)}, _logger{logger},
      _variant{variant}, _hal{device, logger, variant, _cfg},
      _radioManagement{device, logger, variant, _cfg} {
  _logger->info("RtlJaguar3Device constructed ({})",
                variant == jaguar3::ChipVariant::C8822E ? "8822E/EU" : "8822C/CU");
}

void RtlJaguar3Device::Init(Action_ParsedRadioPacket packetProcessor,
                            SelectedChannel channel) {
  _channel = channel;
  _rx_wanted = true; /* RX-side bring-up: no TXAGC apply may touch 0x41e8 */
  _hal.rtw_hal_init(channel);  /* full vendor-source bring-up */
  /* Tune the channel/bandwidth (5/10 MHz ChannelWidth re-clocks to narrowband),
   * then run IQK calibration (it reads RF18 for the tuned channel). */
  _radioManagement.set_channel_bwmode(channel.Channel, channel.ChannelOffset,
                                      channel.ChannelWidth);
  _hal.run_iqk(channel);
  _hal.enable_rx_path(); /* RF into RX mode (IGI toggle) — must follow channel set */
  _hal.config_rfe(channel.Channel); /* 8822e RFE/PAPE antenna-switch pins */
  _hal.config_channel_8822e(channel.Channel); /* 8822e band TX scaling/backoff + shaping */
  _hal.coex_wlan_only_init(); /* lock antenna to WLAN (disable BT/LTE coex) */
  _brought_up = true;

  /* DEVOURER_XTAL_CAP — crystal-cap trim (issue #217, narrowband CFO lever). */
  if (_cfg.tuning.xtal_cap)
    SetXtalCap(*_cfg.tuning.xtal_cap);

  /* tuning.disable_cca — MAC EDCCA-disable research knob (see SetCcaMode). */
  if (_cfg.tuning.disable_cca)
    SetCcaMode(true);

  /* DEVOURER_BF_ARM_BFEE=aa:bb:cc:dd:ee:ff — beamforming self-sounding probe
   * (beamformee side), Jaguar-3 variant. Arms the hardware CSI responder to
   * reply to NDPA+NDP from the given beamformer MAC with a VHT Compressed
   * Beamforming report, no association. Uses the shared MAC recipe with the
   * Jaguar-2/3 config (0xDB, 16-bit CSI param, RX-filter + own-AID gates, and
   * crucially NO 0x9B4 write — that address is the narrowband clock divider on
   * this generation). See BeamformingSounder.h. */
  if (_cfg.bf.beamformee_of) {
    const uint8_t *mac = _cfg.bf.beamformee_of->data();
    /* Jaguar-3 bring-up never programs the self-MAC (0x0610), so the NDPA
     * RA has nothing to match. Give the beamformee a known identity here so
     * the sounder can address it; log it for the test harness. */
    static const uint8_t kBfeeMac[6] = {0x00, 0xe0, 0x4c, 0x88, 0x22, 0xce};
    for (uint16_t i = 0; i < 6; ++i)
      _device.rtw_write8(0x0610 + i, kBfeeMac[i]);
    /* bf.mu upgrades the responder to an MU beamformee, whose report appends
     * the per-subcarrier delta-SNR (MU Exclusive Beamforming Report) the SU
     * report omits. */
    if (_cfg.bf.mu) {
      devourer::bf::arm_beamformee_mu(_device, mac, devourer::bf::kBfeeJaguar23);
      _logger->info("Jaguar3 BF MU-beamformee armed for beamformer "
                    "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x} — "
                    "beamformee MAC 00:e0:4c:88:22:ce",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
      devourer::bf::arm_beamformee(_device, mac, devourer::bf::kBfeeJaguar23);
      _logger->info("Jaguar3 BF beamformee armed for beamformer "
                    "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x} — "
                    "beamformee MAC 00:e0:4c:88:22:ce",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
  }

  StartRxLoop(std::move(packetProcessor));
}

void RtlJaguar3Device::StartRxLoop(Action_ParsedRadioPacket packetProcessor) {
  _packetProcessor = std::move(packetProcessor);
  /* Restartable: clear any stop request left by a prior StopRxLoop(). */
  _rx_stop = false;
  /* Take over bulk-IN from the coex thread's C2H drain up front — also during
   * the register restore below, so its 200 ms bulk reads don't interleave with
   * the RX-path enable sequence. */
  _rx_loop_active = true;

  /* An InitWrite bring-up is TX-oriented: it closed the RX filters
   * (0x6A0-0x6A4, so a TX-only session's unread frames can't fill the on-chip
   * RX FIFO) and never ran enable_rx_path (the 3-wire IGI toggle that puts the
   * RF into RX mode — without it the chip delivers zero frames). This caller
   * wants RX, so restore the monitor_rx_cfg accept-all filters and enable the
   * RX path. Serialized against the coex housekeeping tick, the only other
   * register writer right now. */
  if (_rx_filters_closed) {
    /* A register read can transiently fail right after InitWrite (the FW is
     * still settling from the coex H2C burst) — give it a beat and retry a few
     * times instead of letting the exception tear down the caller's RX
     * thread. */
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (int attempt = 0; attempt < 5; ++attempt) {
      try {
        std::lock_guard<std::mutex> lk(_reg_mu);
        _device.rtw_write<uint16_t>(0x06A0, 0xFFFF);
        _device.rtw_write<uint16_t>(0x06A2, 0xFFFF);
        _device.rtw_write<uint16_t>(0x06A4, 0xFFFF);
        _hal.enable_rx_path();
        _rx_filters_closed = false;
        _logger->info("Jaguar3: RX filters re-opened + RX path enabled for "
                      "concurrent TX+RX");
        break;
      } catch (const std::exception &e) {
        _logger->error("Jaguar3: RX-path restore failed ({}){}", e.what(),
                       attempt < 4 ? " — retrying" : " — giving up");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
    }
  }

  /* DEVOURER_RX_CSI_MASK / DEVOURER_RX_NBI — RX-side per-subcarrier masking
   * (receive-equalizer CSI mask / narrowband notch). Applied after the channel
   * set so it is the final word for a single-channel capture; serialized
   * against the coex housekeeping tick like every other register writer here.
   * See ToneMask.h. */
  if (_cfg.rx.csi_mask || _cfg.rx.nbi) {
    std::lock_guard<std::mutex> lk(_reg_mu);
    devourer::tonemask::apply(
        _device, _logger, devourer::tonemask::Family::JGR3, _channel, 2,
        _cfg.rx.csi_mask ? _cfg.rx.csi_mask->c_str() : nullptr,
        _cfg.rx.nbi ? _cfg.rx.nbi->c_str() : nullptr);
  }

  _logger->info("Jaguar3: entering RX loop (kernel-style async URB queue)");
  uint64_t frames = 0, reads = 0;
  /* Closed-loop CFO tracking runs on the RECEIVER (#217): tick the controller
   * on a ~2 s wall-clock cadence from within the RX loop, off the per-frame
   * CFO fed below. (The coex thread's copy only runs on the TX path.) The
   * crystal-cap register (0x1040) is written READ-FREE from a base cached
   * before the RX flood — a control-transfer READ races the async bulk-IN and
   * throws under load, so the tick composes the field onto the cached base and
   * write32s it. */
  uint32_t reg1040_base = 0;
  bool reg1040_ok = false;
  if (_cfg.tuning.cfo_track) {
    try {
      reg1040_base = _device.rtw_read<uint32_t>(0x1040) & ~0x00FFFC00u;
      reg1040_ok = true;
    } catch (...) {
      _logger->info("Jaguar3 cfo.track: 0x1040 base read failed — disabled");
    }
  }
  auto cfo_next = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  auto cfo_tick = [&]() {
    if (!_cfg.tuning.cfo_track || !reg1040_ok)
      return;
    if (std::chrono::steady_clock::now() < cfo_next)
      return;
    cfo_next += std::chrono::seconds(2);
    double avg_khz = 0;
    const int cur = _xtal_cap < 0 ? 0x20 : _xtal_cap;
    const int nc = _cfo.step(cur, 0x7f, &avg_khz);
    DVR_DEBUG(_logger, "Jaguar3 cfo.track tick: cfo~{} (raw*2.5) cap=0x{:02x} {}",
              static_cast<int>(avg_khz), cur, nc >= 0 ? "step" : "hold");
    if (nc >= 0) {
      const uint32_t field = static_cast<uint32_t>(nc) |
                             (static_cast<uint32_t>(nc) << 7); /* 14-bit */
      try {
        std::lock_guard<std::mutex> lk(_reg_mu);
        _device.rtw_write<uint32_t>(0x1040,
                                    reg1040_base | ((field << 10) & 0x00FFFC00u));
        _xtal_cap = nc;
      } catch (...) {
        return;
      }
    }
    if (nc >= 0) /* log only on an actual step, not every idle tick */
      _logger->info("Jaguar3 cfo.track: cfo~{} (raw*2.5) xtal_cap=0x{:02x}",
                    static_cast<int>(avg_khz), _xtal_cap);
  };
  /* Process one bulk-IN completion: walk the aggregated 8822C RX descriptors. */
  auto on_data = [&](const uint8_t *data, int n) {
    cfo_tick();
    if (++reads <= 8)
      _logger->info("Jaguar3 RX: async completion #{} -> {} bytes", reads, n);
    uint32_t off = 0;
    while (off + jaguar3::RXDESC_SIZE_8822C <= static_cast<uint32_t>(n)) {
      jaguar3::Rx8822cFrame f;
      if (!jaguar3::parse_rx_8822c(data + off, static_cast<size_t>(n) - off, f))
        break;
      if (_packetProcessor) {
        Packet p{};
        p.RxAtrib.pkt_len = static_cast<uint16_t>(f.frame_len);
        p.RxAtrib.crc_err = f.crc_err;
        p.RxAtrib.icv_err = f.icv_err;
        p.RxAtrib.data_rate = f.rx_rate;
        p.RxAtrib.tsfl = f.tsfl;
        p.RxAtrib.drvinfo_sz = static_cast<uint8_t>(f.drvinfo_size);
        p.RxAtrib.shift_sz = f.shift;
        /* RX desc word2 BIT(28) = FW C2H report, not an 802.11 frame. During
         * concurrent TX+RX the FW emits one small C2H per TX, so tag them for
         * the packetProcessor (which skips C2H) instead of surfacing a flood
         * of short "frames". Same check the coex drain uses. */
        bool is_c2h = (data[off + 11] & 0x10) != 0;
        p.RxAtrib.pkt_rpt_type = is_c2h ? RX_PACKET_TYPE::C2H_PACKET
                                        : RX_PACKET_TYPE::NORMAL_RX;
        /* Decode the jgr3 PHY-status report (per-frame RSSI/SNR/EVM) when it is
         * present (monitor_rx_cfg enables APP_PHYSTS + RX_DRVINFO_SZ=4, so the
         * 32-byte report is counted in drvinfo). Skips C2H reports and any
         * frame whose drvinfo is too short (e.g. CCK, which carries no OFDM
         * report). The report sits immediately after the 24-byte descriptor. */
        if (!is_c2h && f.drvinfo_size >= 28)
          jaguar3::parse_phy_sts_jgr3(data + off + jaguar3::RXDESC_SIZE_8822C,
                                      f.drvinfo_size, p.RxAtrib);
        p.Data = std::span<uint8_t>(const_cast<uint8_t *>(f.frame), f.frame_len);
        if (!p.RxAtrib.crc_err) {
          _rxq.add(p.RxAtrib.rssi[0], p.RxAtrib.snr[0], p.RxAtrib.evm[0]);
          _rxpaths.add(p.RxAtrib.rssi, 2); /* 8822C/8822E are 2T2R */
          if (_cfg.tuning.cfo_track)
            _cfo.add(p.RxAtrib.cfo_tail); /* closed-loop CFO input (#217) */
        }
        /* TX-BF apply gate (DEVOURER_BF_TXBF): a VHT Compressed Beamforming
         * Report (category 0x15, action 0x00) from the target peer (addr2) means
         * the WMAC just ingested a fresh V matrix — enable the apply toggle so
         * subsequent TX is steered. Enabling it before any CBR (no V) degrades
         * the link, so this is the gate. One-shot; V refreshes on each periodic
         * re-sound automatically. */
        if (_bf_txbf_armed && !p.RxAtrib.crc_err && f.frame_len >= 26) {
          const uint8_t *fb = f.frame;
          if (fb[24] == 0x15 && fb[25] == 0x00 &&
              std::memcmp(fb + 10, _bf_peer, 6) == 0) {
            _bf_cbr_count.fetch_add(1, std::memory_order_relaxed);
            if (!_bf_apply_on.load(std::memory_order_relaxed)) {
              std::lock_guard<std::mutex> lk(_reg_mu);
              devourer::bf::apply_vmatrix(
                  _device, true, static_cast<uint8_t>(_channel.ChannelWidth));
              _bf_apply_on.store(true, std::memory_order_relaxed);
              _logger->info("Jaguar3 BF: CBR from peer ingested — TXBF apply "
                            "ENABLED (steering subsequent TX)");
            }
          }
        }
        _packetProcessor(p);
      }
      if (++frames <= 5)
        _logger->info("Jaguar3 RX: frame len={} rate={} crc={}", f.frame_len,
                      f.rx_rate, f.crc_err);
      if (f.next_offset == 0)
        break;
      off += f.next_offset;
    }
  };
  /* _rx_loop_active was set on entry (bulk-IN handover from the coex thread's
   * C2H drain — its one in-flight 200 ms bulk_read_raw may steal at most one
   * completion, harmless). Give the endpoint back on exit. */
  _device.bulk_read_async_loop(32 * 1024, 8, on_data, [this]() -> bool {
    return _rx_stop || g_devourer_should_stop;
  });
  _rx_loop_active = false;
}

/* Coex runtime (port of the rtw88 watchdog's coex path): drain the firmware's
 * C2H reports off bulk-IN so the on-chip C2H buffer never fills, and every ~2 s
 * re-apply the 5 GHz coex decision + FW heartbeats so the PTA keeps the antenna
 * with WLAN during sustained TX. Runs concurrently with the TX loop. */
/* Ensure the coex thread is always joined, even if Stop() was never called
 * (exception path / caller that drops the device) — a joinable std::thread in
 * the destructor would otherwise std::terminate. Idempotent with Stop(). */
RtlJaguar3Device::~RtlJaguar3Device() {
  /* Safety net: restore the chip if a CW tone is still armed (before the coex
   * thread is joined — StopCwTone serializes on _reg_mu with it). */
  StopCwTone();
  _coex_stop = true;
  if (_coex_thread.joinable())
    _coex_thread.join();
}

void RtlJaguar3Device::coex_runtime_loop() {
  std::vector<uint8_t> buf(16 * 1024);
  uint64_t tick = 0, c2h = 0, rx = 0;
  /* The coex decision + FW heartbeats run on a fixed ~2 s WALL-CLOCK cadence
   * (steady_clock), independent of how fast bulk-IN completes — a busy bulk-IN
   * pipe must not turn the keepalive into an H2C storm that floods the HMEBOX. */
  auto next_tick = std::chrono::steady_clock::now();
  const auto period = std::chrono::seconds(2);
  while (!_coex_stop && !g_devourer_should_stop) {
    if (!_rx_loop_active.load()) {
      /* Drain bulk-IN (empties the FW C2H buffer). 200 ms timeout bounds
       * shutdown latency when the pipe is idle. */
      int n =
          _device.bulk_read_raw(buf.data(), static_cast<int>(buf.size()), 200);
      if (n >= static_cast<int>(jaguar3::RXDESC_SIZE_8822C)) {
        ++rx;
        if (buf[11] & 0x10) /* RX desc word2 BIT(28) = C2H report */
          ++c2h;
      }
    } else {
      /* StartRxLoop owns bulk-IN — its async URB queue sees the C2H reports
       * as part of the RX stream. Keep the loop pacing without reading. */
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (std::chrono::steady_clock::now() < next_tick)
      continue;
    next_tick += period;
    try {
      std::lock_guard<std::mutex> lk(_reg_mu);
      _hal.coex_run_5g();
      _hal.pwr_track(); /* thermal TX-power compensation (sustains upper 5 GHz) */
      _hal.fw_update_wl_phy_info();
      _hal.fw_set_pwr_mode_active();
      _hal.fw_coex_query_bt_info();
    } catch (...) { break; }
    if (++tick <= 3 || tick % 15 == 0)
      _logger->info("Jaguar3 coex: tick {} (bulk-IN reads={}, C2H={})", tick, rx,
                    c2h);
  }
}

/* Clean shutdown — see IRtlDevice::Stop. Best-effort: a chip that already
 * dropped off the bus will make the de-init writes fail, which is fine. */
void RtlJaguar3Device::Stop() {
  _coex_stop = true;
  if (_coex_thread.joinable())
    _coex_thread.join();
  try {
    _hal.rtw_hal_deinit();
  } catch (...) {
    _logger->info("Jaguar3: Stop() de-init writes failed (chip already gone?)");
  }
}

void RtlJaguar3Device::InitWrite(SelectedChannel channel) {
  _channel = channel;
  /* Concurrent TX+RX intent (DEVOURER_TX_WITH_RX / a later StartRxLoop on this
   * bring-up): enable the RX path at the same point in the sequence Init does
   * and keep the RX filters open. Retrofitting RX state after the TX-oriented
   * bring-up completes is not reliable (validated on 8822EU: enable_rx_path
   * after the coex/FW steps leaves the RX path dead, and the register RMWs
   * race the running TX). */
  const bool want_rx = _cfg.rx.enable_with_tx;
  _rx_wanted = want_rx; /* consumed by every TXAGC ref write (0x41e8 quirk) */
  _hal.rtw_hal_init(channel);  /* full vendor-source bring-up */
  _radioManagement.set_channel_bwmode(channel.Channel, channel.ChannelOffset,
                                      channel.ChannelWidth);
  _hal.run_iqk(channel);
  if (want_rx)
    _hal.enable_rx_path(); /* RF into RX mode — same slot as the Init path */
  _hal.dpk_force_bypass_8822e(); /* 8822e rfe 21/22: kernel bypasses DPK (after IQK) */
  _hal.config_rfe(channel.Channel); /* 8822e RFE/PAPE antenna-switch pins (PA enable) */
  _hal.config_channel_8822e(channel.Channel); /* 8822e band TX scaling/backoff + shaping */

  /* DEVOURER_CW_TONE — a bare RF LO carrier. Armed HERE (before the FW power-mode
   * / coex H2C steps below, which on the 8812EU at 5 GHz leave the chip NAKing
   * control-IN reads that StartCwTone's BB reads need). A bare tone needs none of
   * the per-rate TX power / coex / FW-coex setup. On the 8822e the RFE PA-enable
   * pins (GPIO_MUXCFG 0x40 / PAD_CTRL 0x64) still must be driven for 5 GHz PA
   * output — but that write ALSO breaks subsequent BB reads, so we snapshot the
   * two registers first, arm the tone (reads intact), then blind-write the PA
   * pins. No coex thread (its tick would re-drive RF 0x00 back to RX). */
  if (_cfg.tx.cw_tone) {
    const uint8_t g = _cfg.tx.cw_tone_gain & 0x1F;
    const bool is_eu = _variant == jaguar3::ChipVariant::C8822E;
    uint32_t v40 = 0, v64 = 0;
    if (is_eu) {
      v40 = _device.rtw_read<uint32_t>(0x0040);
      v64 = _device.rtw_read<uint32_t>(0x0064);
    }
    for (int attempt = 1;; ++attempt) {
      try {
        StartCwTone(g);
        break;
      } catch (const std::exception &ex) {
        if (attempt >= 4) {
          _logger->error("CW tone arm failed after retries ({})", ex.what());
          break;
        }
        _logger->error("CW tone arm: USB glitch ({}) — retry {}/3", ex.what(),
                       attempt);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      }
    }
    if (is_eu) { /* drive the RFE PA-enable pins with blind writes (no read) */
      _device.rtw_write<uint32_t>(0x0040, v40 | 0x14030008u);
      _device.rtw_write<uint32_t>(0x0064, v64 & ~0x02040000u);
    }
    _logger->info("Jaguar3: CW tone hold (minimal bring-up, no coex thread)");
    return;
  }

  /* TXAGC from the current runtime-knob state (flat override / offset folded
   * onto the efuse-calibrated refs) — see apply_tx_power_current. Pre-coex,
   * so no _reg_mu needed here. */
  apply_tx_power_current(/*full=*/true);
  _brought_up = true;
  /* WiFi-only coex bring-up: disable the BT/LTE antenna arbitration and lock the
   * antenna to WLAN so on-air TX is not killed by the coex firmware. */
  _hal.coex_wlan_only_init();
  /* RFE GPIO/pad pinmux — the HalMAC "Config PIN Mux" (halmac_init_8822e) that
   * devourer's hand-rolled MAC init skips: route + drive the RFE PA-enable /
   * antenna-switch control pins. Without it the 8822e's PA pins are never driven
   * and cold-start TX is ~3-5 dB weak on ALL bands (the kernel-primed chip only
   * "worked" because the kernel's HalMAC had set this and it survived rmmod).
   * MUST run AFTER coex_wlan_only_init — coex sets GPIO_MUXCFG BT-PTA bits and
   * would otherwise mask the RFE routing. Set the RFE mux bits of GPIO_MUXCFG
   * (0x40) and clear the pad-block bits of PAD_CTRL1 (0x64). Validated on-air
   * (cold, power-cycled): lifts cold 5 GHz from ~5% to ~86% duty @6 dB = kernel
   * parity. 8822e only (rfe 21-24 front-end). */
  if (_variant == jaguar3::ChipVariant::C8822E) {
    /* Raw read-modify-write: phy_set_bb_reg shifts the value by the mask's
     * trailing-zero count (contiguous-field semantics), which mangles these
     * non-contiguous multi-bit masks. */
    uint32_t v40 = _device.rtw_read<uint32_t>(0x0040);
    _device.rtw_write<uint32_t>(0x0040, v40 | 0x14030008u); /* GPIO_MUXCFG: RFE mux */
    uint32_t v64 = _device.rtw_read<uint32_t>(0x0064);
    _device.rtw_write<uint32_t>(0x0064, v64 & ~0x02040000u); /* PAD_CTRL1: RFE pads */
  }

  _hal.fw_set_pwr_mode_active(); /* keep all FW power domains on (no auto-PS) */
  _hal.fw_coex_query_bt_info();  /* make the FW confirm BT is absent */
  _hal.fw_coex_tdma_off();       /* disable coex time-division (WL keeps antenna) */
  /* DEVOURER_BF_ARM_SOUNDER=1 — beamforming self-sounding probe (beamformer
   * side): arm the MAC's hardware sounding engine so a TX-descriptor-marked
   * NDPA (DEVOURER_TX_NDPA=1) is followed by a hardware-generated NDP. The MAC
   * sounding registers are family-neutral (BeamformingSounder.h); the Jaguar-3
   * sounding-protocol control byte is 0xDB (hal_txbf_8822b_enter, shared
   * 8822B/C/E) where Jaguar-1 uses 0xCB. */
  if (_cfg.bf.arm_sounder || _cfg.bf.sounder_self_mac) {
    /* Jaguar-3 bring-up never programs the self-MAC (0x0610); the sounding
     * engine matches the injected NDPA's TA against it before firing the NDP.
     * bf.sounder_self_mac programs that MAC (use the NDPA TA the caller
     * injects — the txdemo's canonical SA); unset leaves it unprogrammed
     * (Jaguar-1 semantics). */
    if (_cfg.bf.sounder_self_mac) {
      const uint8_t *m = _cfg.bf.sounder_self_mac->data();
      for (uint16_t i = 0; i < 6; ++i)
        _device.rtw_write8(0x0610 + i, m[i]);
      _logger->info("Jaguar3 BF sounder: self-MAC programmed to "
                    "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                    m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    _hal.txbf_rfmode_sounder(); /* RF/BB sounding config — needed for the NDP */
    devourer::bf::arm_sounder(_device, /*snd_ptcl_ctrl=*/0xDB);
    _logger->info("Jaguar3 BF sounder armed (beamformer side)");
  }

  /* DEVOURER_BF_TXBF=aa:bb:cc:dd:ee:ff — TX beamforming APPLY (needs the sounder
   * above + DEVOURER_TX_WITH_RX so the beamformer's RX can receive the peer's
   * report). Configure the beamformer entry for `peer_mac` so the WMAC accepts
   * and ingests the peer's Compressed Beamforming Report into the V matrix (in
   * hardware). The apply toggle is NOT flipped here — it is enabled from the RX
   * loop once a CBR from the peer is seen (a blind apply with no V degrades the
   * link). Periodic re-sounding (DEVOURER_TX_NDPA=N>1) keeps V fresh. */
  if (_cfg.bf.txbf_peer) {
    const uint8_t *m = _cfg.bf.txbf_peer->data();
    for (int i = 0; i < 6; ++i) _bf_peer[i] = m[i];
    devourer::bf::arm_beamformer_entry(_device, _bf_peer, /*rx_nss=*/2);
    _bf_txbf_armed = true;
    _logger->info("Jaguar3 BF TXBF entry configured for peer "
                  "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x} — apply gates "
                  "on the first CBR (needs rx.enable_with_tx)",
                  m[0], m[1], m[2], m[3], m[4], m[5]);
  }
  /* TX-only: the bring-up enables accept-all RX (monitor_rx_cfg), but the TX
   * path never reads bulk-IN — so on a trafficked channel the on-chip RX FIFO
   * fills with unread frames and throttles TX. Close the RX filter so no
   * over-the-air frames are buffered (the coex thread still receives FW C2H
   * reports). Skipped when the caller wants concurrent RX — the RX loop will
   * drain bulk-IN. */
  if (!want_rx) {
    _device.rtw_write<uint16_t>(0x06A0, 0x0000);
    _device.rtw_write<uint16_t>(0x06A2, 0x0000);
    _device.rtw_write<uint16_t>(0x06A4, 0x0000);
    _rx_filters_closed = true; /* StartRxLoop re-opens them (best effort) */
  }
  /* DEVOURER_CW_TONE — radiate a bare RF LO carrier at the channel center (MP
   * single-tone). Armed BEFORE the coex thread starts so the arming writes don't
   * contend with the coex tick. DEVOURER_CW_TONE_GAIN=0..31 sets RF 0x00[4:0]. */
  /* Start the coex runtime thread: it drains C2H and re-applies the 5 GHz coex
   * decision every ~2 s so sustained TX isn't silenced by the FW's PTA. (The CW
   * tone path returned earlier and never reaches here.) */
  /* DEVOURER_DIS_CCA=1 — measure-first EDCCA-disable knob (see SetCcaMode).
   * Applied before the coex thread starts so the writes don't contend. */
  if (_cfg.tuning.disable_cca)
    SetCcaMode(true);
  /* DEVOURER_XTAL_CAP — crystal-cap trim (issue #217); before the coex thread
   * so the AFE write doesn't contend with the periodic coex re-apply. */
  if (_cfg.tuning.xtal_cap)
    SetXtalCap(*_cfg.tuning.xtal_cap);
  _coex_thread = std::thread([this] { coex_runtime_loop(); });
  _logger->info("Jaguar3: ready for TX (monitor inject)");
}

/* MP single-tone (CW carrier), Jaguar3 (rtl8822c / rtl8822e) path A. Ported from
 * the vendor phydm_mp_set_single_tone_jgr3 + phydm_set_pmac_txon_jgr3. Two
 * Jaguar3-specific quirks the older chips don't have:
 *   1. RF 0x00 (the RF-mode register) is WRITE-ONLY through the HSSI 3-wire port
 *      (0x1808 path A); the direct BB->RF window (0x3c00) is a read-only shadow
 *      for RF 0x00, so a window write is silently dropped. RF 0x58 (LO enable)
 *      is a normal RF reg and uses the direct window.
 *   2. The bare LO only reaches the PA once the BB/MAC TX path is keyed on via
 *      PMAC (0x1d08[0]=1) + TX-OFDM-on (0x1e70[3:0]=4).
 * The full RF 0x00 word (TX-mode 0x2 + gain, over the snapshot) is written to
 * HSSI as (addr<<20)|(data&0xFFFFF); addr=0 so it's just data&0xFFFFF. */
void RtlJaguar3Device::StartCwTone(uint8_t gain) {
  std::lock_guard<std::mutex> lk(_reg_mu);
  if (_cw_active)
    return;

  constexpr uint16_t HSSI_WR_A = 0x1808;         /* path-A HSSI RF-write port */
  const uint16_t rf58 = 0x3c00 + (0x58 << 2);    /* RF 0x58 via direct window */
  const bool is_2g = _channel.Channel <= 14;

  /* Disable CCA so the bare carrier isn't gated by energy detection. (Pausing
   * MAC TX / disabling CCK TX here was tried and BREAKS the RF 0x00 HSSI write —
   * it re-drives RF 0x00 back to RX mode — so it's deliberately omitted.) */
  if (is_2g) {
    _device.phy_set_bb_reg(0x1a9c, 1u << 20, 0x0);
    _device.phy_set_bb_reg(0x1a14, 0x300, 0x3);
  }
  _device.phy_set_bb_reg(0x1d58, 0xff8, 0x1ff); /* OFDM CCA off */

  /* Key the BB/MAC TX path: PMAC on + TX OFDM on (phydm_set_pmac_txon_jgr3). */
  _device.phy_set_bb_reg(0x1d08, 1u << 0, 0x1);
  _device.phy_set_bb_reg(0x1e70, 0xf, 0x0);
  _device.phy_set_bb_reg(0x1e70, 0xf, 0x4);

  /* Hold the BB in continuous TX (phydm_start_ofdm_cont_tx_jgr3). Without this
   * the BB stays in RX and its RF state machine re-drives RF 0x00 back to RX
   * mode within ~ms, so the manual TX-mode write below doesn't persist and no
   * carrier comes out. */
  _device.phy_set_bb_reg(0x1c3c, 1u << 0, 0x1); /* OFDM block on */
  _device.phy_set_bb_reg(0x1a00, 0x3, 0x0);     /* CCK test mode off */
  _device.phy_set_bb_reg(0x1a00, 1u << 3, 0x1); /* scramble on */
  _device.phy_set_bb_reg(0x1ca4, 0x7, 0x1);     /* continuous TX on */

  /* Snapshot RF 0x00 (read the direct-window shadow — the correct read path). */
  _cw_rf00 = _device.rtw_read<uint32_t>(0x3c00) & 0x000fffff;

  /* RF 0x00 -> TX mode (0x00[19:16]=2) + gain (0x00[4:0]) via the HSSI port. */
  const uint32_t rf0 =
      (_cw_rf00 & ~0xf0000u & ~0x1fu) | (0x2u << 16) | (gain & 0x1f);
  _device.rtw_write<uint32_t>(HSSI_WR_A, rf0 & 0x000fffff);

  /* RF LO enable (0x58 bit1) via the direct window. */
  _device.phy_set_bb_reg(rf58, 1u << 1, 0x1);

  _cw_active = true;
  _logger->info("CW single-tone armed @ ch{} gain={} ({})", _channel.Channel,
                static_cast<int>(gain & 0x1f),
                _variant == jaguar3::ChipVariant::C8822E ? "8822e" : "8822c");
}

/* Mirror of the vendor STOP path: disable the LO, restore RF 0x00 (HSSI),
 * un-key the TX path, re-enable CCA. Serializes on _reg_mu (runs while the coex
 * thread is active). */
void RtlJaguar3Device::StopCwTone() {
  std::lock_guard<std::mutex> lk(_reg_mu);
  if (!_cw_active)
    return;

  constexpr uint16_t HSSI_WR_A = 0x1808;
  const uint16_t rf58 = 0x3c00 + (0x58 << 2);
  const bool is_2g = _channel.Channel <= 14;

  _device.phy_set_bb_reg(rf58, 1u << 1, 0x0);                 /* LO off */
  _device.rtw_write<uint32_t>(HSSI_WR_A, _cw_rf00 & 0x000fffff); /* restore RF 0x00 */
  /* Stop continuous TX (phydm_stop_ofdm_cont_tx_jgr3): test modes off, then BB
   * reset. */
  _device.phy_set_bb_reg(0x1ca4, 0x7, 0x0);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  _device.phy_set_bb_reg(0x1d0c, 1u << 16, 0x0);
  _device.phy_set_bb_reg(0x1d0c, 1u << 16, 0x1);
  _device.phy_set_bb_reg(0x1e70, 0xf, 0x0);     /* TX OFDM off */
  _device.phy_set_bb_reg(0x1d08, 1u << 0, 0x0); /* PMAC off */
  _device.phy_set_bb_reg(0x1d58, 0xff8, 0x0);   /* OFDM CCA on */
  if (is_2g) {
    _device.phy_set_bb_reg(0x1a9c, 1u << 20, 0x1);
    _device.phy_set_bb_reg(0x1a14, 0x300, 0x0);
  }

  _cw_active = false;
  _logger->info("CW single-tone stopped — chip restored");
}

/* Modulated continuous TX (Jaguar3). The JGR3 hardware-continuous hold
 * (0x1ca4) only engages under the full PMAC packet-generator setup, which emits
 * a test pattern rather than decodable frames — no good for a link probe that
 * needs per-frame SNR/EVM. So the HW 100%-duty path is NOT engaged here; we
 * apply the rate and let the caller's back-to-back send_packet loop supply the
 * modulated stimulus at beacon duty. True HW continuous is a documented
 * follow-up (needs the PMAC TX-packet config). */
void RtlJaguar3Device::StartContinuousTx(const devourer::TxMode &mode) {
  SetTxMode(mode);
  std::lock_guard<std::mutex> lk(_reg_mu);
  if (_cont_active)
    return;

  /* --- Stop the normal TRX (phydm_stop_ic_trx) so the PMAC can drive TX --- */
  _cont_txpause = _device.rtw_read8(0x522);        /* save TX-queue bitmap */
  _device.rtw_write8(0x522, 0xff);                 /* pause all TX queues */
  _device.phy_set_bb_reg(0x1d58, 0xff8, 0x1ff);    /* disable OFDM RX CCA */
  _cont_ccktx = _device.rtw_read<uint32_t>(0x1a04) & 0xf0000000u; /* save */
  _device.phy_set_bb_reg(0x1a14, 0x300, 0x3);      /* CCK RxIQ weight = 0 */
  _device.phy_set_bb_reg(0x1a04, 0xf0000000, 0x0); /* disable CCK Tx */

  /* --- Start OFDM continuous TX (phydm_start_ofdm_cont_tx_jgr3) --- */
  _device.phy_set_bb_reg(0x1c3c, 1u << 0, 0x1);    /* OFDM block on */
  _device.phy_set_bb_reg(0x1a00, 0x3, 0x0);        /* CCK test mode off */
  _device.phy_set_bb_reg(0x1a00, 1u << 3, 0x1);    /* scrambler on */
  _device.phy_set_bb_reg(0x1ca4, 0x7, 0x1);        /* continuous TX hold on */

  /* --- Define the PMAC packet: legacy 6M OFDM (phydm_set_sig_jgr3 +
   * phydm_set_mac_phy_txinfo_jgr3). L-SIG bytes {0x0b,0x7d,0x02} = rate 6M,
   * length 1000 B, parity — the vendor's hardcoded 6M packet. --- */
  _device.phy_set_bb_reg(0x1eb4, 0xfffff, 0x1);    /* packet_count = 1 */
  _device.phy_set_bb_reg(0x908, 0xffffff, 0x00027d0b); /* L-SIG (6M) */
  _device.phy_set_bb_reg(0xa58, 0x003f8000, 0x04); /* tx_rate = ODM_RATE6M */
  _device.phy_set_bb_reg(0x900, 1u << 1, 0x0);     /* ndp_sound = 0 */
  _device.phy_set_bb_reg(0x900, 0xff000000, 0x0);  /* txsc/bw/stbc = 0 (20 MHz) */
  _device.phy_set_bb_reg(0x1ae0, 0x7000, 0x0);     /* tx_sc = duplicate */
  _device.phy_set_bb_reg(0x900, 1u << 0, 0x0);     /* not HT */
  _device.phy_set_bb_reg(0x900, 1u << 2, 0x0);     /* not VHT (legacy) */
  _device.phy_set_bb_reg(0x9b8, 0xffff0000, 2000); /* packet period ~500us */

  /* --- Turn on PMAC + TX-OFDM (phydm_set_pmac_txon_jgr3) --- */
  _device.phy_set_bb_reg(0x1d08, 1u << 0, 0x1);    /* PMAC on */
  _device.phy_set_bb_reg(0x1e70, 0xf, 0x0);
  _device.phy_set_bb_reg(0x1e70, 0xf, 0x4);        /* TX OFDM on */

  _cont_active = true;
  _logger->info("Modulated continuous TX armed @ ch{} (Jaguar3 PMAC 6M HW "
                "100%%-duty carrier; idle-hold, StopContinuousTx to end)",
                _channel.Channel);
}

void RtlJaguar3Device::StopContinuousTx() {
  std::lock_guard<std::mutex> lk(_reg_mu);
  if (!_cont_active)
    return;

  /* Stop OFDM continuous (phydm_stop_ofdm_cont_tx_jgr3). */
  _device.phy_set_bb_reg(0x1ca4, 0x7, 0x0);        /* continuous hold off */
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  _device.phy_set_bb_reg(0x1d0c, 1u << 16, 0x0);   /* BB reset pulse */
  _device.phy_set_bb_reg(0x1d0c, 1u << 16, 0x1);
  _device.phy_set_bb_reg(0x1e70, 0xf, 0x0);        /* TX OFDM off */
  _device.phy_set_bb_reg(0x1d08, 1u << 0, 0x0);    /* PMAC off */

  /* Revert the TRX stop (phydm_stop_ic_trx REVERT). */
  _device.rtw_write8(0x522, _cont_txpause);        /* release TX queues */
  _device.phy_set_bb_reg(0x1d58, 0xff8, 0x0);      /* re-enable OFDM RX CCA */
  _device.phy_set_bb_reg(0x1a14, 0x300, 0x0);      /* restore CCK RxIQ */
  _device.phy_set_bb_reg(0x1a04, 0xf0000000, _cont_ccktx); /* restore CCK Tx */

  _cont_active = false;
  _logger->info("Modulated continuous TX stopped — chip restored");
}

/* Frame-free RX energy snapshot for the Jaguar3 (8822C/E) BB. Ported from
 * phydm_fa_cnt_statistics_jgr3 + phydm_reset_bb_hw_cnt (reference/rtl88x2cu
 * phydm_dig.c / phydm_api.c): the newer BB holds the same FA/CCA facilities as
 * the AC classic map, just at different addresses. CCA (0x2c08) is the primary
 * channel-busy signal (a CW tone spikes OFDM CCA); OFDM FA is the vendor sum of
 * the sub-counters. Read-then-reset for a per-call delta; serialized on _reg_mu
 * so it does not race the coex thread's register access. */
RxEnergy RtlJaguar3Device::GetRxEnergy() {
  std::lock_guard<std::mutex> lk(_reg_mu);
  RxEnergy e;
  auto rd = [this](uint16_t addr) { return _device.rtw_read<uint32_t>(addr); };

  const uint32_t cca = rd(0x2c08);
  e.cca_ofdm = (cca >> 16) & 0xffff;
  e.cca_cck = cca & 0xffff;
  e.fa_cck = rd(0x1a5c) & 0xffff;
  /* OFDM FA = parity + rate-illegal + crc8 + mcs + fast-fsync + sb-search +
   * mcs-vht + crc8-vhta (phydm_fa_cnt_statistics_jgr3). */
  const uint32_t r2d04 = rd(0x2d04), r2d08 = rd(0x2d08), r2d10 = rd(0x2d10),
                 r2d20 = rd(0x2d20), r2d0c = rd(0x2d0c);
  e.fa_ofdm = ((r2d04 >> 16) & 0xffff) + (r2d08 & 0xffff) +
              ((r2d08 >> 16) & 0xffff) + (r2d10 & 0xffff) + (r2d20 & 0xffff) +
              ((r2d20 >> 16) & 0xffff) + ((r2d10 >> 16) & 0xffff) +
              (r2d0c & 0xffff);
  e.valid_fa = true;
  e.igi = static_cast<uint8_t>(rd(0x1d70) & 0x7f);
  e.valid_igi = true;

  /* NHM 12-bucket power histogram (frame-free, JGR3 register map). Runs its own
   * ~2 ms measurement window before the FA-counter reset below (0x1eb4[25] also
   * clears BB HW counters). Holds _reg_mu across the short wait — tolerable at
   * the emitter's >=100 ms cadence vs the coex thread's ~2 s tick. */
  devourer::read_nhm(
      devourer::nhm_regs_jgr3(), e.igi, rd,
      [this](uint16_t a, uint32_t m, uint32_t v) {
        _device.phy_set_bb_reg(a, m, v);
      },
      e);

  /* Reset: CCK FA 0x1a2c[15:14] 0->2, CCK CCA 0x1a2c[13:12] 0->2, then OFDM
   * CCA/FA (phydm_reset_bb_hw_cnt jgr3: 0x1eb4[25] 1->0, wrapped by the
   * 0x1d2c[31] rx-clk-gate disable/enable). */
  _device.phy_set_bb_reg(0x1a2c, 0x3u << 14, 0x0);
  _device.phy_set_bb_reg(0x1a2c, 0x3u << 14, 0x2);
  _device.phy_set_bb_reg(0x1a2c, 0x3u << 12, 0x0);
  _device.phy_set_bb_reg(0x1a2c, 0x3u << 12, 0x2);
  _device.phy_set_bb_reg(0x1d2c, 1u << 31, 0x0);
  _device.phy_set_bb_reg(0x1eb4, 1u << 25, 0x1);
  _device.phy_set_bb_reg(0x1eb4, 1u << 25, 0x0);
  _device.phy_set_bb_reg(0x1d2c, 1u << 31, 0x1);
  return e;
}

/* Disable / restore the MAC EDCCA energy-detect gate (the vendor dis_cca proc's
 * MAC half: BIT_DIS_EDCCA 0x520[15] + BIT_EDCCA_MSK_COUNTDOWN 0x524[11]). Caller
 * holds _reg_mu.
 *
 * The vendor recipe ALSO writes three BB registers (0x1a9c[20], 0x1a14[9:8],
 * 0x1d58[0xff8]); those are deliberately NOT done here. 0x1d58[0xff8]=0x1ff is
 * the OFDM-CCA-off write (the CW-tone path uses it to stop OFDM detection for a
 * bare carrier), so applying it makes the RX deaf to OFDM — MEASURED: the full
 * recipe dropped 8822EU delivery from ~6800 to ~10 frames. The MAC EDCCA bit is
 * the only part that's safe to touch on a live RX. See docs / the help-wanted
 * issue for the measured null and why (EDCCA gates TX deferral, which devourer's
 * monitor inject already bypasses). */
void RtlJaguar3Device::apply_cca_mode_locked(bool disabled) {
  uint32_t v520 = _device.rtw_read<uint32_t>(0x0520);
  uint32_t v524 = _device.rtw_read<uint32_t>(0x0524);
  if (disabled) {
    v520 |= (1u << 15);
    v524 &= ~(1u << 11);
  } else {
    v520 &= ~(1u << 15);
    v524 |= (1u << 11);
  }
  _device.rtw_write<uint32_t>(0x0520, v520);
  _device.rtw_write<uint32_t>(0x0524, v524);
}

void RtlJaguar3Device::SetCcaMode(bool disabled) {
  std::lock_guard<std::mutex> lk(_reg_mu);
  _cca_disabled = disabled;
  if (_brought_up)
    apply_cca_mode_locked(disabled);
  _logger->info("Jaguar3: MAC EDCCA {}", disabled ? "DISABLED (dis_cca)"
                                                   : "enabled (default)");
}

void RtlJaguar3Device::SetMonitorChannel(SelectedChannel channel) {
  /* Serialize against the coex thread's housekeeping tick (and any concurrent
   * FastRetune) — channel config is register RMW. Init/InitWrite call the
   * radio-management core directly (no lock needed: the coex thread isn't
   * running yet), so locking here cannot self-deadlock. */
  std::lock_guard<std::mutex> lk(_reg_mu);
  const bool ch_changed = channel.Channel != _channel.Channel;
  _channel = channel;
  _radioManagement.set_channel_bwmode(channel.Channel, channel.ChannelOffset,
                                      channel.ChannelWidth);
  /* Runtime TX-power knobs in use: re-fold them against the NEW channel
   * group's efuse refs (8822E bases are per-group). Gated on a knob being
   * active so the legacy no-knob path stays byte-identical. */
  if (ch_changed)
    _pwr_ref_valid = false;
  if (_brought_up && (_tx_pwr_offset_steps != 0 || _tx_pwr_override >= 0))
    apply_tx_power_current(/*full=*/true);
  /* dis_cca is sticky — the channel set rewrote the BB CCA registers, so
   * re-assert the disable if it was armed. */
  if (_brought_up && _cca_disabled)
    apply_cca_mode_locked(true);
}

void RtlJaguar3Device::FastRetune(uint8_t channel, bool cache_rf) {
  std::lock_guard<std::mutex> lk(_reg_mu);
  if (channel == _channel.Channel)
    return;
  if (_radioManagement.fast_retune(channel, _channel.ChannelOffset,
                                   _channel.ChannelWidth, cache_rf)) {
    _channel.Channel = channel;
    return;
  }
  /* Fast path declined (band change / never tuned) — full channel set at the
   * current bandwidth + offset, under the same lock (the core is unlocked). */
  _channel.Channel = channel;
  _radioManagement.set_channel_bwmode(channel, _channel.ChannelOffset,
                                      _channel.ChannelWidth);
}

void RtlJaguar3Device::FastSetBandwidth(ChannelWidth_t bw) {
  std::lock_guard<std::mutex> lk(_reg_mu);
  if (bw == _channel.ChannelWidth)
    return;
  auto in_set = [](ChannelWidth_t b) {
    return b == CHANNEL_WIDTH_20 || b == CHANNEL_WIDTH_5 ||
           b == CHANNEL_WIDTH_10;
  };
  /* Only a same-channel toggle within {20, 5, 10} — a 40/80 endpoint moves the
   * RF bandwidth, needing the full tune. */
  if (in_set(bw) && in_set(_channel.ChannelWidth) &&
      _radioManagement.fast_set_bandwidth(bw)) {
    _channel.ChannelWidth = bw;
    return;
  }
  /* Fast path declined (40/80 endpoint, cold radio) — full channel set, under
   * the same lock (the core is unlocked). */
  _radioManagement.set_channel_bwmode(_channel.Channel, _channel.ChannelOffset,
                                      bw);
  _channel.ChannelWidth = bw;
}

/* Re-program TXAGC from the current knob state (see header). The 0x41e8
 * TX+RX quirk is 8822E-specific, so the skip flag is derived here — once —
 * from _rx_wanted AND the variant; the 8822C keeps its path-B ref writes. */
void RtlJaguar3Device::apply_tx_power_current(bool full) {
  const int off = _tx_pwr_offset_steps;
  const int flat = _tx_pwr_override;
  const bool skip_b =
      _rx_wanted && _variant == jaguar3::ChipVariant::C8822E;
  _txpwr_sat_low = false;
  _txpwr_sat_high = false;
  auto clamp127 = [&](int v) -> uint8_t {
    if (v < 0) {
      v = 0;
      _txpwr_sat_low = true;
    }
    if (v > 0x7f) {
      v = 0x7f;
      _txpwr_sat_high = true;
    }
    return static_cast<uint8_t>(v);
  };

  if (flat >= 0) {
    /* Flat semantics: reference = flat + offset, per-rate diffs zeroed (once —
     * offset-only steps skip the 32-dword re-zero). */
    _radioManagement.set_tx_power_ref(clamp127(flat + off),
                                      /*zero_diffs=*/!_diffs_zeroed, skip_b);
    _diffs_zeroed = true;
    return;
  }

  if (_variant == jaguar3::ChipVariant::C8822E) {
    /* Default TX power (8822e). The TXAGC reference base is the per-channel
     * power index the kernel programs, from the efuse power-by-rate table
     * (logical 0x22 path A / 0x4C path B, per channel-group); when the byte is
     * valid (<= txgi_max 127) it is used verbatim for kernel-faithful
     * per-channel power. On bare modules the byte is unprogrammed (0xFF) and
     * devourer falls back to JAGUAR3_TXPWR_REF_BASE — an empirically
     * on-air-matched default that is not below the kernel's. The per-rate
     * diffs come from the BB phy_reg_pg table (apply_power_by_rate_8822e);
     * full TSSI-offset power-by-rate is a follow-up. The derived base refs
     * are cached so an offset-only step is just the light ref writes. */
    if (full || !_pwr_ref_valid) {
      uint8_t efuse_a = 0xFF, efuse_b = 0xFF;
      _hal.read_efuse_txpwr_base_8822e(_channel.Channel, efuse_a, efuse_b);
      _pwr_ref_a = (efuse_a <= 127) ? efuse_a : JAGUAR3_TXPWR_REF_BASE;
      _pwr_ref_b = (efuse_b <= 127) ? efuse_b : JAGUAR3_TXPWR_REF_BASE;
      _pwr_ref_valid = true;
    }
    const uint8_t ra = clamp127(static_cast<int>(_pwr_ref_a) + off);
    const uint8_t rb = clamp127(static_cast<int>(_pwr_ref_b) + off);
    if (full || _diffs_zeroed) {
      /* Full apply: refs + the per-rate diff walk (also the path back from
       * flat semantics, which zeroed the diffs). */
      _radioManagement.apply_power_by_rate_8822e(_channel.Channel, ra, rb,
                                                 skip_b);
      _diffs_zeroed = false;
      if (skip_b)
        _logger->info("Jaguar3(8822e): TX+RX mode — path-B OFDM TXAGC ref left "
                      "at table default (keeps RX alive)");
    } else {
      _radioManagement.apply_tx_power_refs_8822e(ra, rb, skip_b);
    }
    return;
  }

  /* 8822c (CU): preserve the flat reference the demo used to impose (40) so
   * the SDR-validated CU TX power is unchanged. The CU's efuse-calibrated
   * per-rate default is a follow-up; the offset shifts this reference. */
  _radioManagement.set_tx_power_ref(
      clamp127(static_cast<int>(JAGUAR3_TXPWR_REF_BASE_8822C) + off),
      /*zero_diffs=*/!_diffs_zeroed, skip_b);
  _diffs_zeroed = true;
}

devourer::TxPowerCaps RtlJaguar3Device::GetTxPowerCaps() {
  devourer::TxPowerCaps caps;
  caps.supported = true;
  caps.index_max = 127;
  caps.step_qdb = 1; /* 0.25 dB per reference step */
  /* On-air slope (tests/txpwr_offset_onair.sh): 8822CU 0.325 dB/idx @ ch36
   * (classifies at the 0.25 nominal, rms 0.60). The 8822EU measures ~0.55
   * dB/idx with an accelerating curve (0.3 low range -> 0.9 around ref 72) —
   * its TSSI/kfree trims reshape the ref->power transfer, so one reference
   * step is NOT a constant 0.25 dB there and the measured flag stays false:
   * a controller should calibrate its own dB-per-step on the E (or lean on
   * GetTxPowerState readback + the ground's RSSI). */
  caps.step_measured = _variant == jaguar3::ChipVariant::C8822C;
  caps.offset_min_qdb = -127;
  caps.offset_max_qdb = 127;
  return caps;
}

int RtlJaguar3Device::SetTxPowerOffsetQdb(int qdb) {
  if (_cw_active) {
    _logger->warn("SetTxPowerOffsetQdb refused: CW tone active (TXAGC does "
                  "not modulate a bare LO carrier)");
    return 0;
  }
  int steps = 0;
  const int applied = devourer::quantize_offset_qdb(qdb, GetTxPowerCaps(),
                                                    &steps);
  _tx_pwr_offset_steps = steps;
  if (_brought_up) {
    std::lock_guard<std::mutex> lk(_reg_mu);
    apply_tx_power_current(/*full=*/false);
  }
  _logger->info("TX-power offset: {} qdB requested -> {} qdB applied "
                "({} steps){}",
                qdb, applied, steps,
                _brought_up ? "" : " (recorded; applies at bring-up)");
  return applied;
}

void RtlJaguar3Device::SetTxPowerIndexOverride(int idx) {
  if (_cw_active) {
    _logger->warn("SetTxPowerIndexOverride refused: CW tone active");
    return;
  }
  _tx_pwr_override = idx < 0 ? -1 : (idx > 0x7f ? 0x7f : idx);
  if (_brought_up) {
    std::lock_guard<std::mutex> lk(_reg_mu);
    /* Clearing back to the efuse baseline needs the full apply (the flat
     * override zeroed the 8822E per-rate diffs). */
    apply_tx_power_current(/*full=*/idx < 0);
  }
}

bool RtlJaguar3Device::ReApplyTxPower() {
  if (!_brought_up || _cw_active)
    return false;
  std::lock_guard<std::mutex> lk(_reg_mu);
  apply_tx_power_current(/*full=*/true);
  return true;
}

devourer::TxCaps RtlJaguar3Device::GetTxCaps() {
  return devourer::tx_caps_for_chains(2); /* 8822C/8822E are 2T2R */
}

int RtlJaguar3Device::SetXtalCap(int cap) {
  /* phydm_set_crystal_cap_reg (8822C/8822E): a 7-bit code into
   * 0x1040[23:17] AND 0x1040[16:10] (Xo and Xi), reg = cap | (cap << 7).
   * cap < 0 reverts to the default (efuse readback is the 2-byte 8822C
   * format, not wired here — fall back to 0x20). */
  const uint8_t c = cap < 0 ? 0x20 : static_cast<uint8_t>(cap & 0x7F);
  const uint32_t reg = static_cast<uint32_t>(c) | (static_cast<uint32_t>(c) << 7);
  std::lock_guard<std::mutex> lk(_reg_mu);
  _device.phy_set_bb_reg(0x1040, 0x00FFFC00, reg);
  _xtal_cap = c;
  _logger->info("Jaguar3: crystal-cap set to 0x{:02x}{}", c,
                cap < 0 ? " (default)" : "");
  return c;
}

devourer::AdapterCaps RtlJaguar3Device::GetAdapterCaps() {
  devourer::AdapterCaps c;
  c.supported = true;
  c.generation = devourer::ChipGeneration::Jaguar3;
  c.transport = _device.is_usb() ? "usb" : "pcie";
  c.tx = GetTxCaps();
  c.txpwr = GetTxPowerCaps();
  c.tx_chains = 2; /* 8822C/8822E are 2T2R */
  c.rx_chains = 2;
  c.per_chain_rssi = true;
  c.bw_mask = devourer::bw_mask_for_generation(c.generation);
  c.fastretune_ok = true;
  c.narrowband_ok = true; /* 5/10 MHz baseband re-clock — Jaguar3 only */
  c.xtal_cap_max = 0x7f;   /* 7-bit AFE crystal-cap trim (0x1040) */
  c.xtal_cap_default = 0x20;
  devourer::set_standard_freq_ranges(c);

  if (_variant == jaguar3::ChipVariant::C8822E) {
    c.chip_name = "RTL8822E";
    c.marketing_names = "RTL8812EU/RTL8822EU";
    c.chip_id = 0x17;
    c.variant = "C8822E";
  } else {
    c.chip_name = "RTL8822C";
    c.marketing_names = "RTL8812CU/RTL8822CU";
    c.chip_id = 0x13;
    c.variant = "C8822C";
  }
  return c;
}

devourer::EfuseStability RtlJaguar3Device::ProbeEfuseStability(int reads) {
  if (!_brought_up)
    return {}; /* supported=false — the efuse read needs a powered chip */
  std::lock_guard<std::mutex> lk(_reg_mu); /* serialize vs the coex tick */
  constexpr uint16_t kMapLen = 0x100;      /* sizeof(HalJaguar3::_efuse_cache) */
  auto st = devourer::ProbeEfuseStabilityImpl(
      [this](uint8_t *buf) { return _hal.probe_efuse_map(buf, kMapLen); },
      kMapLen, reads);
  if (st.reads == 0)
    return {}; /* 8822E: probe refused (OTP unreliable post-bring-up) */
  _logger->info(
      "efuse-stability: reads={} mismatched={} invalid_id={} id=0x{:04x}",
      st.reads, st.mismatched_reads, st.invalid_id_reads, st.eeprom_id);
  return st;
}

devourer::TxPowerState RtlJaguar3Device::GetTxPowerState() {
  devourer::TxPowerState s;
  s.valid = true;
  s.flat_index = static_cast<int16_t>(_tx_pwr_override.load());
  s.offset_steps = static_cast<int16_t>(_tx_pwr_offset_steps.load());
  s.offset_qdb = s.offset_steps; /* 1 qdB per step on Jaguar3 */
  s.saturated_low = _txpwr_sat_low;
  s.saturated_high = _txpwr_sat_high;
  if (_brought_up && !_cw_active) {
    /* Reference readback (the Jaguar3 TXAGC refs ARE readable): OFDM ref
     * 0x18e8[16:10]; MCS7 is the per-rate diff table's zero anchor, so it
     * equals the OFDM ref; CCK ref 0x18a0[22:16]. Under _reg_mu for a
     * coherent snapshot vs the coex tick. */
    std::lock_guard<std::mutex> lk(_reg_mu);
    const uint32_t ofdm = (_device.rtw_read32(0x18e8) >> 10) & 0x7f;
    s.ofdm_index = static_cast<int16_t>(ofdm);
    s.mcs7_index = static_cast<int16_t>(ofdm);
    s.cck_index =
        static_cast<int16_t>((_device.rtw_read32(0x18a0) >> 16) & 0x7f);
    s.hw_readback = true;
  }
  return s;
}

devourer::ThermalStatus RtlJaguar3Device::GetThermalStatus() {
  devourer::ThermalStatus t;
  if (!_brought_up || _cw_active)
    return t; /* RF reads need a live, non-CW-held chip */
  uint8_t raw = 0, baseline = 0xFF;
  bool ok;
  {
    /* The meter trigger is an RF 0x42 RMW — the same register the coex
     * tick's pwr_track toggles. */
    std::lock_guard<std::mutex> lk(_reg_mu);
    ok = _hal.read_thermal(raw, baseline);
  }
  t.raw = raw;
  t.baseline = baseline;
  t.valid = ok && baseline != 0xFF;
  t.delta = t.valid ? static_cast<int>(raw) - static_cast<int>(baseline) : 0;
  return t;
}

bool RtlJaguar3Device::send_packet(const uint8_t *packet, size_t length) {
  /* The coex runtime thread (coex_runtime_loop) drives the periodic coex
   * decision + FW heartbeats + C2H draining, so the TX hot path stays lean. */
  /* Parse the radiotap header (same fields the Jaguar1 path reads) and build an
   * 8822C TX descriptor + synchronous bulk-OUT. The TX path is enabled during
   * rtw_hal_init (3-wire RF + DACK + bf_init + enable_tx_path), so frames go
   * on-air at the tuned channel. */
  if (length < sizeof(struct ieee80211_radiotap_header))
    return false;
  uint16_t radiotap_length = get_unaligned_le16(packet + 2);
  if (radiotap_length == 0 || static_cast<size_t>(radiotap_length) >= length)
    return false;
  const size_t frame_len = length - radiotap_length;

  uint8_t fixed_rate = MGN_1M;
  uint8_t sgi = 0, ldpc = 0, stbc = 0;
  ChannelWidth_t bwidth = CHANNEL_WIDTH_20;
  bool vht = (radiotap_length != 0x0d);
  bool rate_from_radiotap = false; /* did the frame's radiotap carry a rate? */
  /* Per-packet hop target from a radiotap CHANNEL field (0 = none present). */
  int radiotap_channel = 0;

  auto *rtap_hdr = reinterpret_cast<struct ieee80211_radiotap_header *>(
      const_cast<uint8_t *>(packet));
  struct ieee80211_radiotap_iterator it;
  int ret = ieee80211_radiotap_iterator_init(&it, rtap_hdr, radiotap_length,
                                             nullptr);
  while (!ret) {
    ret = ieee80211_radiotap_iterator_next(&it);
    if (ret)
      continue;
    switch (it.this_arg_index) {
    case IEEE80211_RADIOTAP_RATE:
      fixed_rate = *it.this_arg;
      rate_from_radiotap = true;
      break;
    case IEEE80211_RADIOTAP_CHANNEL:
      /* 2 x __le16: frequency (MHz), then flags. Frequency is authoritative
       * for the per-packet hop target; flags are ignored (rate/BW come from
       * the RATE/MCS/VHT fields). Same contract as the Jaguar1 path. */
      radiotap_channel =
          devourer::freq_to_chan(get_unaligned_le16(it.this_arg));
      break;
    case IEEE80211_RADIOTAP_MCS: {
      uint8_t mcs_known = it.this_arg[0];
      uint8_t mcs_flags = it.this_arg[1];
      if ((mcs_flags & IEEE80211_RADIOTAP_MCS_BW_MASK) ==
          IEEE80211_RADIOTAP_MCS_BW_40)
        bwidth = CHANNEL_WIDTH_40;
      sgi = (mcs_flags & 0x04) ? 1 : 0;
      if (mcs_known & IEEE80211_RADIOTAP_MCS_HAVE_MCS) {
        uint8_t idx = it.this_arg[2];
        if (idx <= 31) {
          fixed_rate = MGN_MCS0 + idx;
          rate_from_radiotap = true;
        }
      }
    } break;
    case IEEE80211_RADIOTAP_VHT: {
      uint8_t known = it.this_arg[0];
      uint8_t flags = it.this_arg[2];
      if ((known & 4) && (flags & 4))
        sgi = 1;
      if ((known & 1) && (flags & 1))
        stbc = 1;
      if (known & 0x40) {
        auto bw = it.this_arg[3] & 0x1f;
        if (bw >= 1 && bw <= 3)
          bwidth = CHANNEL_WIDTH_40;
        else if (bw >= 4 && bw <= 10)
          bwidth = CHANNEL_WIDTH_80;
      }
      if (it.this_arg[8] & 1)
        ldpc = 1;
      unsigned mcs = (it.this_arg[4] >> 4) & 0x0f;
      unsigned nss = it.this_arg[4] & 0x0f;
      if (nss > 0) {
        if (nss > 4)
          nss = 4;
        if (mcs > 9)
          mcs = 9;
        fixed_rate = MGN_VHT1SS_MCS0 + ((nss - 1) * 10 + mcs);
        rate_from_radiotap = true;
      }
    } break;
    default:
      break;
    }
  }

  /* Radiotap CHANNEL is authoritative for per-packet frequency, exactly as
   * RATE/MCS are for rate: a frame asking for a different channel triggers a
   * lean FastRetune before the descriptor is built (so the 40-in-80 DATA_SC
   * below keys off up-to-date channel state). FastRetune takes _reg_mu for the
   * retune only — the TX hot path stays lock-free when not hopping. */
  if (radiotap_channel > 0 && radiotap_channel != _channel.Channel)
    FastRetune(static_cast<uint8_t>(radiotap_channel), /*cache_rf=*/true);

  /* Radiotap carried no rate -> apply the runtime TX-mode default (SetTxMode).
   * Without this a rate-less frame (e.g. the demo's beacon) falls back to
   * MGN_1M, so an "MCS7" flood silently went on-air at 1 Mbps. Per-packet
   * radiotap always wins. Ported from RtlJaguarDevice (was Jaguar1-only). */
  if (!rate_from_radiotap && _tx_mode_default.has_value()) {
    const devourer::TxParams tp = devourer::tx_mode_to_params(*_tx_mode_default);
    fixed_rate = tp.fixed_rate;
    vht = tp.vht;
    sgi = tp.sgi ? 1 : 0;
    ldpc = tp.ldpc ? 1 : 0;
    stbc = tp.stbc ? 1 : 0;
    bwidth = static_cast<ChannelWidth_t>(tp.bwidth);
  }

  /* DEVOURER_TX_NDPA=N — beamforming-sounding probe: mark injected frames as
   * NDPA so the armed sounding engine (DEVOURER_BF_ARM_SOUNDER, InitWrite)
   * follows each with a hardware NDP. N is the PERIOD: 1 (or non-numeric) =
   * every frame (self-sounding capture, the original behaviour); N>1 = every Nth
   * frame, so the interleaved data frames stay steerable for TX beamforming. */
  const int ndpa_period = _cfg.bf.ndpa_period;
  bool ndpa = false;
  if (ndpa_period > 0) {
    ndpa = (_ndpa_ctr % static_cast<uint64_t>(ndpa_period)) == 0;
    ++_ndpa_ctr;
  }
  /* Periodic sounding (N>1, the TX-BF case) needs a 2-stream NDP to fill the
   * 2-antenna V matrix, so force those NDPA frames to VHT-2SS regardless of the
   * data rate (measured: a 1SS NDP yields no CBR); the interleaved data frames
   * keep their radiotap rate (1SS/HT), which is what gets steered. N==1
   * (every-frame self-sounding capture) keeps the caller's rate unchanged. */
  if (ndpa && ndpa_period > 1) {
    fixed_rate = MGN_VHT2SS_MCS0;
    vht = true;
  }

  uint8_t bw_desc = (bwidth == CHANNEL_WIDTH_40)   ? 1
                    : (bwidth == CHANNEL_WIDTH_80) ? 2
                                                   : 0;
  uint8_t rate_id = vht ? 9 : 8;

  /* 40-in-80: a 40 MHz frame on an 80 MHz-configured channel needs a data
   * sub-channel telling the PHY which 40 MHz half to use — the lower 40 (which
   * carries the primary), VHT_DATA_SC_40_LOWER_OF_80MHZ = 10. The 40 MHz
   * waveform then rides the 80 MHz RF path and stays decodable by a standard
   * HT40 receiver on the lower-40 channel (the `iw 80MHz` + 40 MHz-radiotap
   * equivalent). */
  uint8_t data_sc = 0;
  if (_channel.ChannelWidth == CHANNEL_WIDTH_80 && bwidth == CHANNEL_WIDTH_40)
    data_sc = 10; /* VHT_DATA_SC_40_LOWER_OF_80MHZ */

  /* Group-address (broadcast/multicast) detection: addr1 is the first MAC
   * address in the 802.11 header (after the 2-byte FC + 2-byte duration). Its
   * I/G bit (bit0 of the first octet) marks a group address; set BMC to match
   * the kernel's descriptor for group-addressed frames. */
  const uint8_t *dot11 = packet + radiotap_length;
  bool bmc = frame_len >= 6 && (dot11[4] & 0x01);
  /* STBC guard (IRtlDevice contract) — 8822C/8822E are 2T2R so this never
   * fires today, but keeps the invariant uniform across families: never air an
   * STBC frame the chip can't do. */
  if (stbc && !GetTxCaps().stbc_ok)
    stbc = 0;
  std::vector<uint8_t> usb_frame(jaguar3::TXDESC_SIZE_8822C + frame_len, 0);
  jaguar3::fill_data_tx_desc_8822c(
      usb_frame.data(), static_cast<uint16_t>(frame_len),
      MRateToHwRate(fixed_rate), rate_id, bw_desc, sgi != 0, ldpc != 0, stbc,
      bmc, ndpa, data_sc);
  std::memcpy(usb_frame.data() + jaguar3::TXDESC_SIZE_8822C,
              packet + radiotap_length, frame_len);

  /* Synchronous, bounded bulk-OUT. The async submit path (_device.send_packet)
   * never reaped its completions on the Jaguar3 TX loop (nothing calls
   * libusb_handle_events there), leaking a libusb_transfer per send. A blocking
   * transfer with a finite timeout completes-or-fails each send and stays
   * responsive to the caller's stop flag. No per-send clear_halt: resetting the
   * data-toggle on the hot path corrupts the chip's USB state machine (see
   * bulk_send_sync_ep). The caller backs off when these fail repeatedly — until
   * the TX-path enable registers are programmed the chip NAKs every frame, and
   * hammering a non-draining endpoint is exactly what wedged its USB core. */
  uint8_t tx_ep = _device.first_bulk_out_ep();
  int rc = _device.bulk_send_sync_ep(tx_ep, usb_frame.data(),
                                     usb_frame.size(), /*timeout_ms=*/20);
  return rc >= 0;
}

SelectedChannel RtlJaguar3Device::GetSelectedChannel() { return _channel; }

uint64_t RtlJaguar3Device::ReadTsf() {
  /* REG_TSFTR 0x0560 (low) / 0x0564 (high); hi/lo/hi with a wrap retry. Under
   * _reg_mu (shared with the coex runtime thread). Starved to 0 under a heavy
   * RX bulk-IN flood — reliable from a quiet TX. */
  std::lock_guard<std::mutex> lk(_reg_mu);
  uint32_t hi = _device.rtw_read<uint32_t>(0x0564);
  uint32_t lo = _device.rtw_read<uint32_t>(0x0560);
  if (_device.rtw_read<uint32_t>(0x0564) != hi) {
    hi = _device.rtw_read<uint32_t>(0x0564);
    lo = _device.rtw_read<uint32_t>(0x0560);
  }
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

void RtlJaguar3Device::SetTxMode(const devourer::TxMode &mode) {
  _tx_mode_default = mode;
}
void RtlJaguar3Device::ClearTxMode() { _tx_mode_default.reset(); }
