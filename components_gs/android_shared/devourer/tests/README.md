# devourer regression test rig

Cross-driver matrix test that compares this project's userspace stack
against the kernel-driver baseline for both TX and RX on plugged-in USB
Wi-Fi adapters.

```
                  TX = devourer       TX = kernel
RX = devourer     end-to-end devourer  does dvr RX a kernel-TX frame?
RX = kernel       does dvr emit valid  baseline / rig sanity check
                  frames?
```

Each cell injects/receives the canonical beacon (SA `57:42:75:05:d6:00`,
matching `txdemo/main.cpp`) for `--duration` seconds and counts hits.
The baseline cell runs first — if it fails the rig itself is broken and
the remaining cells are skipped (override with `--no-baseline-abort`).

## Two run modes

### Local mode

The kernel-side cells run against whatever driver is bound to the DUTs on
the **host** (mainline `rtw88_*` or whatever's loaded). Cheap to set up
but limited to drivers that build cleanly against the host kernel — that's
a moving target as kernels evolve, especially for the out-of-tree
`aircrack-ng/rtl8812au` driver.

```bash
sudo python3 tests/regress.py --channel 100
```

### VM mode (recommended)

The kernel-side cells run inside a **pinned-kernel libvirt VM** that has
the OOT `aircrack-ng/rtl8812au` driver built and loaded. DUTs are
transferred between host and VM per cell via `virsh attach-device` /
`detach-device`. The VM's kernel never moves so the driver never breaks.

Provision the VM once with the included script (Ubuntu 22.04 LTS,
kernel 5.15 — where aircrack-ng's driver builds without patches):

```bash
sudo tests/setup_vm.sh                    # provision; ~5-10 min
sudo tests/setup_vm.sh --status           # show VM IP, ssh hint
```

Then run the matrix in VM mode:

```bash
sudo python3 tests/regress.py --channel 100 \
    --vm-name devourer-testrig \
    --vm-ssh <user>@<VM-IP-from-status>
```

VM mode is what unblocks chipsets where the host kernel driver doesn't
work — e.g. RTL8814AU, where mainline `rtw88_8814au` currently fails to
probe on kernels 6.15+ (`failed to download firmware`, `error -22`), but
`aircrack-ng/rtl8812au` claims it cleanly on the pinned kernel 5.15.

## Prerequisites

### On the host (both modes)

- 2 supported USB Wi-Fi adapters plugged in
- devourer built (`build/WiFiDriverDemo`, `build/WiFiDriverTxDemo`)
- Python 3.9+ with `scapy` (`pip install scapy` or `python3-scapy`)
- `iw`, `tcpdump`, `ip` on PATH
- Passwordless `sudo`, or run directly as root

### For local mode

- Kernel driver(s) installed and `modprobe`-able for your DUTs (rtw88 or
  aircrack-ng — script auto-detects whatever's bound via sysfs)
- NetworkManager users: stop NM, or `nmcli device set <iface> managed no`
  on the test interfaces

### For VM mode (in addition)

- `libvirtd` + `virsh` + `virt-install` on the host
- `xorriso` (for the cloud-init seed ISO that `setup_vm.sh` generates)
- An Ubuntu 22.04 cloud image at `/var/lib/libvirt/images/jammy-base.qcow2`
  (download from <https://cloud-images.ubuntu.com/jammy/current/>)
- Working USB hot-plug on libvirt (`xhci` controller; `setup_vm.sh` adds it)
- The host user's SSH key in `~/.ssh/id_rsa.pub` (or set `SSH_PUBKEY=...`
  before `setup_vm.sh`) — gets baked into the VM's user account
  (defaults to your invoking user; override with `VM_USER=foo`)

The script does a preflight check and prints distro-agnostic install
hints for anything missing.

## Output

Markdown table to stdout, ready to paste into PR comments:

```
## Regression matrix — channel 100, 2026-05-23 13:22:14

- TX adapter: `0bda:8812` (RTL8812AU)
- RX adapter: `0bda:8813` (RTL8814AU)
- Kernel host: VM devourer-testrig via <user>@<VM-IP>
- Cell duration: 10s
- Pass threshold: ≥ 3 hits

|   | TX = devourer | TX = kernel |
|---|---|---|
| RX = devourer | 0 hits / 4500 TX ✗ | 0 hits / 258 TX ✗ |
| RX = kernel | 4172 hits / 4500 TX ✓ | 229 hits / 259 TX ✓ |
```

Pass/fail per cell on hit-count threshold (default ≥ 1 — generous because
air interference makes absolute counts unreliable). Bump for higher-
confidence runs on a quieter channel.

For debugging a specific cell that failed, re-run with `--keep-logs` —
per-cell stdout/stderr logs end up at `/tmp/devourer-regress-last/`.

## CLI knobs

- `--channel N` — Wi-Fi channel for both adapters (default 36; pick the
  channel your nearest AP is on for guaranteed traffic)
- `--duration SECONDS` — per-cell injection/measurement window (default 15)
- `--pass-threshold N` — min hits to pass (default 1)
- `--tx-pid 0xNNNN` / `--rx-pid 0xNNNN` — pick specific DUTs (defaults to
  the first two auto-detected)
- `--no-baseline-abort` — run all 4 cells even if kernel-kernel fails
  (useful when one chipset has no working kernel driver on this rig)
- `--vm-name NAME` / `--vm-ssh USER@HOST` — enter VM mode
- `--keep-logs` — symlink the temp log dir at `/tmp/devourer-regress-last`

Environment variable equivalents: `DEVOURER_VM_NAME`, `DEVOURER_VM_SSH`,
`DEVOURER_SNIFFER_IFACE`.

### `--sniffer-iface` — on-air encoding verification

When set, each matrix cell additionally captures on a third (host-local)
monitor iface and reports the decoded radiotap encoding distribution
alongside the hit count. Composes with `--full-matrix` and
`--encoding-matrix`. Intended for an AR9271 (vanilla radiotap, no
driver-side flag filtering).

```bash
sudo python3 tests/regress.py --encoding-matrix \
    --tx-pid 0x8813 --rx-pid 0x0120 --channel 100 \
    --vm-name devourer-testrig --vm-ssh <user>@<VM-IP> \
    --sniffer-iface wlan0mon
```

Per-cell output gains a `↪ sniffer: N frames — <encoding>=N, ...` line
showing what actually flew. If the `--ldpc` injection comes back tagged
as BCC, mac80211 / the OOT driver stripped the flag before the air —
the chip-side RX never had to refuse an LDPC frame, so any
`k/d`-row-flat result for LDPC means the test setup, not the chip.

The sniffer is host-only and never moved through the VM USB
passthrough. The same helper, run standalone outside the matrix, is
`tests/sniff_air.py` (see below).

## Specialized modes

The default invocation runs the 4-cell matrix on one ordered (TX, RX)
pair. Two additional modes extend coverage along different axes.

### `--full-matrix`: cross-chipset interop

Iterates every ordered (TX, RX) pair of plugged DUTs across all four
driver-side combinations and emits four NxN tables. For N adapters,
N×(N-1)×4 cells; ~16 min for N=3 in VM mode. Useful for catching
cross-chipset regressions in PRs that touch shared HAL code.

```bash
sudo python3 tests/regress.py --full-matrix --channel 100 \
    --vm-name devourer-testrig --vm-ssh <user>@<VM-IP>
```

### `--encoding-matrix`: chip-specific radiotap encoding asymmetries

For one ordered TX→RX pair, iterates (driver mode × radiotap encoding
flags). 16 cells per run (~10 min in VM mode). Designed to surface
chip-specific RX asymmetries that the default and full matrices miss
because they only exercise the chip's default encoding.

```bash
sudo python3 tests/regress.py --encoding-matrix \
    --tx-pid 0x8813 --rx-pid 0x0120 --channel 100 \
    --vm-name devourer-testrig --vm-ssh <user>@<VM-IP>
```

Encoding combos iterated by default — 6 cells per driver mode × 4 driver
modes = 24 cells total per run:

| Combo | Radiotap bit | Notes |
|---|---|---|
| `HT-BCC` | 19 (MCS info) | MCS 1, BCC FEC, 20 MHz |
| `HT-LDPC` | 19 | MCS 1, FEC=LDPC |
| `HT-STBC=1` | 19 | MCS 1, 1 STBC stream |
| `HT-LDPC+STBC` | 19 | MCS 1, FEC=LDPC + 1 STBC stream |
| `VHT-BCC` | 21 (VHT info) | VHT MCS 0, NSS 1, BCC, 20 MHz |
| `VHT-LDPC` | 21 | VHT MCS 0, NSS 1, FEC=LDPC |

VHT (802.11ac) coverage exists because some chips' LDPC decoder limitation
is on the VHT path only — the silicon's HT-LDPC and VHT-LDPC are separate
blocks. RTL8821AU is the motivating case (HT-LDPC tests pass, VHT-LDPC
reportedly fails at RX). Add more in `ENCODING_COMBOS` at the top of
`regress.py` if you need MCS 7, 40/80 MHz, or higher NSS.

The underlying knobs are also usable standalone for one-off targeted TX:

- **Devourer TX:** `DEVOURER_TX_MCS=N`, `DEVOURER_TX_LDPC=1`,
  `DEVOURER_TX_STBC=N`, `DEVOURER_TX_BW=20|40|80|160` env vars read by
  `WiFiDriverTxDemo`. Default mode is HT; `DEVOURER_TX_VHT=1` switches to
  a VHT radiotap header (22 bytes) and exposes `DEVOURER_TX_VHT_MCS=N` +
  `DEVOURER_TX_VHT_NSS=N`. `_LDPC` / `_STBC` / `_BW` apply to whichever
  mode is active.
- **Kernel-side scapy TX:** `--mcs N` / `--ldpc` / `--stbc N` / `--bandwidth
  20|40|80|160` on `tests/inject_beacon.py`, plus `--vht` / `--vht-mcs N`
  / `--vht-nss N` for VHT mode.

#### Caveat: kernel-TX encoding flags may not always reach the air

mac80211 + the `aircrack-ng/88XXau` driver don't necessarily honour the
radiotap MCS flags on TX — the chip's own rate-selection logic may
override them. So the `k/k` and `k/d` rows of the table reflect what
the *kernel* driver chose to transmit, which may collapse encoding
columns onto the chip's default.

To prove what actually flew, run `tests/sniff_air.py` on a third
adapter (AR9271 is the canonical sniffer — it speaks vanilla radiotap
without driver-side filtering) on the same channel, in parallel with
the matrix. Output reports each captured frame's decoded radiotap
MCS / VHT info, including LDPC bit + STBC streams. If a `--ldpc`
injection comes back tagged as BCC, the kernel stripped it before the
air.

```bash
sudo python3 tests/sniff_air.py --iface wlan0mon --channel 100 \
    --duration 60
```

The `d/k` and `d/d` rows are not affected by the kernel-TX caveat —
`WiFiDriverTxDemo` writes the radiotap header directly into the
chip's bulk-OUT buffer, so the `DEVOURER_TX_*` env vars are ground
truth for what flies on devourer TX.

## Supported DUTs

Listed in `SUPPORTED_DUTS` at the top of `regress.py`. Extend the dict
to add new chipsets — the rest of the script is chipset-agnostic.

## Architecture notes

- All kernel-side operations (modprobe / sysfs reads / `iw` / `tcpdump` /
  scapy) go through one abstraction (`KernelHost`). Local mode runs them
  via `subprocess.run`; VM mode wraps them in `ssh ... sudo`. Adding a
  third backend (e.g. remote bare-metal box) is a new class.
- DUT routing in VM mode uses `virsh attach-device` (USB hot-plug). The
  matrix moves DUTs between host and VM per cell as needed, restoring all
  DUTs to the host on exit so the next cell starts from a clean baseline.
- `inject_beacon.py` is shipped to the VM via `scp` each run (small file)
  and exits when its `--duration` elapses — orchestrator waits rather
  than killing, so the final "sent N frames" line is captured.

## Known limitations

- Tests "signal of life", not throughput — air noise makes absolute
  counts unreliable; pass-threshold is deliberately generous.
- Per matrix run: ~100s in local mode, ~3-4 min in VM mode (USB hot-plug
  adds ~5s per cell transition).
- Two-adapter scope today. To extend to >2, add a pairing loop in
  `main()` that runs the 4-cell matrix per chipset pair.
- VM mode assumes a single libvirt host running both `virsh` (locally)
  and the VM. Pulling the VM onto a different host is a `--vm-ssh
  user@vmhost` away on the kernel cell side, but `virsh attach-device`
  still runs locally; if the VM is on a different host, run virsh there
  (via your own wrapper).
- Cell 4 (`devourer-TX → devourer-RX`) requires both DUTs to be on the
  host and devourer-claimable simultaneously. Works fine, but means both
  chipsets need working devourer RX — if one is RX-broken (e.g. current
  RTL8814AU TODO), that cell will always show 0 hits regardless of TX.
