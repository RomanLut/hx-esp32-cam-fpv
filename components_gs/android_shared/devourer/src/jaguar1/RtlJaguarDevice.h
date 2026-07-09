#ifndef RTL_JAGUAR_DEVICE_H
#define RTL_JAGUAR_DEVICE_H

#include <array>
#include <atomic>
#include <functional>
#include <iostream>
#include <iomanip>
#include <memory>
#include <optional>
#include <thread>

#include "logger.h"
#include "BbDbgportReader.h"
#include "HalModule.h"
#include "IRtlDevice.h"
#include "SelectedChannel.h"
#include "EepromManager.h"
#include "RadioManagementModule.h"
#include "FrameParser.h"
#include "TxMode.h"
#include "CfoTracker.h"

extern "C"
{
#include "ieee80211_radiotap.h"
}

/* Action_ParsedRadioPacket is declared in IRtlDevice.h (shared with the
 * Jaguar3 device and the factory). */

/* RtlJaguarDevice is the orchestrator for the Realtek "Jaguar" 802.11ac family
 * — RTL8812AU (2T2R), RTL8811AU (1T1R cut), and RTL8814AU (4T4R RF / 3-SS
 * baseband). The chip is identified at construction time via SYS_CFG bits and
 * USB PID; this class drives bring-up, RX, and TX for whichever member of the
 * family is present. */
class RtlJaguarDevice : public IRtlDevice {
  /* Declared before every component that consumes it: members initialise in
   * declaration order, and _eepromManager / _radioManagement / _halModule all
   * take _cfg in the constructor's init list. */
  const devourer::DeviceConfig _cfg;
  std::shared_ptr<EepromManager> _eepromManager;
  std::shared_ptr<RadioManagementModule> _radioManagement;
  /* Last channel handed to SetMonitorChannel. Value-initialised so the
   * 5GHz CCK clamp in send_packet reads Channel=0 (clamp off) rather than
   * indeterminate garbage before the first channel set. */
  SelectedChannel _channel{};
  RtlAdapter _device;
  HalModule _halModule;
  Logger_t _logger;
  uint8_t debug;
  Action_ParsedRadioPacket _packetProcessor = nullptr;
  /* Runtime TX-mode default (SetTxMode/ClearTxMode); applied in send_packet
   * only when the frame's radiotap carries no rate. */
  std::optional<devourer::TxMode> _tx_mode_default;

  /* CW single-tone (StartCwTone/StopCwTone) saved state for a clean restore:
   * the pre-tone RF 0x00 and four BB dwords — RFE-pinmux words on 8812/8821
   * (0xCB0/0xEB0/0xCB4/0xEB4), per-path TX-scale words on 8814 (0xC1C/0xE1C/
   * 0x181C/0x1A1C). _cw_active guards against double start/stop. */
  bool _cw_active = false;
  uint32_t _cw_rf00 = 0;
  uint32_t _cw_bb[4] = {0, 0, 0, 0};

  /* Modulated continuous TX (StartContinuousTx/StopContinuousTx) guard. */
  bool _cont_active = false;

  /* Bring-up completion (StartWithMonitorMode succeeded): gates the live
   * apply in the runtime TX-power setters — before bring-up they only record
   * state (an ApplyTxPower on a cold chip would write an unpowered BB). */
  bool _brought_up = false;

  /* Rolling per-frame RX link-quality aggregate (GetRxQuality). Fed from the RX
   * loop for every decoded frame; drained by GetRxQuality on the caller's
   * cadence. */
  devourer::RxQualityAccumulator _rxq;
  devourer::RxPathActivityAccumulator _rxpaths;

public:
  RtlJaguarDevice(RtlAdapter device, Logger_t logger,
                  devourer::DeviceConfig cfg = {});
  ~RtlJaguarDevice() override;
  void Init(Action_ParsedRadioPacket packetProcessor,
            SelectedChannel channel) override;
  /* Blocking RX worker loop on an already-brought-up chip (see IRtlDevice).
   * Init = bring-up + BFEE arm + StartRxLoop; a TX+RX caller does InitWrite
   * once, then runs this on its own std::thread next to the TX loop. */
  void StartRxLoop(Action_ParsedRadioPacket packetProcessor) override;
  void StopRxLoop() override { should_stop = true; }
  void SetMonitorChannel(SelectedChannel channel) override;
  /* Lean frequency-hop retune: switches the RF channel only, skipping the
   * per-rate TX-power loop, bandwidth post-set, and thermal pwrtrk tick that
   * SetMonitorChannel runs — none of which change across an intra-band, 20 MHz
   * hop. Falls back to the full SetMonitorChannel automatically when the hop
   * crosses the 2.4/5 GHz boundary or the current bandwidth isn't 20 MHz.
   * cache_rf=true additionally avoids the per-write 20 ms C-cut RF-read sleep
   * by writing RF_CHNLBW from a cached value. Intended for channel hopping;
   * keeps the device channel state in sync for the 5 GHz CCK clamp.
   * (The cache_rf default binds at IRtlDevice — virtual default arguments
   * resolve statically, so overrides must not re-declare it.) */
  void FastRetune(uint8_t channel, bool cache_rf) override;
  void FastSetBandwidth(ChannelWidth_t bw) override;
  void InitWrite(SelectedChannel channel) override;
  /* Legacy per-rate TXAGC override pair — superseded by the IRtlDevice
   * runtime TX-power API (SetTxPowerIndexOverride applies in one call).
   * Inline forwards kept for one release cycle, Rtl8812aDevice-alias style. */
  [[deprecated("use SetTxPowerIndexOverride (applies live)")]]
  void SetTxPowerOverride(int idx) {
    SetTxPowerIndexOverride(idx);
  }
  [[deprecated("SetTxPowerIndexOverride / SetTxPowerOffsetQdb apply live; "
               "use ReApplyTxPower to force a re-program")]]
  void ApplyTxPower() {
    ReApplyTxPower();
  }

  /* Runtime TX-power control (IRtlDevice contract; see src/TxPower.h).
   * Jaguar1 caps: 6-bit TXAGC index, 0.5 dB (2 qdB) per step. The offset
   * folds into ComputeTxPowerIndex, so it reaches the per-rate TXAGC fanout
   * (0xc20..0xc4c / packed 0x1998 on 8814) AND the 0xc54 power-training
   * word, and is re-folded by every SetMonitorChannel. Readback comes from
   * 0xc20/0xc24/0xc30 on 8812/8811/8821; the 8814's packed TXAGC port is
   * write-only, so it reports the software shadow (hw_readback=false). */
  devourer::TxPowerCaps GetTxPowerCaps() override;
  int SetTxPowerOffsetQdb(int qdb) override;
  void SetTxPowerIndexOverride(int idx) override;
  bool ReApplyTxPower() override;
  int SetXtalCap(int cap) override;
  int GetXtalCap() override { return _xtal_cap; }
  devourer::TxPowerState GetTxPowerState() override;
  /* Per-chip TX caps (IRtlDevice): n_ss + STBC/LDPC/SGI/bw from the EFUSE
   * RF-type. STBC needs >=2 chains, so 1T1R cuts (8811AU/8821AU) report
   * stbc_ok=false and send_packet drops an STBC request. */
  devourer::TxCaps GetTxCaps() override;
  /* Aggregate identity + radio + feature caps (IRtlDevice). Composes GetTxCaps
   * / GetTxPowerCaps; identity from the EFUSE version-id + RF-type. */
  devourer::AdapterCaps GetAdapterCaps() override;
  /* Live per-chain RX-path activity (fed via _rxpaths in the RX loop). */
  devourer::ActiveRxPaths GetActiveRxPaths() override {
    return _rxpaths.snapshot();
  }
  /* Read a baseband register (debug/diagnostic). Thin passthrough to the
   * radio manager's BB read — handy for confirming a TXAGC write landed. */
  uint32_t ReadBBReg(uint16_t addr, uint32_t mask);

  /* Realtek MP single-tone (CW carrier) — radiate a bare RF local-oscillator
   * carrier at the tuned channel center. Path A; all Jaguar-1 members —
   * 8812AU (2T2R) / 8821AU (1T1R) via hal_mpt_SetSingleToneTx() (OFDM/CCK off +
   * RFE pinmux), and 8814AU (4T4R) via mpt_SetSingleTone_8814A() (CCA off +
   * per-path TX-scale zero). `gain` is the RF 0x00[4:0] gain index (0 = lowest).
   * A controllable narrowband interferer / MP tone source. StopCwTone() restores
   * the state saved at start and disables the LO — returning the chip to normal
   * TX/RX. Idempotent. */
  void StartCwTone(uint8_t gain);
  void StopCwTone();

  /* Realtek MP modulated continuous TX — the sibling of StartCwTone. Where the
   * CW tone radiates a bare unmodulated LO carrier, this streams a *real*
   * OFDM/HT/VHT waveform back-to-back at `mode`'s rate (the vendor
   * mpt_StartOfdmContTx path: OFDM block on, scrambler on, continuous-TX mode
   * bits 0x914[18:16]=1). It applies `mode` via SetTxMode; the caller primes a
   * few frames to load a PPDU, then the chip holds a 100%-duty modulated carrier
   * (idle-hold — no continuous feed needed). Full-channel, full-MCS occupancy —
   * the active stimulus for spectral / power / thermal characterisation.
   * StopContinuousTx clears the mode bits, pulses a BB reset, and restores.
   * Idempotent via _cont_active. TXAGC/power is the normal per-rate path
   * (SetTxPowerOverride), not a bare RF gain like the CW tone. */
  void StartContinuousTx(const devourer::TxMode& mode);
  void StopContinuousTx();

  /* Frame-free RX energy / channel-busy snapshot (see RxSense.h) — reads the
   * phydm OFDM/CCK false-alarm + CCA counters (0xF48/0xA5C/0xF08) and the DIG
   * IGI noise-floor (0xC50), then resets the counters so the next call is a
   * fresh delta. The read side of the CW tone. NB: if DEVOURER_PHYDM_WATCHDOG is
   * also running it shares/steals these counters. */
  RxEnergy GetRxEnergy() override;

  /* Consolidated windowed RX link-quality snapshot (see RxQuality.h) — subsumes
   * GetRxEnergy. Fed per decoded frame in the RX loop via _rxq. */
  devourer::RxQuality GetRxQuality() override {
    return devourer::build_rx_quality(_rxq.snapshot(), GetRxEnergy());
  }

  /* Adapter-health probes (see src/AdapterHealth.h). The EFUSE probe re-runs
   * the physical map read N times and cross-compares — post-bring-up only
   * (guarded on _brought_up; on the 8814AU a pre-fwdl EFUSE read breaks the
   * RSVD-page fwdl transport). */
  devourer::EfuseStability ProbeEfuseStability(int reads) override;
  devourer::FwBootStatus GetFwBootStatus() override {
    return _halModule.GetFwBootStatus();
  }

  /* Runtime TX-mode default. send_packet honours a frame's own radiotap rate
   * fields per-packet; when a frame's radiotap carries no rate, this mode
   * supplies the modulation / MCS / BW / GI / FEC / STBC instead of the
   * built-in 1M-CCK fallback. Pure state — applied on the next send_packet,
   * no USB I/O. ClearTxMode() reverts to the built-in default. */
  void SetTxMode(const devourer::TxMode& mode);
  void ClearTxMode();

  bool send_packet(const uint8_t* packet, size_t length) override;
  devourer::TxStats GetTxStats() override { return _device.GetTxStats(); }
  SelectedChannel GetSelectedChannel() override;
  uint64_t ReadTsf() override;

  /* Runtime RX-chain selection — the adaptive-link spatial-diversity lever
   * (the read/write superset of the DEVOURER_RX_PATHS env knob). Writes the
   * RX-path-enable mask 0x808[7:0] (bits 0/4 = path A CCK/OFDM, 1/5 = B,
   * 2/6 = C, 3/7 = D): 0x11 = A only, 0x33 = A+B, 0x77 = A+B+C, 0xFF = all.
   * The 8814 has 4 chains to trade; on 8812/8821 the high bits are no-ops.
   * Masking a chain drops its per-chain RSSI to the noise floor — it changes
   * WHICH chains combine, not their gain. A controller with a motion/fade
   * signal switches this live instead of on the env toggle's fixed timer.
   *
   * STICKY: once set, the mask is cached and re-applied after every
   * SetMonitorChannel (IQK saves/restores 0x808, so a channel set would
   * otherwise revert it to the table default) — the same channel-sticky
   * contract as the runtime TX-power knobs. GetRxPathMask reads 0x808 back;
   * -1 = never set (chip at the table default, all paths). */
  void SetRxPathMask(uint8_t mask);
  int GetRxPathMask();

  bool should_stop = false;

  /* Per-queue free-page snapshot read from REG_FIFOPAGE_INFO_1..5
   * (0x0230 / 0x0234 / 0x0238 / 0x023C / 0x0240). 8814A-only — these
   * registers don't exist on 8812 / 8821, so the snapshot stays at all
   * zeros on those chips and start_queue_depth_poller() is a no-op.
   *
   * Polling cadence is set via interval_ms (DEVOURER_QUEUE_POLL_MS in
   * the demos). 0 = disabled. The poller spawns a worker thread that
   * issues a vendor-control read for each register every interval and
   * stores the raw 32-bit word — interpretation (which bits are free
   * pages vs reserved) is left to the consumer, since the per-queue
   * layout differs slightly between chip cuts. */
  void start_queue_depth_poller(uint32_t interval_ms);
  std::array<uint32_t, 5> get_queue_depth() const;

  /* Read the chip thermal meter (RF[A][0x42][15:10]) paired with the EFUSE
   * baseline. Read-only — leaves the TX-power-tracking BB-swing registers
   * untouched. Works on every Jaguar member. Safe to call from the thread
   * that owns the device (e.g. inline in a TX loop) — no USB contention.
   * See devourer::ThermalStatus (src/ThermalStatus.h) for field semantics.
   * NB: on the 8814 the EFUSE baseline is read at the 8812 offset, so the
   * absolute delta may be off there; the raw trend is still valid. */
  ThermalStatus GetThermalStatus() override;

  /* Spawn a background thread that samples the thermal meter every
   * interval_ms and stores a snapshot (queryable via get_thermal_snapshot).
   * Emits a logger->warn when delta >= warn_delta. 0 interval = disabled.
   * Intended for the RX demo, whose Init() blocks the main thread.
   *
   * CONCURRENCY: an RF read is a multi-step BB register sequence over the
   * shared libusb handle. Background phydm-style polling has wedged the chip
   * before (ch100 second-channel-set), so this poller is opt-in and should
   * use a conservative cadence (>= 1 s). A TX loop on the owning thread
   * should prefer the synchronous GetThermalStatus() instead. */
  void start_thermal_poller(uint32_t interval_ms, int warn_delta);
  ThermalStatus get_thermal_snapshot() const;

  /* F2 research helper: read a u32 from the BB debug port at `selector`,
   * with save/restore around register 0x8FC. Lazy-constructs the reader
   * on first call. Returns 0 if the chip wedged on a prior call. See
   * BbDbgportReader.h for the brick-risk caveats. */
  uint32_t read_bb_dbgport(uint32_t selector);
  bool bb_dbgport_wedged() const;

private:
  void StartWithMonitorMode(SelectedChannel selectedChannel);
  bool NetDevOpen(SelectedChannel selectedChannel);

  std::array<std::atomic<uint32_t>, 5> _qd_snap{};
  std::thread _qd_thread;
  std::atomic<bool> _qd_stop{false};

  /* DEVOURER_RX_PATHS toggle thread — cycles the RX-path mask (0x808) for the
   * mobile/fading combining measurement. */
  void start_rx_path_toggle(const std::vector<uint8_t> &masks,
                            uint32_t interval_ms);
  std::thread _rxmask_thread;
  std::atomic<bool> _rxmask_stop{false};
  /* Cached RX-path mask for SetRxPathMask stickiness: -1 = never set (leave
   * the chip's table default across channel sets). Atomic so the toggle
   * thread and a control-plane SetMonitorChannel see a consistent value. */
  std::atomic<int> _rx_path_mask{-1};
  int _xtal_cap = -1; /* current crystal-cap code (SetXtalCap) */
  devourer::CfoTracker _cfo; /* closed-loop CFO tracker (#217) */

  std::thread _therm_thread;
  std::atomic<bool> _therm_stop{false};
  /* Packed last thermal snapshot: bit0 = valid, [8:15] = raw,
   * [16:23] = baseline, [24:31] = signed delta (clamped to int8). Stored as
   * one atomic so a reader sees a consistent tuple without a mutex. */
  std::atomic<uint32_t> _therm_snap{0};

  std::unique_ptr<devourer::BbDbgportReader> _bb_dbgport;
};

/* Backwards-compatibility alias. External callers using the old name still
 * compile but get a deprecation warning. Remove after one release cycle. */
using Rtl8812aDevice [[deprecated("renamed to RtlJaguarDevice")]] =
    RtlJaguarDevice;

#endif /* RTL_JAGUAR_DEVICE_H */
