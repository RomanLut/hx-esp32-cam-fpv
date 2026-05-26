#!/usr/bin/env python3
"""Inject the canonical devourer TX-validation beacon on a kernel-driver
monitor interface.

The frame mirrors txdemo/main.cpp's hardcoded beacon: a probe request with
SA = 57:42:75:05:d6:00. WiFiDriverDemo and WiFiDriverTxDemo both grep for
this SA on RX (the `<devourer-tx-hit>` matcher) so the same beacon works
as the TX source whether the RX side is devourer or tcpdump.

Run from tests/regress.py's kernel-TX cell:

    sudo python3 tests/inject_beacon.py --iface wlpXX --count 500 --interval 0.002

Requires the iface to already be in monitor mode on the chosen channel
(regress.py sets that up). Idempotent: run multiple times safely.
"""

import argparse
import struct
import time

from scapy.all import RadioTap, Dot11, Raw, sendp

# Source MAC matches the canonical beacon SA in txdemo/main.cpp and the
# `<devourer-tx-hit>` matcher in demo/main.cpp. Don't change without
# updating both sides.
CANONICAL_SA = "57:42:75:05:d6:00"

# Radiotap field bits used here. Full list in
# https://www.radiotap.org/.
_RT_TX_FLAGS = 1 << 15
_RT_MCS = 1 << 19
_RT_VHT = 1 << 21


def _build_radiotap_mcs(*, mcs: int, ldpc: bool, stbc: int, bandwidth: int,
                        tx_flags: int = 0x0008) -> bytes:
    """Hand-rolled radiotap header with TX Flags + HT MCS info.

    Scapy's RadioTap layer doesn't expose first-class LDPC / STBC fields, and
    its `MCS` post-field plumbing is brittle across versions. Easier to emit
    the bytes ourselves and prepend them as Raw. Layout: u8 version, u8 pad,
    u16 it_len (LE), u32 it_present (LE), u16 TX Flags, u8 MCS known, u8 MCS
    flags, u8 MCS index. Total 13 bytes — matches txdemo/main.cpp's header.
    """
    known = 0x37  # bandwidth | mcs_idx | gi | fec_type | stbc known
    flags = 0
    if bandwidth == 40:
        flags |= 0x01  # bw bits 0..1: 00=20, 01=40, 10=20L, 11=20U
    if ldpc:
        flags |= 0x10  # bit 4: FEC type 0=BCC 1=LDPC
    flags |= (stbc & 0x3) << 5  # bits 5..6: STBC stream count 0..3
    return (
        struct.pack('<BBHIH', 0, 0, 13, _RT_TX_FLAGS | _RT_MCS, tx_flags)
        + bytes([known, flags, mcs & 0x7F])
    )


def _build_radiotap_vht(*, vht_mcs: int, nss: int, ldpc: bool, stbc: bool,
                        bandwidth: int, tx_flags: int = 0x0008) -> bytes:
    """Hand-rolled radiotap header with TX Flags + VHT info (radiotap bit 21).

    VHT is 802.11ac's encoding spec; required for testing chips like RTL8821AU
    whose LDPC behaviour at HT (bit 19) doesn't reflect their VHT path. Layout
    (22 bytes total): 8-byte header (version/pad/it_len/it_present), 2-byte TX
    Flags, then 12 bytes of VHT info — u16 known, u8 flags, u8 bandwidth,
    u8[4] mcs_nss (user 0..3 each = mcs<<4 | nss), u8 coding (LDPC nibble per
    user), u8 group_id, u16 partial_aid. Single-user only (users 1-3 zeroed).
    """
    bw_code = {20: 0, 40: 1, 80: 4, 160: 11}.get(bandwidth, 0)
    # known: bit 0 STBC, bit 2 GI, bit 6 bandwidth known.
    known = (1 << 0) | (1 << 2) | (1 << 6)
    flags = 0
    if stbc:
        flags |= 0x01  # bit 0: STBC enabled
    mcs_nss = bytes([((vht_mcs & 0xF) << 4) | (nss & 0xF), 0, 0, 0])
    coding = 0x01 if ldpc else 0x00  # user-0 nibble: 0=BCC, 1=LDPC
    vht_info = (
        struct.pack('<HBB', known, flags, bw_code)
        + mcs_nss
        + bytes([coding, 0])     # coding, group_id
        + struct.pack('<H', 0)   # partial_aid
    )
    assert len(vht_info) == 12, len(vht_info)
    return (
        struct.pack('<BBHIH', 0, 0, 22, _RT_TX_FLAGS | _RT_VHT, tx_flags)
        + vht_info
    )


def build_beacon(rate_mbps_x2: int = 0, *, mcs=None, ldpc: bool = False,
                 stbc: int = 0, bandwidth: int = 20, vht: bool = False,
                 vht_mcs: int = 0, nss: int = 1):
    """Mgmt / probe-request frame matching txdemo's beacon_frame[]. The body
    payload doesn't matter for hit-count testing — only SA is matched.

    `rate_mbps_x2` is in 500kbps units (the radiotap convention): 12 → 6Mbps
    OFDM, 2 → 1Mbps CCK, etc. 0 leaves the rate unspecified, which lets the
    chip pick its own default (varies by chipset and is the source of the
    8812-vs-8814 asymmetry we're investigating).

    Encoding selection:
      - default: legacy radiotap with optional `Rate`, chip picks encoding.
      - `mcs` / `ldpc` / `stbc` (without `vht`): HT radiotap (bit 19).
      - `vht=True`: VHT radiotap (bit 21); use `vht_mcs` / `nss` / `ldpc` /
        `stbc` / `bandwidth` to control. Default VHT MCS 0 / NSS 1 / 20 MHz.
        Required for testing chips whose LDPC RX limitation is on the VHT
        path (e.g. RTL8821AU's reported LDPC-RX-no per Eachine Sphere Link)."""
    dot11_bytes = bytes(
        Dot11(
            type=0,  # mgmt
            subtype=4,  # probe request
            addr1="ff:ff:ff:ff:ff:ff",  # DA broadcast
            addr2=CANONICAL_SA,  # SA — matched by RX side
            addr3=CANONICAL_SA,  # BSSID
        )
        / b"\x00\x00\x00\x00\x00\x00\x00\x00"  # ssid IE (empty)
    )
    if vht:
        rt_bytes = _build_radiotap_vht(
            vht_mcs=vht_mcs, nss=nss, ldpc=ldpc, stbc=bool(stbc),
            bandwidth=bandwidth,
        )
        return Raw(rt_bytes + dot11_bytes)
    if mcs is not None or ldpc or stbc:
        rt_bytes = _build_radiotap_mcs(
            mcs=mcs if mcs is not None else 1,
            ldpc=ldpc, stbc=stbc, bandwidth=bandwidth,
        )
        return Raw(rt_bytes + dot11_bytes)
    rt = RadioTap(Rate=rate_mbps_x2) if rate_mbps_x2 else RadioTap()
    return rt / Raw(dot11_bytes)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iface", required=True, help="monitor-mode wlan iface")
    ap.add_argument(
        "--duration",
        type=float,
        default=30.0,
        help="seconds to inject (default 30)",
    )
    ap.add_argument(
        "--interval",
        type=float,
        default=0.002,
        help="inter-frame gap seconds (default 0.002 = 500 fps, matches txdemo)",
    )
    ap.add_argument(
        "--rate",
        type=int,
        default=0,
        help="TX rate in 500kbps units (e.g. 12 = 6Mbps OFDM, 2 = 1Mbps CCK). "
             "0 (default) leaves it unspecified — chip picks its own default.",
    )
    ap.add_argument(
        "--mcs", type=int, default=None,
        help="HT MCS index (sets the MCS info radiotap field). Implied 1 if "
             "any of --ldpc / --stbc is set without an explicit --mcs.",
    )
    ap.add_argument(
        "--ldpc", action="store_true",
        help="Set MCS LDPC bit (FEC type = LDPC instead of BCC).",
    )
    ap.add_argument(
        "--stbc", type=int, default=0,
        help="STBC stream count, 0..3 (default 0 = no STBC).",
    )
    ap.add_argument(
        "--bandwidth", type=int, default=20, choices=(20, 40, 80, 160),
        help="bandwidth in MHz. HT honours 20/40; VHT honours 20/40/80/160.",
    )
    ap.add_argument(
        "--vht", action="store_true",
        help="emit VHT (802.11ac) radiotap (bit 21) instead of HT (bit 19). "
             "Combine with --ldpc / --stbc / --vht-mcs / --vht-nss / "
             "--bandwidth. Needed to test chips whose LDPC RX limitation is "
             "specifically on the VHT path (e.g. RTL8821AU).",
    )
    ap.add_argument(
        "--vht-mcs", type=int, default=0,
        help="VHT MCS index, 0..9 typical (default 0). Only used with --vht.",
    )
    ap.add_argument(
        "--vht-nss", type=int, default=1,
        help="VHT spatial streams (NSS), 1..4 (default 1). Only used with "
             "--vht.",
    )
    args = ap.parse_args()

    pkt = build_beacon(
        args.rate, mcs=args.mcs, ldpc=args.ldpc, stbc=args.stbc,
        bandwidth=args.bandwidth, vht=args.vht, vht_mcs=args.vht_mcs,
        nss=args.vht_nss,
    )
    end = time.monotonic() + args.duration
    sent = 0
    while time.monotonic() < end:
        try:
            sendp(pkt, iface=args.iface, verbose=False)
            sent += 1
        except OSError as e:
            # iface went down mid-test — bail rather than spin.
            print(f"inject_beacon: sendp failed after {sent} frames: {e}")
            break
        time.sleep(args.interval)
    print(f"inject_beacon: sent {sent} frames on {args.iface}")


if __name__ == "__main__":
    main()
