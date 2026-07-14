#ifndef RTL_JAGUAR3_DEVICE_H
#define RTL_JAGUAR3_DEVICE_H

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

#include "logger.h"
#include "CfoTracker.h"
#include "IRtlDevice.h"
#include "TxMode.h"
#include "RtlAdapter.h"
#include "SelectedChannel.h"
#include "ChipVariant.h"
#include "HalJaguar3.h"
#include "RadioManagementJaguar3.h"

/* RtlJaguar3Device is the orchestrator for the Realtek "Jaguar3" 802.11ac family
 * — RTL8822CU, RTL8812EU, RTL8822EU. It is the Jaguar3 sibling of
 * RtlJaguarDevice (Jaguar1) and implements the same IRtlDevice contract so the
 * demos and WiFiDriver factory treat both uniformly.
 *
 * Bring-up is ported from Realtek vendor source (rtl88x2cu/phydm/halrf):
 * power-on, firmware download, MAC/BB/RF config, IQK and DACK drive RX,
 * channel/bandwidth (incl. 5/10 MHz narrowband) and on-air TX. send_packet is
 * on-air; sustained continuous TX is kept alive by the coex runtime thread
 * (coex_runtime_loop) — see CLAUDE.md. */
class RtlJaguar3Device : public IRtlDevice {
public:
  RtlJaguar3Device(RtlAdapter device, Logger_t logger,
                   jaguar3::ChipVariant variant = jaguar3::ChipVariant::C8822C,
                   devourer::DeviceConfig cfg = {});
  ~RtlJaguar3Device() override;

  void Init(Action_ParsedRadioPacket packetProcessor,
            SelectedChannel channel) override;
  /* Blocking RX worker loop on an already-brought-up chip (see IRtlDevice).
   * Init = bring-up + BFEE arm + StartRxLoop; a TX+RX caller (self-sounding
   * single-radio ground station) does InitWrite once, then runs this on its
   * own std::thread next to the TX loop. NB: for reliable RX the TX+RX intent
   * must be declared at InitWrite time (DEVOURER_TX_WITH_RX set) — InitWrite
   * then keeps the RX filters open and enables the RX path during bring-up.
   * On a plain TX-only bring-up this falls back to a best-effort retrofit
   * (filter re-open + enable_rx_path), which has proven unreliable on 8822E.
   * Takes over the bulk-IN endpoint from the coex thread's C2H drain for as
   * long as it runs. */
  void StartRxLoop(Action_ParsedRadioPacket packetProcessor) override;
  void StopRxLoop() override { _rx_stop = true; }
  void SetMonitorChannel(SelectedChannel channel) override;
  /* Lean frequency-hop retune (Jaguar3 port of the Jaguar1 FastRetune — see
   * docs/frequency-hopping.md): the RF18 channel write inside its 3-wire
   * bracket + force-anapar + BB reset, with the channel-keyed constants (SCO,
   * TX DFIR, AGC table) written only when their bucket changes. Skips the
   * bandwidth block, RXBB, MAC BW/TXSC, and — in 5/10 MHz mode — preserves the
   * narrowband dividers, so fast hops keep the NB re-clock without re-running
   * the divider recipe. Falls back to the full set_channel_bwmode on a band
   * change. cache_rf selects the cached-RF18 variant (meaningful on the 8822c,
   * whose RF18 is a read-modify-write; the 8822e composes it from scratch).
   * Serializes on _reg_mu, so it is safe during an active TX session — the
   * sanctioned in-session hop primitive (unlike a bare SetMonitorChannel). */
  void FastRetune(uint8_t channel, bool cache_rf) override;
  void FastSetBandwidth(ChannelWidth_t bw) override;
  void InitWrite(SelectedChannel channel) override;
  bool send_packet(const uint8_t *packet, size_t length) override;
  devourer::TxStats GetTxStats() override { return _device.GetTxStats(); }
  SelectedChannel GetSelectedChannel() override;
  uint64_t ReadTsf() override;
  void Stop() override;

  /* Runtime TX-power control (IRtlDevice contract; see src/TxPower.h).
   * Jaguar3 caps: 7-bit TXAGC reference, 0.25 dB (1 qdB) per step. The offset
   * shifts the per-path reference anchor (0x18e8/0x41e8 OFDM, 0x18a0/0x41a0
   * CCK) — the 0x3a00 per-rate diff table is offset-invariant, so a live step
   * is ~8 gated register writes (apply_tx_power_refs_8822e / a diffs-kept
   * set_tx_power_ref) under _reg_mu, serialized against the coex tick's
   * pwr_track (which RMWs the [7:0] thermal field of the SAME 0x18a0/0x41a0
   * dwords — field-disjoint, so thermal compensation and the offset compose).
   * The 8822E TX+RX 0x41e8 quirk is enforced structurally: every ref write
   * takes skip_path_b_ofdm_ref from _rx_wanted. A full SetMonitorChannel
   * re-folds the knobs against the new channel group's efuse refs (gated on a
   * knob being active); FastRetune never touches TXAGC. GetThermalStatus
   * reads RF 0x42[6:1] via the calibration impl (efuse baseline on the E,
   * first-read cold reference on the C). */
  devourer::TxPowerCaps GetTxPowerCaps() override;
  int SetTxPowerOffsetQdb(int qdb) override;
  void SetTxPowerIndexOverride(int idx) override;
  bool ReApplyTxPower() override;
  int SetXtalCap(int cap) override;
  int GetXtalCap() override { return _xtal_cap; }
  devourer::TxPowerState GetTxPowerState() override;
  devourer::ThermalStatus GetThermalStatus() override;
  /* Per-chip TX caps (IRtlDevice): 8822C/8822E are 2T2R (STBC ok). */
  devourer::TxCaps GetTxCaps() override;
  /* Aggregate identity + radio + feature caps (IRtlDevice). Composes GetTxCaps
   * / GetTxPowerCaps; identity from ChipVariant, transport from the adapter. */
  devourer::AdapterCaps GetAdapterCaps() override;
  /* Live per-chain RX-path activity (fed via _rxpaths in the RX loop). */
  devourer::ActiveRxPaths GetActiveRxPaths() override {
    return _rxpaths.snapshot();
  }
  /* Runtime TX-mode default — applied in send_packet when the radiotap carries
   * no rate. Without this the Jaguar3 TX path fell back to MGN_1M for rate-less
   * frames (so DEVOURER_TX_RATE/an MCS flood went on-air at 1 Mbps): the feature
   * was ported for Jaguar1 (RtlJaguarDevice) but not Jaguar3. */
  void SetTxMode(const devourer::TxMode &mode) override;
  void ClearTxMode() override;

  /* Realtek MP single-tone (CW carrier) — radiate a bare RF local-oscillator
   * carrier at the tuned channel center. Path A; Jaguar3 (rtl8822c / rtl8822e,
   * identical recipe). Unlike the older chips, Jaguar3's RF mode register (0x00)
   * is written through the HSSI 3-wire port (0x1808), NOT the direct BB->RF
   * window (which is read-only for RF 0x00), and the tone only radiates once the
   * BB/MAC TX path is keyed via PMAC (0x1d08/0x1e70) — see phydm_mp_set_single_
   * tone_jgr3 + phydm_set_pmac_txon_jgr3. `gain` is RF 0x00[4:0]. StopCwTone()
   * restores RF 0x00, re-enables CCA and disables the LO + PMAC. Serializes on
   * _reg_mu against the coex runtime thread. Idempotent. */
  void StartCwTone(uint8_t gain);
  void StopCwTone();

  /* Modulated continuous TX — sibling of StartCwTone. Engages the JGR3 hardware
   * continuous mode via the phydm PMAC packet generator: stop the normal TRX
   * (pause TX queues 0x522, disable OFDM/CCK CCA), define a legacy-6M PMAC packet
   * (L-SIG + rate + tx-info), enable PMAC (0x1d08) + TX-OFDM (0x1e70) + the
   * continuous hold (0x1ca4). Radiates a 100%-duty modulated OFDM carrier at 6M
   * (the SIG encoding is simplest for legacy OFDM; the stimulus rate is fixed at
   * 6M regardless of `mode`). Idle-hold; StopContinuousTx reverses it. Serializes
   * on _reg_mu against the coex thread. */
  void StartContinuousTx(const devourer::TxMode& mode);
  void StopContinuousTx();

  /* Frame-free RX energy / channel-busy snapshot (see RxSense.h). Jaguar3's
   * phydm DIG/FA machinery was never ported to devourer, but the 8822C/E BB has
   * the same facilities in a newer register space — OFDM/CCK CCA (0x2c08), CCK
   * FA (0x1a5c), OFDM FA (sum of 0x2d04/08/10/20/0c), IGI (0x1d70) — reset via
   * 0x1a2c + 0x1eb4[25]. Read-then-reset for a per-call delta; serialized on
   * _reg_mu against the coex runtime thread. The read side of the CW tone. */
  RxEnergy GetRxEnergy() override;

  /* Consolidated windowed RX link-quality snapshot (see RxQuality.h) — subsumes
   * GetRxEnergy. Fed per decoded frame in the RX loop via _rxq. On Jaguar3 the
   * noise-floor is the passive rssi-snr estimate (this generation has no
   * background DIG, so IGI is static and can't track the floor). */
  devourer::RxQuality GetRxQuality() override {
    return devourer::build_rx_quality(_rxq.snapshot(), GetRxEnergy());
  }

  /* dis_cca / EDCCA-disable investigation knob (DEVOURER_DIS_CCA). Writes the
   * vendor rtw_proc.c dis_cca recipe (MAC BIT_DIS_EDCCA 0x520[15] + EDCCA-mask
   * countdown 0x524[11], BB 0x1a9c[20]/0x1a14[9:8]/0x1d58[0xff8]); disabled=false
   * restores the inverse. Sticky across SetMonitorChannel; serialized on _reg_mu
   * against the coex tick. Measure-first — kept for the swept-AWGN A/B; not on
   * IRtlDevice until the measurement justifies it. */
  void SetCcaMode(bool disabled);

  /* Adapter-health probes (see src/AdapterHealth.h). EFUSE probe is 8822C
   * only — the 8822E's OTP is not reliably readable post-bring-up by design
   * (HalJaguar3::cache_efuse_8822e), so probing it would flag healthy units;
   * 8822E returns supported=false. Serialized on _reg_mu against the coex
   * tick like every other register-touching entry point. */
  devourer::EfuseStability ProbeEfuseStability(int reads) override;
  devourer::FwBootStatus GetFwBootStatus() override {
    return _hal.fw_boot_status();
  }

  bool should_stop = false;

private:
  RtlAdapter _device;
  const devourer::DeviceConfig _cfg;
  Logger_t _logger;
  jaguar3::ChipVariant _variant;
  int _xtal_cap = -1; /* current crystal-cap code (SetXtalCap) */
  jaguar3::HalJaguar3 _hal;
  jaguar3::RadioManagementJaguar3 _radioManagement;
  SelectedChannel _channel{};
  Action_ParsedRadioPacket _packetProcessor = nullptr;
  /* Runtime TX-power knobs (atomic so GetTxPowerState's cached snapshot is
   * readable cross-thread). Flat override -1 = the chip's efuse-calibrated
   * power; offset in 0.25 dB reference steps. Applied live under _reg_mu once
   * brought up; recorded and folded at InitWrite before. */
  std::atomic<int> _tx_pwr_override{-1};
  std::atomic<int> _tx_pwr_offset_steps{0};
  /* Rail-hit flags from the last apply (references clamped at 0/0x7f). */
  std::atomic<bool> _txpwr_sat_low{false};
  std::atomic<bool> _txpwr_sat_high{false};
  /* Bring-up completion: gates the live apply (+ _reg_mu use) in the setters. */
  bool _brought_up = false;
  /* Rolling per-frame RX link-quality aggregate (GetRxQuality). */
  devourer::RxQualityAccumulator _rxq;
  devourer::RxPathActivityAccumulator _rxpaths;
  devourer::CfoTracker _cfo; /* closed-loop CFO tracker (DEVOURER_CFO_TRACK) */
  /* Frame counter for periodic NDPA sounding (DEVOURER_TX_NDPA=N). */
  uint64_t _ndpa_ctr = 0;
  /* TX beamforming apply state (DEVOURER_BF_TXBF). The entry is configured at
   * InitWrite (arm_beamformer_entry); the apply toggle is enabled from the RX
   * loop only after a Compressed Beamforming Report from _bf_peer is ingested
   * (blind apply degrades). */
  bool _bf_txbf_armed = false;
  std::atomic<bool> _bf_apply_on{false};
  std::atomic<uint64_t> _bf_cbr_count{0};
  uint8_t _bf_peer[6] = {0};
  /* dis_cca sticky state — re-applied after SetMonitorChannel (the channel set
   * rewrites the BB CCA registers). Caller holds _reg_mu. */
  bool _cca_disabled = false;
  void apply_cca_mode_locked(bool disabled);
  /* TX+RX intent (DEVOURER_TX_WITH_RX at InitWrite / an RX-side Init):
   * consumed as skip_path_b_ofdm_ref by EVERY TXAGC ref write, so no offset
   * churn can ever touch 0x41e8 while RX is alive (the 8822E RX-desense
   * quirk is enforced structurally, not by call-site discipline). */
  bool _rx_wanted = false;
  /* Cached 8822E per-channel-group efuse base refs (the values InitWrite
   * derived, incl. the 0x4b fallback) so an offset-only step recomputes
   * effective refs without re-deriving; invalidated by a channel change. */
  uint8_t _pwr_ref_a = 0, _pwr_ref_b = 0;
  bool _pwr_ref_valid = false;
  /* True while the 0x3a00 per-rate diff table is zeroed (flat semantics /
   * 8822C default) — repeated flat steps then skip the 32-dword re-zero. */
  bool _diffs_zeroed = false;
  /* Re-program TXAGC from the current knob state. full=true re-derives the
   * 8822E efuse refs + rewrites the per-rate diff table (bring-up / channel
   * change / flat<->efuse transitions); full=false is the light offset step.
   * Caller holds _reg_mu when the coex thread may be running. */
  void apply_tx_power_current(bool full);
  /* Runtime TX-mode default (SetTxMode/ClearTxMode). */
  std::optional<devourer::TxMode> _tx_mode_default;

  /* CW single-tone (StartCwTone/StopCwTone) saved state: the pre-tone RF 0x00
   * (path A, full 20-bit). CCA/PMAC restore to fixed values, so no BB snapshot
   * is needed. _cw_active guards double start/stop. */
  bool _cw_active = false;
  uint32_t _cw_rf00 = 0;
  /* Modulated continuous TX (StartContinuousTx/StopContinuousTx) guard + saved
   * TRX state for a clean restore (TX-queue pause byte 0x522, CCK-TX path
   * 0x1a04[31:28]), captured by the phydm_stop_ic_trx-equivalent at start. */
  bool _cont_active = false;
  uint8_t _cont_txpause = 0;
  uint32_t _cont_ccktx = 0;
  /* Coex runtime: a background thread that drains bulk-IN, dispatches firmware
   * C2H reports (BT-info etc.) and runs the periodic coex decision so the FW's
   * PTA keeps the antenna with WLAN during sustained TX.
   * THREADING CONTRACT: while a TX session is active (between InitWrite and Stop)
   * the ~2 s housekeeping tick, StartRxLoop's one-shot RX-filter restore, and
   * every channel retune (SetMonitorChannel / FastRetune — register RMW)
   * serialize on _reg_mu. The TX hot path (send_packet) does no register I/O of
   * its own, so it runs lock-free alongside them; a radiotap-CHANNEL hop inside
   * send_packet takes the lock only for the retune itself (so a hop can be
   * delayed by up to one coex tick). Callers must still not invoke SetTxPower
   * mid-session. Bulk-IN has exactly one reader at a time: while StartRxLoop is
   * active (_rx_loop_active) the coex thread skips its C2H drain — the RX async
   * loop sees the C2H reports as part of its stream. */
  std::thread _coex_thread;
  volatile bool _coex_stop = false;
  void coex_runtime_loop();
  /* StartRxLoop stop request (StopRxLoop). */
  volatile bool _rx_stop = false;
  /* True while StartRxLoop owns bulk-IN (gates the coex thread's drain). */
  std::atomic<bool> _rx_loop_active{false};
  /* Set when InitWrite zeroes the RX filters (0x6A0-0x6A4) for TX-only
   * throughput; StartRxLoop restores the monitor_rx_cfg values and clears it. */
  bool _rx_filters_closed = false;
  /* Serializes the coex housekeeping tick against StartRxLoop's register
   * restore (the only two register writers during an active TX session). */
  std::mutex _reg_mu;
};

#endif /* RTL_JAGUAR3_DEVICE_H */
