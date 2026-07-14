#ifndef IRTL_DEVICE_H
#define IRTL_DEVICE_H

#include <cstddef>
#include <cstdint>
#include <functional>

#include "AdapterCaps.h"
#include "AdapterHealth.h"
#include "RxQuality.h"
#include "RxSense.h"
#include "SelectedChannel.h"
#include "ThermalStatus.h"
#include "TxCaps.h"
#include "TxMode.h"
#include "TxPower.h"
#include "TxStats.h"

/* Packet is the parsed-RX type handed to the RX callback. Forward-declared here
 * (a reference in the std::function signature needs only an incomplete type) so
 * the interface header stays light; the full definition lives in FrameParser.h. */
struct Packet;

using Action_ParsedRadioPacket = std::function<void(const Packet &)>;

/* IRtlDevice is the chip-family-agnostic device contract used by the demos and
 * the WiFiDriver factory. Three implementations exist:
 *   - RtlJaguarDevice   — Realtek "Jaguar" wave-1 (8812AU/8811AU/8821AU/8814AU)
 *   - RtlJaguar2Device  — Realtek "Jaguar2" (8822BU/8812BU)
 *   - RtlJaguar3Device  — Realtek "Jaguar3" (8822CU/8812EU/8822EU)
 *
 * Chip-family-specific research helpers (BB-debug-port reads, the 8814 queue
 * poller, ...) are intentionally NOT part of this interface — callers that need
 * them dynamic_cast down to the concrete type. */
class IRtlDevice {
public:
  virtual ~IRtlDevice() = default;

  virtual void Init(Action_ParsedRadioPacket packetProcessor,
                    SelectedChannel channel) = 0;
  virtual void InitWrite(SelectedChannel channel) = 0;

  /* Blocking RX worker loop. Assumes the chip is already brought up (a prior
   * Init or InitWrite on this device) — performs NO bring-up, channel set, or
   * beamforming arming. Runs on the CALLER's thread and returns once
   * StopRxLoop() is called or the global stop flag is set; it is restartable
   * after it returns. This is the piece that lets one process bring up once
   * (InitWrite) and then run TX and RX concurrently on the same claimed handle
   * — Init is the RX-only convenience wrapper (bring-up + StartRxLoop). */
  virtual void StartRxLoop(Action_ParsedRadioPacket packetProcessor) = 0;

  /* Ask a running StartRxLoop to exit (sets a flag; the caller then joins
   * whatever thread runs StartRxLoop). Default no-op. */
  virtual void StopRxLoop() {}
  virtual void SetMonitorChannel(SelectedChannel channel) = 0;

  /* Lean intra-band, same-bandwidth channel retune for hop/sweep dwells: the RF
   * channel switch only, skipping the per-rate TX-power loop and bandwidth
   * post-set a hop doesn't need (see docs/frequency-hopping.md). Generations
   * with a fast path override this (Jaguar1, Jaguar3 — both fall back to the
   * full path internally on a band change); the default is the full
   * SetMonitorChannel at the current width/offset, so callers may hop through
   * this unconditionally on any chip. `cache_rf` selects the cached-RF-write
   * variant where one exists (false = re-read per hop, for A/B measurement);
   * the default argument binds here, at the interface. */
  virtual void FastRetune(uint8_t channel, bool cache_rf = true) {
    (void)cache_rf;
    SelectedChannel c = GetSelectedChannel();
    c.Channel = channel;
    SetMonitorChannel(c);
  }

  /* Lean same-channel bandwidth switch between 20 MHz and 5/10 MHz narrowband —
   * the bandwidth analogue of FastRetune. On the chips that support it the
   * whole switch collapses to a single BB dword write, because across a
   * 20<->5/10 toggle the RF stays in 20 MHz mode and only the baseband ADC/DAC
   * re-clock register changes (everything else — RF bandwidth, MAC BW, TX
   * power, IQK — is invariant, so it is skipped). Generations with a fast path
   * override this; the default falls back to a full SetMonitorChannel at the
   * current channel/offset, so callers may use it unconditionally. */
  virtual void FastSetBandwidth(ChannelWidth_t bw) {
    SelectedChannel c = GetSelectedChannel();
    c.ChannelWidth = bw;
    SetMonitorChannel(c);
  }

  /* Force a flat absolute TXAGC index across all rates (the debug /
   * SDR-visibility knob — same knob as SetTxPowerIndexOverride, kept for
   * source compatibility; the override form has the explicit clear). NB this
   * is a REAL flat override on every generation now — it previously had three
   * divergent semantics (Jaguar1: pre-efuse fallback, silently ignored with
   * loaded EFUSE; Jaguar2: dead store; Jaguar3: flat reference). */
  virtual void SetTxPower(uint8_t power) {
    SetTxPowerIndexOverride(static_cast<int>(power));
  }

  /* --- Runtime TX-power control (see src/TxPower.h for the full model) ---
   *
   * THREADING CONTRACT: like every other control-plane entry point
   * (SetMonitorChannel, FastRetune, SetTxMode, Start/StopCwTone), these are
   * single-control-thread calls — invoke them from the thread that owns the
   * device's control plane, never concurrently with a channel set. Jaguar3
   * additionally serializes against its coex/thermal tick internally; on
   * Jaguar1/2 the USB-touching apply itself is the caller's to sequence.
   * GetTxPowerCaps and the cached (non-readback) part of GetTxPowerState are
   * safe from any thread. */

  /* Static capabilities of this family's TX-power knobs; supported=false on
   * the default (a generation without the API wired). */
  virtual devourer::TxPowerCaps GetTxPowerCaps() { return {}; }

  /* Adjust TX power RELATIVE to the efuse-calibrated per-rate table (or the
   * flat override when one is active) in quarter-dB. Quantized to the family
   * step and clamped to the caps range; returns the APPLIED qdB (0 when
   * unsupported), so a closed-loop controller knows exactly what moved.
   * Sticky: survives SetMonitorChannel (re-folded against the new channel's
   * table) and FastRetune (hop paths never rewrite TXAGC). Refused (returns
   * 0, logs) while a CW tone holds the chip — TXAGC does not modulate a bare
   * LO carrier. */
  virtual int SetTxPowerOffsetQdb(int qdb) {
    (void)qdb;
    return 0;
  }

  /* Force / clear the flat absolute TXAGC index: idx >= 0 forces it for all
   * rates (composes with the offset), idx < 0 reverts to the efuse per-rate
   * baseline. The primary knob — SetTxPower forwards here; a generation
   * without the API wired ignores it (caps.supported=false). */
  virtual void SetTxPowerIndexOverride(int idx) { (void)idx; }

  /* Re-program the TX-power registers from the current knob state at the
   * CURRENT channel — the hook tests use to force a re-apply without moving
   * any knob. Returns false when unsupported or the chip isn't brought up. */
  virtual bool ReApplyTxPower() { return false; }

  /* Crystal (XTAL) load-capacitance trim — the CFO lever. Writes the AFE
   * crystal-cap field (a per-chip register), pulling the chip's reference
   * oscillator a few ppm to align a marginal TX/RX crystal pair; the payoff
   * is narrowband at the edge of its CFO budget (5 MHz at 5 GHz). `cap` is a
   * raw trim code in [0, GetAdapterCaps().xtal_cap_max]; cap < 0 reverts to
   * the efuse/default value. Both physical caps (Xi/Xo) are set together.
   * Returns the applied code, or -1 when unsupported. Sticky across channel
   * changes (an AFE register, untouched by the RF retune). */
  virtual int SetXtalCap(int cap) {
    (void)cap;
    return -1;
  }

  /* Current crystal-cap code (the last SetXtalCap value, or the efuse default
   * at bring-up). -1 when unsupported. */
  virtual int GetXtalCap() { return -1; }

  /* Snapshot of the knob state + representative effective indices (register
   * readback where the family's TXAGC block is readable). */
  virtual devourer::TxPowerState GetTxPowerState() { return {}; }

  /* Chip thermal-meter snapshot (RF 0x42 family; efuse baseline where the
   * family wires one — see src/ThermalStatus.h). The PA-heating input of the
   * adaptive-link controller. Default returns an all-invalid reading. */
  virtual devourer::ThermalStatus GetThermalStatus() { return {}; }

  /* Per-chip TX capability report (see src/TxCaps.h): spatial streams, STBC /
   * LDPC / SGI support, max bandwidth — derived from the chip identity resolved
   * at construction. A caller (or send_packet) uses it to avoid requesting a
   * feature the silicon can't do (e.g. STBC on a 1T1R part, which produces a
   * frame that never decodes). Default returns supported=false. */
  virtual devourer::TxCaps GetTxCaps() { return {}; }

  /* Aggregate STATIC adapter-capability report (see src/AdapterCaps.h):
   * identity (chip name, generation, variant, transport, chip-id), TX/RX chain
   * counts, the composed TxCaps + TxPowerCaps, the supported channel-width set,
   * per-band tunable + characterized frequency spans, and the per-family
   * feature flags (per-packet TX power, narrowband, fast retune, per-chain
   * RSSI). Resolved at construction — safe from any thread and callable BEFORE
   * Init/InitWrite (the demos emit it as the `adapter.caps` event right after
   * CreateRtlDevice). Default returns supported=false. */
  virtual devourer::AdapterCaps GetAdapterCaps() { return {}; }

  /* Best-effort live estimate of which RX chains are actually carrying signal
   * (see ActiveRxPaths in src/RxQuality.h) — derived from the per-frame
   * per-chain RSSI the frame parser fills, so it needs an RX loop running and
   * ambient traffic to mean anything. A chain whose mean RSSI sits far below
   * the strongest is reported inactive (a disconnected/blocked antenna, or a
   * software-masked path). Delta semantics like GetRxQuality: the window drains
   * on read. Default is an all-invalid snapshot; a generation with >=2 RX
   * chains overrides. NOT part of AdapterCaps, which is static. */
  virtual devourer::ActiveRxPaths GetActiveRxPaths() { return {}; }

  virtual bool send_packet(const uint8_t *packet, size_t length) = 0;
  virtual SelectedChannel GetSelectedChannel() = 0;

  /* Read the 64-bit hardware TSF (Timing Synchronization Function) timer — the
   * 802.11 MAC's free-running microsecond clock (REG_TSFTR). It runs off the
   * chip's crystal and is latched into every RX descriptor at receive
   * (rx_pkt_attrib::tsfl, the low 32 bits), so it is a precise, host-jitter-free
   * timing reference for multi-radio sync / TDOA / scheduled bursts. Returns 0
   * where unsupported (default). NB: a register read is a control transfer —
   * calling it concurrently with a heavy RX bulk-IN load can race (catch the
   * exception). */
  virtual uint64_t ReadTsf() { return 0; }

  /* Clean shutdown: halt TRX DMA and power the chip down to a re-enumerable
   * state (mirrors the kernel driver's card-disable on unbind). Call after the
   * RX/TX loop exits and BEFORE releasing/closing the USB interface, so the
   * adapter isn't abandoned with its USB core hung. Default no-op. */
  virtual void Stop() {}

  /* Runtime TX-mode default: the modulation/rate/BW/GI/FEC used when a frame's
   * radiotap carries no rate (per-packet radiotap always wins). Both chip
   * families implement it; default no-op keeps other impls unaffected. Without
   * it, a rate-less frame falls back to MGN_1M — e.g. an MCS7 flood would
   * silently go on-air at 1 Mbps. */
  virtual void SetTxMode(const devourer::TxMode & /*mode*/) {}
  virtual void ClearTxMode() {}

  /* TX submission health snapshot (see TxStats.h) — the driver-side drop /
   * congestion signal an adaptive-link controller uses to detect a full TX FIFO
   * (a bulk-OUT TIMEOUT = recoverable back-pressure) vs a hard error. Counted at
   * the shared USB bulk-OUT layer, so every generation reports it. Default is an
   * all-zero snapshot. */
  virtual devourer::TxStats GetTxStats() { return {}; }

  /* Frame-free RX energy / channel-busy snapshot (see RxSense.h) — the read side
   * of the DEVOURER_CW_TONE emitter, used for spectrum-sensing / interferer
   * detection. Reads the chip's phydm false-alarm + CCA counters, DIG/IGI, and
   * (optionally) the NHM power histogram. FA/CCA counts are the delta since the
   * previous call. Default returns an all-invalid snapshot; each generation
   * overrides with a real reader. */
  virtual RxEnergy GetRxEnergy() { return {}; }

  /* Consolidated windowed RX link-quality snapshot (see RxQuality.h) — the
   * runtime feed a closed-loop adaptive-link controller reads instead of
   * scraping the demo's stdout. Fuses the per-frame RSSI/SNR/EVM aggregate the
   * device accumulates internally, a passive noise-floor estimate (rssi - snr,
   * the self-jamming signal), the frame-free FA/CCA/IGI energy, and the
   * LinkHealth verdict. Drains the window (delta semantics) and SUBSUMES
   * GetRxEnergy (it calls it internally + consumes the FA/CCA delta — don't also
   * poll GetRxEnergy separately on the same cadence). Default is an all-invalid
   * snapshot; each generation overrides. */
  virtual devourer::RxQuality GetRxQuality() { return {}; }

  /* --- Adapter-health probes (see src/AdapterHealth.h; examples/doctor is
   * the reference consumer) --- */

  /* Perform `reads` fresh PHYSICAL EFUSE logical-map reads (each pass re-runs
   * the efuse-controller read sequence — not the cached shadow) and
   * cross-compare them. Dying silicon returns different content per read;
   * healthy silicon is byte-identical every time. Post-bring-up only: returns
   * supported=false before Init/InitWrite (on the 8814AU a pre-fwdl EFUSE
   * read breaks the RSVD-page firmware download). Control-plane threading
   * contract applies (same as SetMonitorChannel). */
  virtual devourer::EfuseStability ProbeEfuseStability(int reads = 4) {
    (void)reads;
    return {};
  }

  /* Outcome of the most recent firmware download (populated during
   * Init/InitWrite). On Jaguar1 a failed FW boot does not abort bring-up —
   * this is the only place the failure is visible to a caller. */
  virtual devourer::FwBootStatus GetFwBootStatus() { return {}; }
};

#endif /* IRTL_DEVICE_H */
