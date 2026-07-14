#ifndef RTL_JAGUAR2_DEVICE_H
#define RTL_JAGUAR2_DEVICE_H

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "logger.h"
#include "IRtlDevice.h"
#include "TxMode.h"
#include "RtlAdapter.h"
#include "SelectedChannel.h"
#include "CfoTracker.h"
#include "HalJaguar2.h"
#include "HalmacJaguar2MacInit.h"
#include "HalmacJaguar2Fw.h"
#include "ChipVariant.h"
#include "Jaguar2Calibration.h"

/* RtlJaguar2Device is the orchestrator for the Realtek "Jaguar2" 802.11ac family
 * — RTL8822BU (chip 8822B, 2T2R, USB). It is the Jaguar2 sibling of
 * RtlJaguarDevice (Jaguar1) and RtlJaguar3Device (Jaguar3) and implements the
 * same IRtlDevice contract so the demos and the WiFiDriver factory treat all
 * three uniformly.
 *
 * Jaguar2 is a hybrid of the two existing generations: firmware download, MAC
 * init and power sequencing follow the HalMAC path (like Jaguar3), while the
 * phydm BB/AGC/RF register tables use the older `check_positive` format (like
 * Jaguar1, via the shared PhyTableLoader). Bring-up is ported from the vendor
 * rtl88x2bu (8822B) / rtl8821cu (8821C) trees.
 *
 * Multi-chip via jaguar2::ChipVariant (C8822B 2T2R / C8821C 1T1R), selected in
 * the WiFiDriver factory from the SYS_CFG2 chip-id and threaded into HalJaguar2
 * (Jaguar2PhyTables table data + RF-path count), HalmacJaguar2Fw (blob) and the
 * Jaguar2Calibration IQK factory — the same strategy-dispatch shape as the
 * Jaguar3 8822C/8822E HAL. */
class RtlJaguar2Device : public IRtlDevice {
public:
  RtlJaguar2Device(RtlAdapter device, Logger_t logger,
                   jaguar2::ChipVariant variant = jaguar2::ChipVariant::C8822B,
                   devourer::DeviceConfig cfg = {});
  ~RtlJaguar2Device() override;

  void Init(Action_ParsedRadioPacket packetProcessor,
            SelectedChannel channel) override;
  /* Blocking RX worker loop on an already-brought-up chip (see IRtlDevice).
   * Init = bring_up + StartRxLoop; a TX+RX caller does InitWrite once, then
   * runs this on its own std::thread next to the TX loop. Starts (and on exit
   * stops) the DIG thread — TX-only sessions stay DIG-free. */
  void StartRxLoop(Action_ParsedRadioPacket packetProcessor) override;
  void StopRxLoop() override { _rx_stop = true; }
  void SetMonitorChannel(SelectedChannel channel) override;
  /* Lean frequency-hop retune (the Jaguar2 port of the Jaguar1 FastRetune —
   * see docs/frequency-hopping.md): composed cached writes for RF18 + the
   * on-change channel constants, via HalJaguar2::fast_retune; falls back to
   * the full set_channel_bw on a band change. Same concurrency model as
   * SetMonitorChannel: no lock — the DIG thread (RX sessions only) writes its
   * own registers (IGI/FA), and TX-only sessions have no concurrent register
   * writer. cache_rf=false re-primes the compose cache per hop (A/B
   * measurement). */
  void FastRetune(uint8_t channel, bool cache_rf) override;
  void FastSetBandwidth(ChannelWidth_t bw) override;
  void InitWrite(SelectedChannel channel) override;
  bool send_packet(const uint8_t *packet, size_t length) override;
  devourer::TxStats GetTxStats() override { return _device.GetTxStats(); }
  SelectedChannel GetSelectedChannel() override;
  uint64_t ReadTsf() override;
  void Stop() override;
  void SetTxMode(const devourer::TxMode &mode) override;
  void ClearTxMode() override;

  /* Runtime TX-power control (IRtlDevice contract; see src/TxPower.h).
   * Jaguar2 caps: 6-bit TXAGC index, 0.5 dB (2 qdB) per step. The offset
   * folds into HalJaguar2::apply_tx_power after the regulatory min() (or onto
   * the flat override), covering CCK/OFDM/HT and — 8822B included — the VHT
   * sections. Live once brought up; recorded and folded at bring-up before.
   * A full SetMonitorChannel re-applies the knobs against the new channel's
   * efuse group (gated on a knob being active so the legacy no-knob path
   * stays byte-identical); FastRetune never touches TXAGC. Readback from
   * 0x1d00/0x1d04/0x1d10. GetThermalStatus reads RF[A] 0x42[15:10] + the
   * efuse baseline at 0xBA. */
  devourer::TxPowerCaps GetTxPowerCaps() override;
  int SetTxPowerOffsetQdb(int qdb) override;
  void SetTxPowerIndexOverride(int idx) override;
  bool ReApplyTxPower() override;
  int SetXtalCap(int cap) override;
  int GetXtalCap() override { return _xtal_cap; }
  devourer::TxPowerState GetTxPowerState() override;
  devourer::ThermalStatus GetThermalStatus() override;
  /* Per-chip TX caps (IRtlDevice): the 8821C is 1T1R (no STBC), the 8822B
   * 2T2R. send_packet drops an STBC request the variant can't honour. */
  devourer::TxCaps GetTxCaps() override;
  /* Aggregate identity + radio + feature caps (IRtlDevice). Composes GetTxCaps
   * / GetTxPowerCaps; identity from ChipVariant, transport from the adapter. */
  devourer::AdapterCaps GetAdapterCaps() override;
  /* Live per-chain RX-path activity (fed via _rxpaths in the RX loop). */
  devourer::ActiveRxPaths GetActiveRxPaths() override {
    return _rxpaths.snapshot();
  }

  /* Per-packet TX-power offset — the zero-cost per-frame power trim the
   * adaptive link wants (distinct from the per-rate TXAGC that
   * SetTxPowerOffsetQdb shifts). Sets the 8822B/8821C TX-descriptor TXPWR_OFSET
   * field (a hardware LUT applied on top of the rate's power); no register
   * write, no channel-set cost. `step` is the raw 3-bit LUT index: 0=none,
   * 1=-3dB, 2=-7dB, 3=-11dB, 4=+3dB, 5=+6dB. Applied as the default to every
   * frame lacking a per-packet radiotap DBM_TX_POWER field (which, on an
   * injected frame, send_packet honours as a per-packet dB delta quantized to
   * this LUT — the true per-packet path). On-air-confirmed: the LUT tracks
   * on-air power (tests/txpkt_pwr_ofset_onair.sh). */
  void SetTxPacketPowerStep(uint8_t step);

  /* Realtek MP single-tone (CW carrier) — radiate a bare RF local-oscillator
   * carrier at the tuned channel center. Path A; both Jaguar2 variants, per the
   * vendor hal_mpt_SetSingleToneTx() branches: OFDM/CCK modulators off, RF path A
   * to TX mode at `gain` (RF 0x00[4:0]) with the LO enabled. 8822B forces the
   * RFE pinmux (0x77777777) + RFE-inverse; 8821C uses its own path-A pinmux
   * (0xCB0[0xF0F0]=0x707) and gates the LO via RF 0x75[16] on 2.4 GHz (BTG) /
   * RF 0x58[1] on 5 GHz. StopCwTone() restores the saved state and disables the
   * LO. Idempotent. A controllable narrowband interferer / MP tone source. */
  void StartCwTone(uint8_t gain);
  void StopCwTone();

  /* Modulated continuous TX — sibling of StartCwTone. Streams a 100%-duty
   * modulated OFDM carrier via the vendor 0x914 continuous mode. The 0x914 bit
   * alone wedges the USB TX FIFO; the fix is rCCAonSec (0x838)=0x6d first, after
   * which the carrier self-radiates from BB state (no send_packet feed needed —
   * the demo idle-holds). SDR-verified as a flat ~18 MHz OFDM block.
   * StopContinuousTx clears the mode, pulses a BB reset, restores 0x838. */
  void StartContinuousTx(const devourer::TxMode& mode);
  void StopContinuousTx();

  /* Frame-free RX energy snapshot (see RxSense.h) — the FA/CCA/IGI values
   * dig_step samples over its ~100 ms window, plus a fresh NHM power histogram.
   * The read side of the CW tone. */
  RxEnergy GetRxEnergy() override;

  /* Consolidated windowed RX link-quality snapshot (see RxQuality.h) — subsumes
   * GetRxEnergy. Fed per decoded frame in the RX loop via _rxq. */
  devourer::RxQuality GetRxQuality() override {
    return devourer::build_rx_quality(_rxq.snapshot(), GetRxEnergy());
  }

  /* Adapter-health probes (see src/AdapterHealth.h). EFUSE probe re-reads the
   * physical logical map N times (~0.5 s per pass over USB control transfers)
   * and cross-compares; post-bring-up only. FW status comes from the DLFW
   * state machine's real hardware boundaries (HalmacJaguar2Fw::boot_status:
   * checksum-ready bits vs the 0xC078 boot handshake) — on Jaguar2 a FW
   * failure additionally aborts bring_up (throws after retries), but the
   * status still tells WHICH stage died. */
  devourer::EfuseStability ProbeEfuseStability(int reads) override;
  devourer::FwBootStatus GetFwBootStatus() override {
    return _fw.boot_status();
  }

private:
  RtlAdapter _device;
  const devourer::DeviceConfig _cfg;
  /* Rolling per-frame RX link-quality aggregate (GetRxQuality). */
  devourer::RxQualityAccumulator _rxq;
  devourer::RxPathActivityAccumulator _rxpaths;
  Logger_t _logger;
  jaguar2::ChipVariant _variant;
  int _xtal_cap = -1; /* current crystal-cap code (SetXtalCap) */
  devourer::CfoTracker _cfo; /* closed-loop CFO tracker (DEVOURER_CFO_TRACK) */
  jaguar2::HalJaguar2 _hal;
  jaguar2::HalmacJaguar2MacInit _macinit;
  jaguar2::HalmacJaguar2Fw _fw;
  SelectedChannel _channel{};
  Action_ParsedRadioPacket _packetProcessor = nullptr;
  /* Runtime TX-power knobs (atomic so GetTxPowerState's cached snapshot is
   * readable cross-thread; setters are control-plane-thread calls). Flat
   * override -1 = efuse per-rate baseline; offset in 0.5 dB index steps. */
  std::atomic<int> _tx_pwr_override{-1};
  std::atomic<int> _tx_pwr_offset_steps{0};
  /* Default per-packet TXPWR_OFSET LUT step (0 = none) — see SetTxPacketPowerStep. */
  std::atomic<uint8_t> _tx_pkt_pwr_step{0};
  /* Bring-up completion: gates the live apply in the TX-power setters. */
  bool _brought_up = false;
  /* Re-program TXAGC from the current knob state at the current channel:
   * flat override (+offset) via set_tx_power_flat, else the efuse per-rate
   * path with the offset folded. Called from bring_up and live from the
   * setters/SetMonitorChannel. */
  void apply_tx_power_current();
  /* rfe_type resolved during bring_up (efuse + DEVOURER_RFE), cached so
   * SetMonitorChannel can retune (set_channel_bw needs it). */
  uint8_t _rfe = 0;
  std::optional<devourer::TxMode> _tx_mode_default;

  /* CW single-tone (StartCwTone/StopCwTone) saved state for a clean restore:
   * the pre-tone RF 0x00 (path A) and the four RFE-pinmux BB words
   * (0xCB0/0xEB0/0xCB4/0xEB4). _cw_active guards double start/stop. Atomic so
   * the thermal-track tick reads a coherent value for its skip-during-tone
   * guard. */
  std::atomic<bool> _cw_active{false};
  uint32_t _cw_rf00 = 0;
  uint32_t _cw_bb[4] = {0, 0, 0, 0};

  /* Modulated continuous TX (StartContinuousTx/StopContinuousTx) guard + saved
   * pre-continuous rCCAonSec (0x838) for a clean restore. Atomic — see
   * _cw_active. */
  std::atomic<bool> _cont_active{false};
  uint32_t _cont_cca838 = 0;

  /* DIG (dynamic initial gain) background thread — periodically runs
   * HalJaguar2::dig_step so IGI tracks the false-alarm rate for weak-signal RX. */
  std::thread _dig_thread;
  std::atomic<bool> _dig_stop{false};
  void stop_dig();

  /* Per-chip halrf calibration (IQK + thermal TX-power tracking).
   * Persistent (not a bring_up local) so the thermal tick can reuse it after
   * bring-up; constructed in bring_up once cut / rf-type are known. */
  std::unique_ptr<jaguar2::Jaguar2Calibration> _cal;

  /* Thermal TX-power tracking background thread. Reads the RF
   * thermal meter every ~2 s and writes the MIX_MODE swing compensation via
   * _cal->pwr_track(). Started at the end of bring_up so it covers BOTH the RX
   * (Init) and TX-only (InitWrite) sessions — the sustained-TX case the DIG
   * thread (RX-only) never reaches. Serialized against SetMonitorChannel /
   * FastRetune / the TX-power setters / GetThermalStatus by _reg_mu (the RF
   * read window is a multi-transfer sequence that must not tear). */
  std::mutex _reg_mu;
  std::thread _pwrtrack_thread;
  std::atomic<bool> _pwrtrack_stop{false};
  void start_pwrtrack();
  void stop_pwrtrack();

  /* StartRxLoop stop request (StopRxLoop). volatile (not atomic) to match the
   * signal-flag pattern used across the library (g_devourer_should_stop). */
  volatile bool _rx_stop = false;

  /* Shared cold bring-up (power-on -> DLFW -> MAC/BB/RF -> channel -> LCK ->
   * IQK -> coex -> enable RX/TX engine). Used by both Init (RX) and InitWrite
   * (TX). Leaves the chip fully calibrated on `channel`. */
  void bring_up(SelectedChannel channel);
};

#endif /* RTL_JAGUAR2_DEVICE_H */
