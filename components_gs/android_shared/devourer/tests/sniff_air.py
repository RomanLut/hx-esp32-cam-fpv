#!/usr/bin/env python3
"""Capture frames on a monitor-mode interface and report what encoding was
actually on-air.

Companion to `tests/inject_beacon.py` + `tests/regress.py --encoding-matrix`:
when those tests show the same hit count across BCC / LDPC / STBC columns,
the question is "did the kernel TX path actually emit different encodings,
or did mac80211 strip the flags?". Run this script on a third adapter
(e.g. AR9271, which speaks vanilla radiotap and is widely used as a
sniffer), filter on the canonical injection SA, and decode the radiotap
of each captured frame to see what flew.

Usage:

    sudo python3 tests/sniff_air.py --iface wlan0mon --channel 100 \\
        --duration 30

Requires the iface to be plugged + driver-supported. The script will put
it into monitor mode on the chosen channel before starting tcpdump.

Output: per-encoding hit counts, e.g.

    captured 412 frames matching SA=57:42:75:05:d6:00 in 30s:
      HT MCS=1 BCC 20MHz STBC=0    300 frames (72.8%)
      HT MCS=1 LDPC 20MHz STBC=0   100 frames (24.3%)
      HT MCS=1 LDPC 20MHz STBC=1    12 frames (2.9%)

If a `--ldpc` injection is reported back as BCC, mac80211 / driver
stripped the bit before the air. That answers the question.
"""

import argparse
import struct
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path


# Source MAC matches the canonical beacon SA in txdemo/main.cpp and
# inject_beacon.py. Don't change without updating both.
CANONICAL_SA = bytes.fromhex("574275 05d600".replace(" ", ""))

# Radiotap field bits we care about.
_RT_TX_FLAGS = 1 << 15
_RT_MCS = 1 << 19
_RT_VHT = 1 << 21

# Field sizes in bytes for each radiotap presence bit. Source:
# https://www.radiotap.org/ — only fields we need are filled out; the rest
# are byte-counted so we can skip past them to reach MCS / VHT.
# Format: (size, alignment).
_RT_FIELDS = {
    0:  (8, 8),   # TSFT
    1:  (1, 1),   # Flags
    2:  (1, 1),   # Rate
    3:  (4, 2),   # Channel (u16 freq + u16 flags)
    4:  (2, 2),   # FHSS
    5:  (1, 1),   # Antenna signal
    6:  (1, 1),   # Antenna noise
    7:  (2, 2),   # Lock quality
    8:  (2, 2),   # TX attenuation
    9:  (2, 2),   # DB TX attenuation
    10: (1, 1),   # DBM TX power
    11: (1, 1),   # Antenna
    12: (1, 1),   # DB antenna signal
    13: (1, 1),   # DB antenna noise
    14: (2, 2),   # RX Flags
    15: (2, 2),   # TX Flags
    16: (1, 1),   # RTS retries
    17: (1, 1),   # Data retries
    18: (8, 4),   # XChannel (8 bytes, 4-byte aligned)
    19: (3, 1),   # MCS info
    20: (8, 4),   # A-MPDU
    21: (12, 2),  # VHT info
}


def _aligned(offset: int, align: int) -> int:
    return (offset + align - 1) & ~(align - 1)


def _parse_radiotap(frame: bytes):
    """Decode radiotap MCS (bit 19) / VHT (bit 21) info from one captured
    frame. Returns dict with keys: kind ('HT' | 'VHT' | 'legacy'), mcs,
    ldpc, stbc, bw, nss (where applicable)."""
    if len(frame) < 8:
        return None
    version, _pad, it_len = struct.unpack_from("<BBH", frame, 0)
    if version != 0 or it_len < 8 or it_len > len(frame):
        return None

    # Walk all it_present words (continue while ext bit set).
    presence: list[int] = []
    off = 4
    while off + 4 <= it_len:
        word = struct.unpack_from("<I", frame, off)[0]
        presence.append(word)
        off += 4
        if not (word & (1 << 31)):
            break

    # Fields start after the last it_present word; offset relative to frame.
    # Iterate bits 0..21 in the first word — the only one we know fixed
    # field sizes for. Anything in extension words we skip.
    cur = off
    word0 = presence[0] if presence else 0
    parsed = {}
    for bit in range(32):
        if not (word0 & (1 << bit)):
            continue
        if bit not in _RT_FIELDS:
            # Unknown field — can't safely skip past it. Bail.
            return None
        size, align = _RT_FIELDS[bit]
        cur = _aligned(cur, align)
        if cur + size > it_len:
            return None
        if bit == 19:  # MCS info
            mcs_known, mcs_flags, mcs_idx = struct.unpack_from(
                "<BBB", frame, cur,
            )
            parsed["mcs_known"] = mcs_known
            parsed["mcs_flags"] = mcs_flags
            parsed["mcs_idx"] = mcs_idx
        elif bit == 21:  # VHT info
            vht_known, vht_flags, vht_bw = struct.unpack_from(
                "<HBB", frame, cur,
            )
            vht_mcs_nss = frame[cur + 4:cur + 8]
            vht_coding = frame[cur + 8]
            parsed["vht_known"] = vht_known
            parsed["vht_flags"] = vht_flags
            parsed["vht_bw"] = vht_bw
            parsed["vht_mcs_nss"] = vht_mcs_nss
            parsed["vht_coding"] = vht_coding
        cur += size

    # Decide kind.
    if "vht_mcs_nss" in parsed:
        mcs = (parsed["vht_mcs_nss"][0] >> 4) & 0xF
        nss = parsed["vht_mcs_nss"][0] & 0xF
        bw_map = {0: 20, 1: 40, 4: 80, 11: 160}
        bw = bw_map.get(parsed["vht_bw"], parsed["vht_bw"])
        return {
            "kind": "VHT",
            "mcs": mcs,
            "nss": nss,
            "ldpc": bool(parsed["vht_coding"] & 0x01),
            "stbc": bool(parsed["vht_flags"] & 0x01),
            "bw": bw,
            "rt_len": it_len,
        }
    if "mcs_idx" in parsed:
        bw_lo = parsed["mcs_flags"] & 0x03
        bw = 40 if bw_lo == 1 else 20
        return {
            "kind": "HT",
            "mcs": parsed["mcs_idx"],
            "nss": 1,  # HT doesn't carry NSS in radiotap, default to 1
            "ldpc": bool(parsed["mcs_flags"] & 0x10),
            "stbc": (parsed["mcs_flags"] >> 5) & 0x3,
            "bw": bw,
            "rt_len": it_len,
        }
    return {"kind": "legacy", "rt_len": it_len}


def _frame_sa(frame: bytes) -> bytes | None:
    """Extract the 802.11 frame's SA (addr2) from a captured frame.
    Frame layout: radiotap header (variable, length in bytes 2-3 LE),
    then 802.11 frame starts with FC u16 / Duration u16 / addr1[6] /
    addr2[6] ..."""
    if len(frame) < 4:
        return None
    it_len = struct.unpack_from("<H", frame, 2)[0]
    sa_offset = it_len + 4 + 6  # skip rt, FC, Duration, addr1
    if sa_offset + 6 > len(frame):
        return None
    return frame[sa_offset:sa_offset + 6]


def _set_monitor(iface: str, channel: int) -> None:
    """Tear iface down, retype it to monitor, set channel, bring it up.
    Same sequence as regress.py's iface_to_monitor."""
    cmds = [
        ["ip", "link", "set", iface, "down"],
        ["iw", "dev", iface, "set", "type", "monitor"],
        ["ip", "link", "set", iface, "up"],
        ["iw", "dev", iface, "set", "channel", str(channel)],
    ]
    for c in cmds:
        subprocess.run(c, check=True)


def _capture(iface: str, duration: float, pcap_path: Path) -> int:
    """Run tcpdump filtered on the canonical SA for `duration` seconds.
    Writes a pcap to `pcap_path`. Returns frames-written count from tcpdump
    stderr ('N packets captured')."""
    sa_str = ":".join(f"{b:02x}" for b in CANONICAL_SA)
    proc = subprocess.run(
        ["timeout", "--signal=INT", "--kill-after=2", str(duration),
         "tcpdump", "-i", iface, "-w", str(pcap_path), "-U", "-nn",
         f"ether src {sa_str}"],
        capture_output=True, text=True,
    )
    # tcpdump prints "N packets captured" to stderr after exit on SIGINT.
    for line in (proc.stderr or "").splitlines():
        if "packets captured" in line:
            try:
                return int(line.split()[0])
            except ValueError:
                pass
    return 0


def _read_pcap_frames(pcap_path: Path):
    """Minimal pcap-savefile reader. Yields each frame's raw bytes.
    Avoids depending on scapy.rdpcap so this can run on hosts without
    the full scapy install."""
    with pcap_path.open("rb") as f:
        gh = f.read(24)
        if len(gh) != 24:
            return
        magic = struct.unpack_from("<I", gh, 0)[0]
        # 0xa1b2c3d4 microsecond, 0xa1b23c4d nanosecond — both LE here.
        if magic not in (0xA1B2C3D4, 0xA1B23C4D):
            return
        while True:
            hdr = f.read(16)
            if len(hdr) < 16:
                return
            _ts_sec, _ts_usec, incl_len, _orig_len = struct.unpack("<IIII", hdr)
            data = f.read(incl_len)
            if len(data) < incl_len:
                return
            yield data


def main():
    ap = argparse.ArgumentParser(
        description="Capture on a monitor iface and decode the radiotap "
                    "encoding of each frame matching the canonical SA.",
    )
    ap.add_argument("--iface", required=True,
                    help="monitor-capable iface (e.g. wlan9271 for AR9271)")
    ap.add_argument("--channel", type=int, required=True,
                    help="Wi-Fi channel to monitor on")
    ap.add_argument("--duration", type=float, default=30.0,
                    help="capture window in seconds (default 30)")
    ap.add_argument("--pcap-out", type=Path,
                    help="keep the raw pcap at this path (default: temp file)")
    args = ap.parse_args()

    _set_monitor(args.iface, args.channel)

    if args.pcap_out:
        pcap = args.pcap_out
    else:
        pcap = Path(tempfile.mkstemp(suffix=".pcap", prefix="sniff_air-")[1])

    print(f"capturing on {args.iface} ch{args.channel} for {args.duration:.0f}s "
          f"→ {pcap}")
    n_captured = _capture(args.iface, args.duration, pcap)

    buckets: Counter = Counter()
    total = 0
    parse_failed = 0
    for frame in _read_pcap_frames(pcap):
        sa = _frame_sa(frame)
        if sa != CANONICAL_SA:
            continue
        total += 1
        info = _parse_radiotap(frame)
        if info is None:
            parse_failed += 1
            continue
        if info["kind"] == "VHT":
            key = (f"VHT MCS={info['mcs']} NSS={info['nss']} "
                   f"{'LDPC' if info['ldpc'] else 'BCC'} "
                   f"{info['bw']}MHz STBC={int(info['stbc'])}")
        elif info["kind"] == "HT":
            key = (f"HT MCS={info['mcs']} "
                   f"{'LDPC' if info['ldpc'] else 'BCC'} "
                   f"{info['bw']}MHz STBC={int(info['stbc'])}")
        else:
            key = "legacy (no MCS / VHT field)"
        buckets[key] += 1

    print(f"\ncaptured {total} frames matching "
          f"SA={':'.join(f'{b:02x}' for b in CANONICAL_SA)} "
          f"({n_captured} hit the tcpdump filter; {parse_failed} radiotap "
          f"parse errors)")
    if total == 0:
        print("  (nothing — check channel, TX side running, iface up)")
        return 0
    for key, n in buckets.most_common():
        pct = 100.0 * n / total
        print(f"  {key:<48} {n:5d} frames ({pct:5.1f}%)")
    if not args.pcap_out:
        try:
            pcap.unlink()
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
