#!/usr/bin/env python3
"""Cross-driver regression matrix for devourer.

Runs a 4-cell test on a host with two compatible USB Wi-Fi adapters, comparing
this project's userspace stack ("devourer") against the kernel driver
(mainline rtw88 or aircrack-ng/rtl8812au) for both TX and RX:

                      TX = devourer        TX = kernel
    RX = devourer     [end-to-end dvr]     [does dvr RX kernel-TX frame?]
    RX = kernel       [does dvr emit       [baseline / rig sanity]
                       valid frames?]

Each cell injects/receives a known-SA beacon (57:42:75:05:d6:00, matching
the canonical frame in txdemo/main.cpp and the RX matcher in demo/main.cpp).
A cell passes if the RX side observes >= --pass-threshold hits within the
test duration. The baseline cell is run first; if it fails, the rig itself
is broken (interference, channel, antennas) and the matrix is aborted.

Two run modes:

  * Local mode (default): kernel cells run against the host kernel driver,
    devourer cells against host libusb. Both sides share the host. Cheap to
    set up but limited to whichever drivers build cleanly against the host
    kernel — that's a moving target as kernels evolve.

  * VM mode (--vm-name + --vm-ssh): kernel cells run inside a pinned-kernel
    libvirt VM that has the OOT aircrack-ng driver built and loaded. The
    VM's kernel never moves so the driver never breaks. DUTs are
    transferred between host and VM per cell via virsh USB hot-plug.
    Provision the VM with `tests/setup_vm.sh`.

Designed to be run manually after building devourer:

    cd /path/to/devourer && cmake --build build -j
    # Local mode:
    sudo python3 tests/regress.py --channel 100
    # VM mode (after tests/setup_vm.sh):
    sudo python3 tests/regress.py --channel 100 \\
        --vm-name devourer-testrig --vm-ssh <user>@<VM-IP>

Portability: tool paths resolved via `which`, wlan interfaces discovered via
`iw dev` (works for systemd `wlp*` and classic `wlan*`), kernel driver
claiming each DUT read from sysfs (no hardcoded module names). NetworkManager
users: stop NM for the duration of the test, or `nmcli device set <iface>
managed no` on the test interfaces.
"""

from __future__ import annotations

import argparse
import dataclasses
import glob
import os
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional

# Source MAC of the canonical beacon — must match txdemo/main.cpp and the
# `<devourer-tx-hit>` matcher in demo/main.cpp. tcpdump filter and scapy
# injector both use this.
CANONICAL_SA = "57:42:75:05:d6:00"

# Map every supported PID to the chipset family used for log readability.
# Detection of the kernel driver claiming the device is dynamic (via sysfs);
# this table is informational only.
SUPPORTED_DUTS = {
    "0bda:8812": "RTL8812AU",
    "0bda:8813": "RTL8814AU",
    "2357:0120": "RTL8821AU (TP-Link T2U Plus)",
    "0bda:0811": "RTL8811AU",
    "0bda:a811": "RTL8811AU",
    "0bda:b811": "RTL8811AU",
}

# Required external tools on the host. Each entry: (binary, install hint).
REQUIRED_TOOLS = [
    ("iw", "your distro's `iw` package"),
    ("tcpdump", "your distro's `tcpdump` package"),
    ("ip", "iproute2 (almost always preinstalled)"),
    ("modprobe", "kmod (almost always preinstalled)"),
]
# Python module deps (checked separately; scapy is the heavy one).
REQUIRED_PY_MODS = [
    ("scapy", "pip install scapy  # or your distro's `python3-scapy`"),
]


# ---------------------------------------------------------------------------
# Subprocess helpers.
# ---------------------------------------------------------------------------


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    """Run a command synchronously on the host, capturing output."""
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


# ---------------------------------------------------------------------------
# KernelHost — abstracts "the machine that runs the kernel-driver side".
#
# In local mode the kernel host IS the local machine. In VM mode it's a
# libvirt guest reached via SSH. The orchestrator only sees this interface;
# everything kernel-side (modprobe, sysfs reads, iw, tcpdump, scapy) goes
# through it.
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class KernelHost:
    """One of two flavours. Use KernelHost.local() or KernelHost.via_ssh()."""

    # ssh target like "<user>@<vm-ip>". Empty string for local execution.
    ssh_target: str = ""
    # libvirt domain name for USB passthrough. Empty for local mode (no DUT
    # movement needed — DUTs already on the same machine).
    vm_name: str = ""

    @classmethod
    def local(cls) -> "KernelHost":
        return cls(ssh_target="", vm_name="")

    @classmethod
    def via_ssh(cls, ssh_target: str, vm_name: str) -> "KernelHost":
        return cls(ssh_target=ssh_target, vm_name=vm_name)

    @property
    def is_remote(self) -> bool:
        return bool(self.ssh_target)

    # -- command execution ---------------------------------------------------

    def _ssh_prefix(self) -> list[str]:
        """ssh argv with auth set up. When the script is launched via sudo,
        use SUDO_USER's keys (root usually doesn't have keys provisioned
        on the VM)."""
        args = ["ssh", "-o", "BatchMode=yes",
                "-o", "StrictHostKeyChecking=accept-new"]
        sudo_user = os.environ.get("SUDO_USER")
        if sudo_user:
            sudo_home = os.path.expanduser(f"~{sudo_user}")
            for keyname in ("id_rsa", "id_ed25519", "id_ecdsa"):
                keypath = os.path.join(sudo_home, ".ssh", keyname)
                if os.path.exists(keypath):
                    args += ["-i", keypath]
                    break
        args.append(self.ssh_target)
        return args

    def run(self, cmd: list[str], **kw) -> subprocess.CompletedProcess:
        """Run a kernel-side command. Local: subprocess. Remote: ssh."""
        if self.is_remote:
            wrapped = self._ssh_prefix() + [
                "sudo " + " ".join(shlex.quote(c) for c in cmd)
            ]
            return run(wrapped, **kw)
        return run(cmd, **kw)

    def popen(self, cmd: list[str], **kw) -> subprocess.Popen:
        """Spawn a long-running kernel-side process. Returns Popen."""
        if self.is_remote:
            wrapped = self._ssh_prefix() + [
                "sudo " + " ".join(shlex.quote(c) for c in cmd)
            ]
            return subprocess.Popen(wrapped, start_new_session=True, **kw)
        return subprocess.Popen(cmd, start_new_session=True, **kw)

    # -- file I/O across hosts ----------------------------------------------

    def _scp_prefix(self) -> list[str]:
        args = ["scp", "-o", "BatchMode=yes",
                "-o", "StrictHostKeyChecking=accept-new"]
        sudo_user = os.environ.get("SUDO_USER")
        if sudo_user:
            sudo_home = os.path.expanduser(f"~{sudo_user}")
            for keyname in ("id_rsa", "id_ed25519", "id_ecdsa"):
                keypath = os.path.join(sudo_home, ".ssh", keyname)
                if os.path.exists(keypath):
                    args += ["-i", keypath]
                    break
        return args

    def push_file(self, local_path: Path, remote_path: str) -> None:
        """Copy a local file onto the kernel host. No-op in local mode."""
        if not self.is_remote:
            return
        scp_cmd = self._scp_prefix() + [
            str(local_path), f"{self.ssh_target}:{remote_path}",
        ]
        r = run(scp_cmd)
        if r.returncode != 0:
            raise RuntimeError(f"scp failed: {r.stderr.strip()}")

    def fetch_file(self, remote_path: str, local_path: Path) -> None:
        """Copy a remote file back to local. No-op in local mode (same file)."""
        if not self.is_remote:
            return
        scp_cmd = self._scp_prefix() + [
            f"{self.ssh_target}:{remote_path}", str(local_path),
        ]
        r = run(scp_cmd)
        if r.returncode != 0:
            local_path.write_text("")

    # -- DUT routing (VM passthrough) ---------------------------------------

    def _dut_already_attached(self, dut: "Dut") -> bool:
        """True if dut is currently passed through to this VM."""
        r = run(["sudo", "virsh", "dumpxml", self.vm_name])
        if r.returncode != 0:
            return False
        want_v = f"<vendor id='0x{dut.vid}'/>"
        want_p = f"<product id='0x{dut.pid}'/>"
        return want_v in r.stdout and want_p in r.stdout

    def take_dut(self, dut: "Dut") -> None:
        """Bring `dut` to this kernel host. Local: no-op (just unbind from
        host kernel driver if any). VM: idempotent virsh attach-device."""
        if not self.is_remote:
            return
        if self._dut_already_attached(dut):
            return
        # Make sure the host kernel isn't holding it.
        detach_from_host_kernel(dut)
        xml = (
            "<hostdev mode='subsystem' type='usb' managed='yes'>"
            f"<source><vendor id='0x{dut.vid}'/><product id='0x{dut.pid}'/>"
            "</source></hostdev>"
        )
        with tempfile.NamedTemporaryFile("w", suffix=".xml", delete=False) as f:
            f.write(xml)
            xml_path = f.name
        try:
            r = run(["sudo", "virsh", "attach-device", self.vm_name,
                     xml_path, "--live"])
            if r.returncode != 0:
                raise RuntimeError(
                    f"virsh attach-device {dut.vidpid} → {self.vm_name} "
                    f"failed: {(r.stderr or r.stdout).strip()}"
                )
        finally:
            os.unlink(xml_path)

    def release_all_known_duts(self, duts: list["Dut"]) -> None:
        """At script start, ensure no DUTs are leftover-attached to the VM
        from a previous run. Called once before the matrix."""
        if not self.is_remote:
            return
        for dut in duts:
            if self._dut_already_attached(dut):
                self.release_dut(dut)
        time.sleep(2.0)

    def release_dut(self, dut: "Dut") -> None:
        """Send `dut` back to the host. Local: no-op. VM: virsh detach."""
        if not self.is_remote:
            return
        xml = (
            "<hostdev mode='subsystem' type='usb' managed='yes'>"
            f"<source><vendor id='0x{dut.vid}'/><product id='0x{dut.pid}'/>"
            "</source></hostdev>"
        )
        with tempfile.NamedTemporaryFile("w", suffix=".xml", delete=False) as f:
            f.write(xml)
            xml_path = f.name
        try:
            run(["sudo", "virsh", "detach-device", self.vm_name,
                 xml_path, "--live"])
            # Don't raise on detach errors — device may already be gone.
        finally:
            os.unlink(xml_path)

    # -- wlan iface discovery ------------------------------------------------

    def wait_for_wlan_iface(self, dut: "Dut", timeout: float = 20.0) -> str:
        """Block until a wlan iface for `dut` appears on the kernel host."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            iface = self._wlan_iface_for_dut(dut)
            if iface:
                return iface
            time.sleep(0.5)
        raise RuntimeError(
            f"no wlan iface appeared for {dut.vidpid} on "
            f"{self.ssh_target or 'local'} after {timeout}s — "
            f"kernel driver may have failed to bind"
        )

    def _wlan_iface_for_dut(self, dut: "Dut") -> Optional[str]:
        if not self.is_remote:
            # Local mode: walk /sys/bus/usb/devices for the DUT, then
            # look at <iface_id>/net/ for a wlan name.
            for d in glob.glob("/sys/bus/usb/devices/*"):
                try:
                    with open(f"{d}/idVendor") as f:
                        if f.read().strip() != dut.vid:
                            continue
                    with open(f"{d}/idProduct") as f:
                        if f.read().strip() != dut.pid:
                            continue
                except (FileNotFoundError, PermissionError):
                    continue
                net_dir = f"{d}:1.0/net"
                if not os.path.isdir(net_dir):
                    return None
                ifaces = os.listdir(net_dir)
                return ifaces[0] if ifaces else None
            return None
        # Remote: ssh and iterate /sys/bus/usb/devices/ over there.
        r = self.run([
            "sh", "-c",
            "for d in /sys/bus/usb/devices/*; do "
            "  [ -e \"$d/idVendor\" ] || continue; "
            f"  [ \"$(cat $d/idVendor)\" = \"{dut.vid}\" ] || continue; "
            f"  [ \"$(cat $d/idProduct)\" = \"{dut.pid}\" ] || continue; "
            "  ls \"$d:1.0/net\" 2>/dev/null | head -1; "
            "  break; "
            "done",
        ])
        out = (r.stdout or "").strip()
        return out or None

    def iface_to_monitor(self, iface: str, channel: int) -> None:
        """Put a wlan iface into monitor mode on a channel."""
        def _go(cmd):
            r = self.run(cmd)
            if r.returncode != 0:
                raise RuntimeError(
                    f"{' '.join(cmd)} exit={r.returncode}: "
                    f"{(r.stderr or r.stdout).strip() or '(no stderr)'}"
                )
        _go(["ip", "link", "set", iface, "down"])
        _go(["iw", "dev", iface, "set", "type", "monitor"])
        _go(["ip", "link", "set", iface, "up"])
        _go(["iw", "dev", iface, "set", "channel", str(channel)])


# ---------------------------------------------------------------------------
# Preflight — check prerequisites with friendly errors.
# ---------------------------------------------------------------------------


def preflight(devourer_root: Path, kh: KernelHost) -> None:
    """Bail early if anything required is missing."""
    missing = []
    for tool, hint in REQUIRED_TOOLS:
        if shutil.which(tool) is None:
            missing.append(f"  - command `{tool}` not on PATH (install: {hint})")
    for mod, hint in REQUIRED_PY_MODS:
        try:
            __import__(mod)
        except ImportError:
            missing.append(f"  - python module `{mod}` not importable "
                           f"(install: {hint})")
    for binary in ("WiFiDriverDemo", "WiFiDriverTxDemo"):
        if not (devourer_root / "build" / binary).is_file():
            missing.append(
                f"  - devourer binary `build/{binary}` missing — run "
                f"`cmake -S {devourer_root} -B {devourer_root}/build && "
                f"cmake --build {devourer_root}/build -j`"
            )
    if os.geteuid() != 0:
        missing.append(
            "  - this script needs root on the host (modprobe / sysfs writes). "
            "Re-run with `sudo`."
        )
    if kh.is_remote:
        # Sanity check ssh + sudo on the VM works.
        r = kh.run(["true"])
        if r.returncode != 0:
            missing.append(
                f"  - kernel host `{kh.ssh_target}` not reachable via "
                f"`ssh ... sudo true`: {(r.stderr or r.stdout).strip()}"
            )
        else:
            for tool in ("iw", "tcpdump", "ip", "modprobe", "python3"):
                r = kh.run(["which", tool])
                if r.returncode != 0:
                    missing.append(
                        f"  - kernel-host `{kh.ssh_target}` is missing "
                        f"`{tool}` — install inside the VM."
                    )
        # virsh on local host (we drive virsh from here).
        if shutil.which("virsh") is None:
            missing.append(
                "  - VM mode needs `virsh` on the local host (libvirt-clients)."
            )
    if missing:
        sys.stderr.write("Prerequisites not met:\n" + "\n".join(missing) + "\n")
        sys.exit(2)


# ---------------------------------------------------------------------------
# DUT discovery — find plugged-in adapters via sysfs (host side only;
# the VM sees DUTs only when we explicitly hand them over via virsh).
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class Dut:
    sysfs_id: str  # e.g. "1-14" (the USB device path component on host)
    vid: str  # lowercase hex, e.g. "0bda"
    pid: str  # lowercase hex, e.g. "8812"
    chipset: str  # human-readable

    @property
    def vidpid(self) -> str:
        return f"{self.vid}:{self.pid}"

    @property
    def iface_id(self) -> str:
        """Interface address sysfs expects for bind/unbind."""
        return f"{self.sysfs_id}:1.0"


def discover_duts() -> list[Dut]:
    duts: list[Dut] = []
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            with open(f"{d}/idVendor") as f:
                vid = f.read().strip()
            with open(f"{d}/idProduct") as f:
                pid = f.read().strip()
        except (FileNotFoundError, IsADirectoryError, PermissionError):
            continue
        key = f"{vid}:{pid}"
        if key in SUPPORTED_DUTS:
            duts.append(
                Dut(
                    sysfs_id=os.path.basename(d),
                    vid=vid,
                    pid=pid,
                    chipset=SUPPORTED_DUTS[key],
                )
            )
    return duts


def host_kernel_driver_for_dut(dut: Dut) -> Optional[str]:
    """Return the kernel driver currently bound to dut on the HOST. None if
    nothing bound (e.g. because the DUT is currently passed through to a VM)."""
    link = f"/sys/bus/usb/devices/{dut.iface_id}/driver"
    if not os.path.islink(link):
        return None
    return os.path.basename(os.readlink(link))


def detach_from_host_kernel(dut: Dut) -> None:
    """Unbind dut from whatever host kernel driver claims it. No-op if
    nothing claims it."""
    drv = host_kernel_driver_for_dut(dut)
    if drv is None:
        return
    try:
        with open(f"/sys/bus/usb/drivers/{drv}/unbind", "w") as f:
            f.write(dut.iface_id)
    except OSError as e:
        sys.stderr.write(f"detach_from_host_kernel({dut.iface_id}): {e}\n")


def attach_to_host_kernel(dut: Dut) -> None:
    """Re-bind dut to whatever host kernel driver matches it (via modalias)."""
    try:
        with open("/sys/bus/usb/drivers_probe", "w") as f:
            f.write(dut.iface_id)
    except OSError as e:
        sys.stderr.write(f"attach_to_host_kernel({dut.iface_id}): {e}\n")


# ---------------------------------------------------------------------------
# Cell results + scoring.
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class CellResult:
    hits: int
    tx_attempts: int
    tx_failures: int
    duration_s: float
    notes: str = ""

    def passed(self, threshold: int) -> bool:
        return self.hits >= threshold

    def fmt(self, threshold: int) -> str:
        mark = "✓" if self.passed(threshold) else "✗"
        attempts_str = f"{self.tx_attempts} TX"
        if self.tx_failures > 0:
            attempts_str += f" ({self.tx_failures} fail)"
        return f"{self.hits} hits / {attempts_str} / {self.duration_s:.0f}s {mark}"


# ---------------------------------------------------------------------------
# Process spawners — devourer cells (always local) + kernel cells (via kh).
# ---------------------------------------------------------------------------


def _devourer_env(dut: Dut, channel: int,
                  tx_encoding: Optional[dict] = None) -> dict[str, str]:
    env = os.environ.copy()
    env["DEVOURER_VID"] = f"0x{dut.vid}"
    env["DEVOURER_PID"] = f"0x{dut.pid}"
    env["DEVOURER_CHANNEL"] = str(channel)
    env["DEVOURER_USB_QUIET"] = "1"
    if tx_encoding:
        # DEVOURER_TX_* knobs are read by txdemo/main.cpp to patch the
        # radiotap MCS info bytes before the TX loop. Only meaningful when
        # spawning WiFiDriverTxDemo; harmless on the RX side.
        if tx_encoding.get("mcs") is not None:
            env["DEVOURER_TX_MCS"] = str(tx_encoding["mcs"])
        if tx_encoding.get("ldpc"):
            env["DEVOURER_TX_LDPC"] = "1"
        if tx_encoding.get("stbc"):
            env["DEVOURER_TX_STBC"] = str(tx_encoding["stbc"])
        if tx_encoding.get("bandwidth") is not None:
            env["DEVOURER_TX_BW"] = str(tx_encoding["bandwidth"])
        if tx_encoding.get("vht"):
            env["DEVOURER_TX_VHT"] = "1"
            if tx_encoding.get("vht_mcs") is not None:
                env["DEVOURER_TX_VHT_MCS"] = str(tx_encoding["vht_mcs"])
            if tx_encoding.get("nss") is not None:
                env["DEVOURER_TX_VHT_NSS"] = str(tx_encoding["nss"])
    return env


def _spawn_devourer_rx(
    devourer_root: Path, dut: Dut, channel: int, log_path: Path
) -> subprocess.Popen:
    fh = open(log_path, "w")
    return subprocess.Popen(
        [str(devourer_root / "build" / "WiFiDriverDemo")],
        env=_devourer_env(dut, channel),
        stdout=fh, stderr=subprocess.STDOUT, start_new_session=True,
    )


def _spawn_devourer_tx(
    devourer_root: Path, dut: Dut, channel: int, log_path: Path,
    encoding: Optional[dict] = None,
) -> subprocess.Popen:
    fh = open(log_path, "w")
    return subprocess.Popen(
        [str(devourer_root / "build" / "WiFiDriverTxDemo")],
        env=_devourer_env(dut, channel, tx_encoding=encoding),
        stdout=fh, stderr=subprocess.STDOUT, start_new_session=True,
    )


def _spawn_kernel_rx(
    kh: KernelHost, iface: str, channel: int, log_path: Path
) -> subprocess.Popen:
    """RX side: tcpdump on the kernel host's wlan iface, filter on
    CANONICAL_SA. Local mode writes to `log_path` directly; VM mode writes
    to /tmp/dvr-tcpdump.log on the VM and the parser fetches it later."""
    kh.iface_to_monitor(iface, channel)
    fh = open(log_path, "w")
    if kh.is_remote:
        # Use the VM's /tmp for stdout, fetch it after termination.
        cmd = ["sh", "-c",
               f"tcpdump -i {shlex.quote(iface)} -e -nn -l "
               f"ether src {CANONICAL_SA} 2>/dev/null"]
    else:
        cmd = ["tcpdump", "-i", iface, "-e", "-nn", "-l",
               "ether", "src", CANONICAL_SA]
    return kh.popen(cmd, stdout=fh, stderr=subprocess.DEVNULL)


def _spawn_kernel_tx(
    kh: KernelHost, devourer_root: Path, iface: str, channel: int,
    duration: float, log_path: Path, encoding: Optional[dict] = None,
) -> subprocess.Popen:
    """TX side: scapy injector that emits the canonical beacon.
    Local mode runs tests/inject_beacon.py directly. VM mode scps it over
    first, then ssh-runs it."""
    kh.iface_to_monitor(iface, channel)
    fh = open(log_path, "w")
    injector = devourer_root / "tests" / "inject_beacon.py"
    extra: list[str] = []
    if encoding:
        if encoding.get("mcs") is not None:
            extra += ["--mcs", str(encoding["mcs"])]
        if encoding.get("ldpc"):
            extra.append("--ldpc")
        if encoding.get("stbc"):
            extra += ["--stbc", str(encoding["stbc"])]
        if encoding.get("bandwidth") is not None:
            extra += ["--bandwidth", str(encoding["bandwidth"])]
        if encoding.get("vht"):
            extra.append("--vht")
            if encoding.get("vht_mcs") is not None:
                extra += ["--vht-mcs", str(encoding["vht_mcs"])]
            if encoding.get("nss") is not None:
                extra += ["--vht-nss", str(encoding["nss"])]
    if kh.is_remote:
        # Ship the injector to the VM (overwrites each run — fine for the
        # tiny script).
        kh.push_file(injector, "/tmp/inject_beacon.py")
        cmd = ["python3", "/tmp/inject_beacon.py",
               "--iface", iface, "--duration", str(duration)] + extra
    else:
        cmd = [sys.executable, str(injector),
               "--iface", iface, "--duration", str(duration)] + extra
    return kh.popen(cmd, stdout=fh, stderr=subprocess.STDOUT)


def _spawn_sniffer(
    iface: str, channel: int, pcap_path: Path,
) -> subprocess.Popen:
    """Optional 3rd-adapter on-air verifier. Puts `iface` in monitor mode
    on `channel` and runs tcpdump → pcap filtered on the canonical SA.
    Lets sniff_air's radiotap parser later report what encoding actually
    flew (vs what inject_beacon.py / txdemo *requested*). Host-side only;
    sniffer adapters don't get moved into the VM. AR9271 is the canonical
    sniffer chip — vanilla radiotap, no driver-side flag filtering."""
    # Reuse the local iface_to_monitor logic via subprocess (matches the
    # Local-mode path of KernelHost.iface_to_monitor; we don't go through
    # the kh abstraction because the sniffer never leaves the host).
    for c in [
        ["ip", "link", "set", iface, "down"],
        ["iw", "dev", iface, "set", "type", "monitor"],
        ["ip", "link", "set", iface, "up"],
        ["iw", "dev", iface, "set", "channel", str(channel)],
    ]:
        subprocess.run(c, check=False)
    fh = open(pcap_path.with_suffix(".tcpdump.log"), "w")
    sa_str = ":".join(
        f"{b:02x}" for b in bytes.fromhex(CANONICAL_SA.replace(":", ""))
    )
    return subprocess.Popen(
        ["tcpdump", "-i", iface, "-w", str(pcap_path), "-U", "-nn",
         f"ether src {sa_str}"],
        stdout=fh, stderr=subprocess.STDOUT, start_new_session=True,
    )


def _summarise_sniffer_pcap(pcap_path: Path) -> str:
    """Parse the captured pcap and return a one-line summary of encoding
    distribution. Returns an empty string if the pcap is missing or empty —
    callers should treat that as "no on-air data" rather than a failure."""
    try:
        # Local import: sniff_air sits next to regress.py.
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import sniff_air  # type: ignore
    except ImportError:
        return ""
    if not pcap_path.exists() or pcap_path.stat().st_size == 0:
        return ""
    from collections import Counter
    buckets: Counter = Counter()
    total = 0
    for frame in sniff_air._read_pcap_frames(pcap_path):
        sa = sniff_air._frame_sa(frame)
        if sa != sniff_air.CANONICAL_SA:
            continue
        total += 1
        info = sniff_air._parse_radiotap(frame)
        if info is None:
            buckets["parse-error"] += 1
            continue
        if info["kind"] == "VHT":
            buckets[f"VHT MCS{info['mcs']}/NSS{info['nss']} "
                    f"{'LDPC' if info['ldpc'] else 'BCC'} "
                    f"{info['bw']}MHz STBC={int(info['stbc'])}"] += 1
        elif info["kind"] == "HT":
            buckets[f"HT MCS{info['mcs']} "
                    f"{'LDPC' if info['ldpc'] else 'BCC'} "
                    f"{info['bw']}MHz STBC={int(info['stbc'])}"] += 1
        else:
            buckets["legacy"] += 1
    if total == 0:
        return "sniffer: 0 frames matched"
    parts = [
        f"{label}={n}" for label, n in buckets.most_common(3)
    ]
    return f"sniffer: {total} frames — " + ", ".join(parts)


def _terminate(proc: subprocess.Popen, grace: float = 2.0) -> None:
    if proc.poll() is not None:
        return
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGINT)
        proc.wait(timeout=grace)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait()


# ---------------------------------------------------------------------------
# Log parsers.
# ---------------------------------------------------------------------------


def _count_devourer_rx_hits(log_path: Path) -> int:
    last = 0
    try:
        for line in log_path.read_text().splitlines():
            if "<devourer-tx-hit>" in line:
                for tok in line.split():
                    if tok.startswith("hits="):
                        try:
                            last = max(last, int(tok.split("=", 1)[1]))
                        except ValueError:
                            pass
    except FileNotFoundError:
        pass
    return last


def _count_devourer_tx_attempts(log_path: Path) -> tuple[int, int]:
    """Returns (max_tx_count_logged, send_failures). The print is rate-limited
    so when sends fail often, max_tx_count_logged stays low — surface failure
    count alongside so the picture is honest."""
    last = 0
    failures = 0
    try:
        for line in log_path.read_text().splitlines():
            if line.startswith("<devourer-tx>TX #"):
                tok = line.split("#", 1)[1].split()[0]
                try:
                    last = max(last, int(tok))
                except ValueError:
                    pass
            elif "Failed to send packet" in line:
                failures += 1
    except FileNotFoundError:
        pass
    return last, failures


def _count_tcpdump_hits(log_path: Path) -> int:
    try:
        return sum(1 for _ in log_path.read_text().splitlines())
    except FileNotFoundError:
        return 0


def _count_kernel_tx_sent(log_path: Path) -> int:
    """inject_beacon.py emits `inject_beacon: sent N frames on IFACE` at exit."""
    try:
        for line in log_path.read_text().splitlines():
            if line.startswith("inject_beacon: sent "):
                return int(line.split()[2])
    except (FileNotFoundError, ValueError):
        pass
    return 0


# ---------------------------------------------------------------------------
# Cell orchestration.
# ---------------------------------------------------------------------------


def _ensure_dut_location(
    dut: Dut, want_at_kernel_host: bool, kh: KernelHost
) -> None:
    """Move `dut` to the right machine for the current cell.

    Local mode: nothing to physically move — DUT is always on the host —
    but we do need to detach it from any kernel driver before devourer can
    claim it.

    VM mode: when `want_at_kernel_host` is True, virsh attach-device to the
    VM. When False, virsh detach-device back to the host, then unbind from
    whatever host kernel driver claims it (so devourer can libusb-claim).
    """
    if not kh.is_remote:
        # Local mode: ALWAYS detach from kernel before devourer use.
        if not want_at_kernel_host:
            detach_from_host_kernel(dut)
        else:
            # Need kernel-bound on host. Ensure it's bound (re-probe).
            attach_to_host_kernel(dut)
        return
    # VM mode.
    if want_at_kernel_host:
        kh.take_dut(dut)
        # Give the VM kernel time to enumerate and bind.
        time.sleep(3.0)
    else:
        kh.release_dut(dut)
        # Wait for the device to reappear on the host bus.
        time.sleep(2.0)
        # Then detach from host kernel driver so devourer can libusb-claim it.
        detach_from_host_kernel(dut)


def run_cell(
    devourer_root: Path,
    tx_dut: Dut,
    rx_dut: Dut,
    tx_side: str,  # "devourer" | "kernel"
    rx_side: str,  # "devourer" | "kernel"
    channel: int,
    duration: float,
    tmpdir: Path,
    kh: KernelHost,
    encoding: Optional[dict] = None,
    sniffer_iface: Optional[str] = None,
) -> CellResult:
    """Run one matrix cell. State contract: always restore DUTs to a clean
    baseline (host kernel-bound) on exit via try/finally.

    `encoding` (optional dict, used by --encoding-matrix) passes radiotap
    TX encoding flags through to the TX-side spawn: kernel TX → injector
    CLI args, devourer TX → DEVOURER_TX_* env vars.

    `sniffer_iface` (optional, --sniffer-iface) puts a 3rd-adapter monitor
    iface into capture alongside the cell. The pcap is parsed at cell-end
    via sniff_air's radiotap decoder and a one-line summary appended to
    the CellResult.notes. Lets the matrix prove what encoding actually
    flew, vs what inject_beacon.py / txdemo *requested*."""
    cell_id = f"tx-{tx_side}_rx-{rx_side}"
    tx_log = tmpdir / f"{cell_id}.tx.log"
    rx_log = tmpdir / f"{cell_id}.rx.log"
    sniffer_pcap = tmpdir / f"{cell_id}.sniffer.pcap" if sniffer_iface else None

    # Stage 1: route DUTs to their target machines for this cell.
    _ensure_dut_location(tx_dut, want_at_kernel_host=(tx_side == "kernel"), kh=kh)
    _ensure_dut_location(rx_dut, want_at_kernel_host=(rx_side == "kernel"), kh=kh)

    rx_proc: Optional[subprocess.Popen] = None
    tx_proc: Optional[subprocess.Popen] = None
    sniffer_proc: Optional[subprocess.Popen] = None
    try:
        # Stage 2: bring up RX side first so it's listening when TX begins.
        if rx_side == "devourer":
            rx_proc = _spawn_devourer_rx(devourer_root, rx_dut, channel, rx_log)
            # devourer ~5s for fwdl + channel set.
            time.sleep(6.0)
        else:
            rx_iface = kh.wait_for_wlan_iface(rx_dut)
            rx_proc = _spawn_kernel_rx(kh, rx_iface, channel, rx_log)
            time.sleep(1.0)

        # Stage 2b: 3rd-adapter sniffer (optional, host-local). Starts
        # before TX so it captures the full window.
        if sniffer_iface and sniffer_pcap is not None:
            try:
                sniffer_proc = _spawn_sniffer(
                    sniffer_iface, channel, sniffer_pcap
                )
                time.sleep(0.5)
            except Exception as e:
                # Sniffer is observational — never fail the cell on
                # sniffer issues, just record and move on.
                print(f"  ! sniffer failed to start: {e}", flush=True)
                sniffer_proc = None

        # Stage 3: TX side.
        if tx_side == "devourer":
            tx_proc = _spawn_devourer_tx(
                devourer_root, tx_dut, channel, tx_log, encoding=encoding
            )
            tx_warmup = 6.0
        else:
            tx_iface = kh.wait_for_wlan_iface(tx_dut)
            tx_proc = _spawn_kernel_tx(
                kh, devourer_root, tx_iface, channel, duration, tx_log,
                encoding=encoding,
            )
            tx_warmup = 0.5

        time.sleep(tx_warmup)
        measure_start = time.monotonic()
        if tx_side == "kernel":
            # inject_beacon.py self-terminates after its --duration. Wait for
            # it instead of killing — otherwise we lose the final "sent N
            # frames" line and the TX-count parser sees 0. Generous margin
            # for ssh setup + scapy import.
            try:
                tx_proc.wait(timeout=duration + 15)
            except subprocess.TimeoutExpired:
                pass  # fall through to _terminate
        else:
            # devourer TX loops forever — explicit kill after `duration`.
            time.sleep(duration)
        measure_end = time.monotonic()

        # Stage 4: shut down, drain, parse.
        _terminate(tx_proc)
        time.sleep(1.0)
        _terminate(rx_proc)
        if sniffer_proc is not None:
            _terminate(sniffer_proc)

        if rx_side == "devourer":
            hits = _count_devourer_rx_hits(rx_log)
        else:
            hits = _count_tcpdump_hits(rx_log)
        if tx_side == "devourer":
            tx_attempts, tx_failures = _count_devourer_tx_attempts(tx_log)
        else:
            tx_attempts = _count_kernel_tx_sent(tx_log)
            tx_failures = 0

        notes = ""
        if sniffer_pcap is not None:
            notes = _summarise_sniffer_pcap(sniffer_pcap)

        return CellResult(
            hits=hits,
            tx_attempts=tx_attempts,
            tx_failures=tx_failures,
            duration_s=measure_end - measure_start,
            notes=notes,
        )
    finally:
        # Restore clean baseline so the next cell starts from a known state.
        if tx_proc is not None and tx_proc.poll() is None:
            _terminate(tx_proc)
        if rx_proc is not None and rx_proc.poll() is None:
            _terminate(rx_proc)
        if sniffer_proc is not None and sniffer_proc.poll() is None:
            _terminate(sniffer_proc)
        # Pull DUTs back to the host (so the next cell can choose freely).
        if kh.is_remote:
            kh.release_dut(tx_dut)
            kh.release_dut(rx_dut)
            time.sleep(1.5)
        # Re-attach to host kernel where applicable.
        attach_to_host_kernel(tx_dut)
        attach_to_host_kernel(rx_dut)


# ---------------------------------------------------------------------------
# Matrix.
# ---------------------------------------------------------------------------


def run_matrix(
    devourer_root: Path,
    tx_dut: Dut,
    rx_dut: Dut,
    channel: int,
    duration: float,
    threshold: int,
    tmpdir: Path,
    kh: KernelHost,
    abort_on_baseline_fail: bool = True,
    sniffer_iface: Optional[str] = None,
) -> dict[tuple[str, str], CellResult]:
    cells = [
        ("kernel", "kernel"),       # baseline — rig sanity
        ("devourer", "kernel"),     # does devourer emit valid frames?
        ("kernel", "devourer"),     # does devourer RX a known-good frame?
        ("devourer", "devourer"),   # end-to-end devourer
    ]
    results: dict[tuple[str, str], CellResult] = {}
    for tx_side, rx_side in cells:
        cell_label = f"TX={tx_side:>8s}  RX={rx_side:>8s}"
        print(f"[{time.strftime('%H:%M:%S')}] running cell {cell_label} ...",
              flush=True)
        try:
            r = run_cell(
                devourer_root, tx_dut, rx_dut, tx_side, rx_side,
                channel, duration, tmpdir, kh,
                sniffer_iface=sniffer_iface,
            )
        except Exception as e:
            print(f"  ✗ cell crashed: {e}", flush=True)
            r = CellResult(hits=0, tx_attempts=0, tx_failures=0,
                           duration_s=0.0, notes=str(e))
        results[(tx_side, rx_side)] = r
        print(f"  → {r.fmt(threshold)}", flush=True)
        if r.notes and sniffer_iface:
            print(f"  ↪ {r.notes}", flush=True)
        if (
            (tx_side, rx_side) == ("kernel", "kernel")
            and not r.passed(threshold)
            and abort_on_baseline_fail
        ):
            print(
                "BASELINE cell failed — the rig isn't moving frames. Aborting "
                "remaining cells. Re-run with --no-baseline-abort to attempt "
                "the rest anyway (useful when one chipset has no working "
                "kernel driver on this rig).",
                file=sys.stderr, flush=True,
            )
            for remaining in cells[1:]:
                results[remaining] = CellResult(
                    hits=0, tx_attempts=0, tx_failures=0,
                    duration_s=0.0, notes="skipped (baseline failed)",
                )
            break
    return results


# ---------------------------------------------------------------------------
# N-adapter full matrix — runs every ordered (TX, RX) pair across all 4
# driver-side combinations and emits one NxN table per mode.
# ---------------------------------------------------------------------------


# The four mode-matrices. Each is a (tx_side, rx_side) tuple labelled with
# the question it answers.
FULL_MATRIX_MODES = [
    ("kernel", "kernel",
     "Kernel-only (rig sanity / cross-chipset kernel interop)"),
    ("devourer", "kernel",
     "devourer TX → kernel RX (does devourer emit valid frames?)"),
    ("kernel", "devourer",
     "kernel TX → devourer RX (does devourer RX a known-good frame?)"),
    ("devourer", "devourer",
     "devourer ↔ devourer (end-to-end devourer)"),
]


def run_full_matrix(
    devourer_root: Path,
    duts: list[Dut],
    channel: int,
    duration: float,
    threshold: int,
    tmpdir: Path,
    kh: KernelHost,
    sniffer_iface: Optional[str] = None,
) -> dict[tuple[str, str, str, str], CellResult]:
    """Run every ordered (TX, RX) pair of distinct DUTs across all four
    driver-side combinations. Returns a dict keyed by
    (tx_side, rx_side, tx_vidpid, rx_vidpid)."""
    results: dict[tuple[str, str, str, str], CellResult] = {}
    pairs = [(tx, rx) for tx in duts for rx in duts if tx.sysfs_id != rx.sysfs_id]
    total = len(pairs) * len(FULL_MATRIX_MODES)
    idx = 0
    for tx_dut, rx_dut in pairs:
        for tx_side, rx_side, _label in FULL_MATRIX_MODES:
            idx += 1
            cell_id = (
                f"[{time.strftime('%H:%M:%S')}] [{idx}/{total}] "
                f"TX={tx_dut.chipset} ({tx_side}) → "
                f"RX={rx_dut.chipset} ({rx_side})"
            )
            print(cell_id + " ...", flush=True)
            try:
                r = run_cell(
                    devourer_root, tx_dut, rx_dut, tx_side, rx_side,
                    channel, duration, tmpdir, kh,
                    sniffer_iface=sniffer_iface,
                )
            except Exception as e:
                print(f"  ✗ cell crashed: {e}", flush=True)
                r = CellResult(hits=0, tx_attempts=0, tx_failures=0,
                               duration_s=0.0, notes=str(e))
            results[(tx_side, rx_side, tx_dut.vidpid, rx_dut.vidpid)] = r
            print(f"  → {r.fmt(threshold)}", flush=True)
            if r.notes and sniffer_iface:
                print(f"  ↪ {r.notes}", flush=True)
    return results


def emit_full_markdown(
    duts: list[Dut],
    channel: int,
    duration: float,
    threshold: int,
    kh: KernelHost,
    results: dict[tuple[str, str, str, str], CellResult],
) -> str:
    """Render four NxN tables, one per (tx_side, rx_side) mode. Diagonal is
    blanked (can't TX and RX with the same physical adapter)."""
    out = []
    out.append(f"# Full regression matrix — channel {channel}, "
               f"{time.strftime('%Y-%m-%d %H:%M:%S')}\n")
    out.append(f"- Kernel host: "
               f"{'VM ' + kh.vm_name + ' via ' + kh.ssh_target if kh.is_remote else 'local'}")
    out.append(f"- Cell duration: {duration:.0f}s   Pass threshold: ≥ {threshold} hits")
    out.append("- Adapters:")
    for d in duts:
        out.append(f"  - `{d.vidpid}` ({d.chipset})")
    out.append("")

    short = {d.vidpid: d.chipset.split(" ")[0] for d in duts}

    for tx_side, rx_side, label in FULL_MATRIX_MODES:
        out.append(f"## {label}\n")
        # Header
        header = "| TX \\ RX |" + "".join(
            f" {short[d.vidpid]} |" for d in duts
        )
        sep = "|---|" + "---|" * len(duts)
        out.append(header)
        out.append(sep)
        for tx_dut in duts:
            row_cells = []
            for rx_dut in duts:
                if tx_dut.sysfs_id == rx_dut.sysfs_id:
                    row_cells.append("—")
                    continue
                r = results.get((tx_side, rx_side, tx_dut.vidpid, rx_dut.vidpid))
                row_cells.append(r.fmt(threshold) if r else "?")
            out.append(f"| {short[tx_dut.vidpid]} | " + " | ".join(row_cells) + " |")
        out.append("")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Encoding matrix — one TX/RX pair, iterate (driver-mode × encoding flags).
# Surfaces chip-specific radiotap-encoding asymmetries that --full-matrix
# can't see because every cell there uses the default encoding (MCS 1, BCC,
# no STBC). Motivating example: RTL8821AU TXes LDPC fine but can't RX LDPC
# frames (the chip's decoder lacks the LDPC block), so an `LDPC` k/d cell
# to an 8821AU goes to 0 hits while `BCC` works.
# ---------------------------------------------------------------------------

ENCODING_COMBOS = [
    # HT (radiotap bit 19): MCS 1, 20 MHz.
    ("HT-BCC",        {"mcs": 1}),
    ("HT-LDPC",       {"mcs": 1, "ldpc": True}),
    ("HT-STBC=1",     {"mcs": 1, "stbc": 1}),
    ("HT-LDPC+STBC",  {"mcs": 1, "ldpc": True, "stbc": 1}),
    # VHT (radiotap bit 21): VHT MCS 0, NSS 1, 20 MHz. 8821AU's LDPC-RX-no
    # limitation (per Eachine Sphere Link reports) is on the VHT path —
    # HT-LDPC didn't surface it because the chip's HT and VHT decoders are
    # separate code paths in silicon.
    ("VHT-BCC",       {"vht": True, "vht_mcs": 0, "nss": 1}),
    ("VHT-LDPC",      {"vht": True, "vht_mcs": 0, "nss": 1, "ldpc": True}),
]


def run_encoding_matrix(
    devourer_root: Path,
    tx_dut: Dut,
    rx_dut: Dut,
    channel: int,
    duration: float,
    threshold: int,
    tmpdir: Path,
    kh: KernelHost,
    sniffer_iface: Optional[str] = None,
) -> dict[tuple[str, str, str], CellResult]:
    """For one ordered (TX, RX) pair, iterate every driver-mode × encoding
    combination. Returns dict keyed by (tx_side, rx_side, encoding_label)."""
    results: dict[tuple[str, str, str], CellResult] = {}
    total = len(FULL_MATRIX_MODES) * len(ENCODING_COMBOS)
    idx = 0
    for tx_side, rx_side, _label in FULL_MATRIX_MODES:
        for enc_label, enc in ENCODING_COMBOS:
            idx += 1
            cell_hdr = (
                f"[{time.strftime('%H:%M:%S')}] [{idx}/{total}] "
                f"TX={tx_dut.chipset} ({tx_side}) → "
                f"RX={rx_dut.chipset} ({rx_side})  enc=[{enc_label}]"
            )
            print(cell_hdr + " ...", flush=True)
            try:
                r = run_cell(
                    devourer_root, tx_dut, rx_dut, tx_side, rx_side,
                    channel, duration, tmpdir, kh, encoding=enc,
                    sniffer_iface=sniffer_iface,
                )
            except Exception as e:
                print(f"  ✗ cell crashed: {e}", flush=True)
                r = CellResult(hits=0, tx_attempts=0, tx_failures=0,
                               duration_s=0.0, notes=str(e))
            results[(tx_side, rx_side, enc_label)] = r
            print(f"  → {r.fmt(threshold)}", flush=True)
            if r.notes and sniffer_iface:
                print(f"  ↪ {r.notes}", flush=True)
    return results


def emit_encoding_markdown(
    tx_dut: Dut, rx_dut: Dut, channel: int, duration: float,
    threshold: int, kh: KernelHost,
    results: dict[tuple[str, str, str], CellResult],
) -> str:
    """Render one table: rows = driver mode (k/k, d/k, k/d, d/d),
    columns = encoding combo. A chip with an encoding-specific RX
    limitation will show a single column going to 0 in the k/d row."""
    out = []
    out.append(f"# Encoding matrix — channel {channel}, "
               f"{time.strftime('%Y-%m-%d %H:%M:%S')}\n")
    out.append(f"- TX adapter: `{tx_dut.vidpid}` ({tx_dut.chipset})")
    out.append(f"- RX adapter: `{rx_dut.vidpid}` ({rx_dut.chipset})")
    out.append(f"- Kernel host: "
               f"{'VM ' + kh.vm_name + ' via ' + kh.ssh_target if kh.is_remote else 'local'}")
    out.append(f"- Cell duration: {duration:.0f}s   "
               f"Pass threshold: ≥ {threshold} hits\n")
    header = "| Mode (TX/RX driver) |" + "".join(
        f" {lbl} |" for lbl, _ in ENCODING_COMBOS
    )
    sep = "|---|" + "---|" * len(ENCODING_COMBOS)
    out.append(header)
    out.append(sep)
    for tx_side, rx_side, _ in FULL_MATRIX_MODES:
        row = f"| {tx_side[0]}/{rx_side[0]} |"
        for enc_label, _ in ENCODING_COMBOS:
            r = results.get((tx_side, rx_side, enc_label))
            row += f" {r.fmt(threshold) if r else '?'} |"
        out.append(row)
    out.append("")
    return "\n".join(out)


def emit_markdown(
    tx_dut: Dut, rx_dut: Dut, channel: int, duration: float,
    threshold: int, kh: KernelHost,
    results: dict[tuple[str, str], CellResult],
) -> str:
    out = []
    out.append(f"## Regression matrix — channel {channel}, "
               f"{time.strftime('%Y-%m-%d %H:%M:%S')}\n")
    out.append(f"- TX adapter: `{tx_dut.vidpid}` ({tx_dut.chipset})")
    out.append(f"- RX adapter: `{rx_dut.vidpid}` ({rx_dut.chipset})")
    out.append(f"- Kernel host: "
               f"{'VM ' + kh.vm_name + ' via ' + kh.ssh_target if kh.is_remote else 'local'}")
    out.append(f"- Cell duration: {duration:.0f}s")
    out.append(f"- Pass threshold: ≥ {threshold} hits\n")
    out.append("|   | TX = devourer | TX = kernel |")
    out.append("|---|---|---|")
    for rx_side in ("devourer", "kernel"):
        cells = []
        for tx_side in ("devourer", "kernel"):
            r = results.get((tx_side, rx_side))
            cells.append(r.fmt(threshold) if r else "—")
        out.append(f"| RX = {rx_side} | {cells[0]} | {cells[1]} |")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# CLI.
# ---------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(
        description="Cross-driver regression matrix for devourer.",
    )
    ap.add_argument(
        "--devourer-root", type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repo root with build/WiFiDriverDemo + build/WiFiDriverTxDemo",
    )
    ap.add_argument(
        "--channel", type=int, default=36,
        help="Wi-Fi channel (default 36; pick a busy channel like 100 if your "
             "AP is on it — higher hit counts mean less variance)",
    )
    ap.add_argument(
        "--duration", type=float, default=15.0,
        help="seconds each cell injects/receives (default 15)",
    )
    ap.add_argument(
        "--pass-threshold", type=int, default=1,
        help="min hits for a cell to pass (default 1 — generous because air "
             "interference makes absolute counts unreliable)",
    )
    ap.add_argument(
        "--tx-pid",
        help="USB PID hex of TX adapter (default: first auto-detected DUT)",
    )
    ap.add_argument(
        "--rx-pid",
        help="USB PID hex of RX adapter (default: second auto-detected DUT)",
    )
    ap.add_argument(
        "--keep-logs", action="store_true",
        help="don't delete the per-cell log files after the run",
    )
    ap.add_argument(
        "--no-baseline-abort", action="store_true",
        help="run all 4 cells even if kernel-kernel baseline fails",
    )
    ap.add_argument(
        "--full-matrix", action="store_true",
        help="iterate every ordered (TX, RX) pair of plugged DUTs across "
             "all 4 driver-side combinations. Emits four NxN tables instead "
             "of one 4-cell table. Ignores --tx-pid / --rx-pid.",
    )
    ap.add_argument(
        "--encoding-matrix", action="store_true",
        help="for one ordered TX→RX pair (use --tx-pid / --rx-pid to select, "
             "first two auto-detected DUTs otherwise), iterate "
             "(driver-mode × radiotap encoding combo). Surfaces chip-specific "
             "asymmetries like the RTL8821AU LDPC-RX-no limitation.",
    )
    ap.add_argument(
        "--vm-name",
        default=os.environ.get("DEVOURER_VM_NAME", ""),
        help="libvirt domain to run kernel cells in (env: DEVOURER_VM_NAME). "
             "If unset, kernel cells run locally on the host.",
    )
    ap.add_argument(
        "--vm-ssh",
        default=os.environ.get("DEVOURER_VM_SSH", ""),
        help="ssh target (user@host) for the VM (env: DEVOURER_VM_SSH). "
             "Required if --vm-name is set.",
    )
    ap.add_argument(
        "--sniffer-iface",
        default=os.environ.get("DEVOURER_SNIFFER_IFACE", ""),
        help="host monitor-mode iface to attach as a 3rd-adapter sniffer "
             "(intended: AR9271). When set, each cell pcap-captures the air "
             "and reports decoded encoding distribution alongside the hit "
             "count — answers 'what encoding actually flew' for "
             "--encoding-matrix runs. Always host-local; never moved to the "
             "VM. Env: DEVOURER_SNIFFER_IFACE.",
    )
    args = ap.parse_args()

    if args.vm_name and not args.vm_ssh:
        sys.stderr.write("--vm-name requires --vm-ssh\n")
        sys.exit(2)
    if args.vm_ssh and not args.vm_name:
        sys.stderr.write("--vm-ssh requires --vm-name\n")
        sys.exit(2)

    kh = (KernelHost.via_ssh(args.vm_ssh, args.vm_name)
          if args.vm_name else KernelHost.local())

    preflight(args.devourer_root, kh)

    duts = discover_duts()
    if len(duts) < 2:
        sys.stderr.write(
            f"Need at least 2 supported DUTs plugged in on host, found "
            f"{len(duts)}:\n"
        )
        for d in duts:
            sys.stderr.write(f"  - {d.vidpid} ({d.chipset}) at {d.sysfs_id}\n")
        sys.stderr.write("Plug another compatible adapter or extend "
                         "SUPPORTED_DUTS table.\n")
        sys.exit(2)

    def pick(pid_arg, default_idx):
        if pid_arg is None:
            return duts[default_idx]
        for d in duts:
            if d.pid == pid_arg.lower().removeprefix("0x"):
                return d
        sys.stderr.write(f"No plugged DUT has PID {pid_arg}\n")
        sys.exit(2)

    if args.encoding_matrix:
        tx_dut = pick(args.tx_pid, 0)
        rx_dut = pick(args.rx_pid, 1)
        if tx_dut.sysfs_id == rx_dut.sysfs_id:
            sys.stderr.write("TX and RX must be different physical devices.\n")
            sys.exit(2)
        print(f"Encoding matrix mode:")
        print(f"  TX adapter: {tx_dut.vidpid} ({tx_dut.chipset}) at {tx_dut.sysfs_id}")
        print(f"  RX adapter: {rx_dut.vidpid} ({rx_dut.chipset}) at {rx_dut.sysfs_id}")
        print(f"Kernel host: "
              f"{'VM ' + kh.vm_name + ' (' + kh.ssh_target + ')' if kh.is_remote else 'local'}")
        n_cells = len(FULL_MATRIX_MODES) * len(ENCODING_COMBOS)
        print(f"Channel: {args.channel}   Duration/cell: {args.duration}s   "
              f"Pass threshold: ≥{args.pass_threshold} hits")
        print(f"Total cells: {n_cells} "
              f"({len(FULL_MATRIX_MODES)} driver modes × "
              f"{len(ENCODING_COMBOS)} encoding combos)\n")

        kh.release_all_known_duts([tx_dut, rx_dut])
        with tempfile.TemporaryDirectory(prefix="devourer-regress-") as td:
            tmpdir = Path(td)
            results = run_encoding_matrix(
                devourer_root=args.devourer_root,
                tx_dut=tx_dut, rx_dut=rx_dut,
                channel=args.channel, duration=args.duration,
                threshold=args.pass_threshold,
                tmpdir=tmpdir, kh=kh,
                sniffer_iface=args.sniffer_iface or None,
            )
            print()
            md = emit_encoding_markdown(
                tx_dut, rx_dut, args.channel, args.duration,
                args.pass_threshold, kh, results,
            )
            print(md, flush=True)
            sys.stdout.flush()
            if args.keep_logs:
                kept = Path(tempfile.gettempdir()) / "devourer-regress-last"
                if kept.is_symlink() or kept.exists():
                    kept.unlink()
                kept.symlink_to(tmpdir)
                print(f"(logs kept at {kept} — symlink, valid until next run)",
                      flush=True)
                sys.stdout.flush()
                os._exit(0)
        return

    if args.full_matrix:
        print(f"Full matrix mode over {len(duts)} adapters:")
        for d in duts:
            print(f"  - {d.vidpid} ({d.chipset}) at {d.sysfs_id}")
        print(f"Kernel host: "
              f"{'VM ' + kh.vm_name + ' (' + kh.ssh_target + ')' if kh.is_remote else 'local'}")
        n_pairs = len(duts) * (len(duts) - 1)
        n_cells = n_pairs * len(FULL_MATRIX_MODES)
        print(f"Channel: {args.channel}   Duration/cell: {args.duration}s   "
              f"Pass threshold: ≥{args.pass_threshold} hits")
        print(f"Total cells: {n_cells} "
              f"({n_pairs} ordered pairs × {len(FULL_MATRIX_MODES)} mode-combos)\n")

        kh.release_all_known_duts(duts)

        with tempfile.TemporaryDirectory(prefix="devourer-regress-") as td:
            tmpdir = Path(td)
            results = run_full_matrix(
                devourer_root=args.devourer_root,
                duts=duts,
                channel=args.channel, duration=args.duration,
                threshold=args.pass_threshold,
                tmpdir=tmpdir, kh=kh,
                sniffer_iface=args.sniffer_iface or None,
            )
            print()
            md = emit_full_markdown(
                duts, args.channel, args.duration,
                args.pass_threshold, kh, results,
            )
            print(md)
            if args.keep_logs:
                kept = Path(tempfile.gettempdir()) / "devourer-regress-last"
                if kept.is_symlink() or kept.exists():
                    kept.unlink()
                kept.symlink_to(tmpdir)
                print(f"(logs kept at {kept} — symlink, valid until next run)")
                os._exit(0)
        return

    tx_dut = pick(args.tx_pid, 0)
    rx_dut = pick(args.rx_pid, 1)
    if tx_dut.sysfs_id == rx_dut.sysfs_id:
        sys.stderr.write("TX and RX must be different physical devices.\n")
        sys.exit(2)

    print(f"TX: {tx_dut.vidpid} ({tx_dut.chipset}) at {tx_dut.sysfs_id}")
    print(f"RX: {rx_dut.vidpid} ({rx_dut.chipset}) at {rx_dut.sysfs_id}")
    print(f"Kernel host: "
          f"{'VM ' + kh.vm_name + ' (' + kh.ssh_target + ')' if kh.is_remote else 'local'}")
    print(f"Channel: {args.channel}   Duration/cell: {args.duration}s   "
          f"Pass threshold: ≥{args.pass_threshold} hits\n")

    # Clean baseline: pull any leftover DUTs back from the VM so we start
    # the matrix with both DUTs on the host.
    kh.release_all_known_duts([tx_dut, rx_dut])

    with tempfile.TemporaryDirectory(prefix="devourer-regress-") as td:
        tmpdir = Path(td)
        results = run_matrix(
            devourer_root=args.devourer_root,
            tx_dut=tx_dut, rx_dut=rx_dut,
            channel=args.channel, duration=args.duration,
            threshold=args.pass_threshold,
            tmpdir=tmpdir, kh=kh,
            abort_on_baseline_fail=not args.no_baseline_abort,
            sniffer_iface=args.sniffer_iface or None,
        )
        print()
        md = emit_markdown(
            tx_dut, rx_dut, args.channel, args.duration,
            args.pass_threshold, kh, results,
        )
        print(md)
        if args.keep_logs:
            kept = Path(tempfile.gettempdir()) / "devourer-regress-last"
            if kept.is_symlink() or kept.exists():
                kept.unlink()
            kept.symlink_to(tmpdir)
            print(f"(logs kept at {kept} — symlink, valid until next run)")
            os._exit(0)


if __name__ == "__main__":
    main()
