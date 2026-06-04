#!/usr/bin/env bash
# Provision a libvirt VM for kernel-side regression testing.
#
# Why a VM: aircrack-ng/rtl8812au (the out-of-tree driver with the best
# 8812/8814/8821 chipset coverage) lags newer kernels by 6-12 months. As of
# kernel 6.15+ the OOT driver hits API breakages (timer_*, cfg80211 callback
# signatures, etc.) that require ongoing patching. Putting the kernel-side
# tests in a pinned-kernel VM (Ubuntu 22.04 LTS = kernel 5.15) lets the host
# upgrade freely without breaking the test rig.
#
# Prerequisites on the host:
#   - libvirtd + virsh
#   - virt-install
#   - xorriso (for cloud-init seed ISO)
#   - jammy-base.qcow2 cloud image at /var/lib/libvirt/images/ (or set BASE)
#   - SSH key for the calling user (defaults to ~/.ssh/id_rsa.pub)
#
# Usage:
#   tests/setup_vm.sh           # full provision (creates VM + waits for SSH)
#   tests/setup_vm.sh --teardown  # destroy + undefine the VM
#
# After provisioning:
#   tests/setup_vm.sh --status   # show VM IP, ssh hint, USB passthrough state

set -euo pipefail

VM_NAME="${VM_NAME:-devourer-testrig}"
VM_RAM_MB="${VM_RAM_MB:-2048}"
VM_VCPUS="${VM_VCPUS:-2}"
VM_DISK_GB="${VM_DISK_GB:-20}"
BASE_IMAGE="${BASE_IMAGE:-/var/lib/libvirt/images/jammy-base.qcow2}"
LIBVIRT_IMAGES="${LIBVIRT_IMAGES:-/var/lib/libvirt/images}"
# Username to create inside the VM. Defaults to the invoking user
# (SUDO_USER when called via sudo, else USER). Override with VM_USER=foo.
VM_USER="${VM_USER:-${SUDO_USER:-$USER}}"
SSH_PUBKEY="${SSH_PUBKEY:-$(eval echo "~$VM_USER/.ssh/id_rsa.pub")}"
WORK_DIR="${WORK_DIR:-$(eval echo "~$VM_USER/devourer-testrig-setup")}"

cmd="${1:-provision}"

vm_ip() {
  sudo virsh domifaddr "$VM_NAME" 2>/dev/null \
    | awk '/ipv4/ {print $4}' | cut -d/ -f1 | head -1
}

case "$cmd" in
  --teardown|teardown)
    sudo virsh destroy "$VM_NAME" 2>/dev/null || true
    sudo virsh undefine "$VM_NAME" --remove-all-storage 2>/dev/null || true
    echo "destroyed VM $VM_NAME"
    exit 0
    ;;
  --status|status)
    state=$(sudo virsh domstate "$VM_NAME" 2>/dev/null || echo "missing")
    echo "VM:      $VM_NAME ($state)"
    ip=$(vm_ip)
    echo "IP:      ${ip:-(none — DHCP not assigned)}"
    if [ -n "${ip:-}" ]; then
      echo "SSH:     ssh $VM_USER@$ip"
    fi
    echo "USB passthrough (current):"
    sudo virsh dumpxml "$VM_NAME" 2>/dev/null \
      | grep -A2 '<hostdev mode="subsystem" type="usb"' \
      | grep -E 'vendor id|product id' \
      | sed 's/^/  /'
    exit 0
    ;;
esac

if [ ! -f "$SSH_PUBKEY" ]; then
  echo "SSH_PUBKEY not found at $SSH_PUBKEY" >&2
  exit 1
fi

if [ ! -f "$BASE_IMAGE" ]; then
  echo "BASE_IMAGE not found at $BASE_IMAGE" >&2
  echo "Download an Ubuntu 22.04 cloud image:" >&2
  echo "  wget https://cloud-images.ubuntu.com/jammy/current/jammy-server-cloudimg-amd64.img" >&2
  echo "  sudo mv jammy-server-cloudimg-amd64.img $BASE_IMAGE" >&2
  exit 1
fi

if sudo virsh dominfo "$VM_NAME" >/dev/null 2>&1; then
  echo "VM $VM_NAME already exists. Use --teardown first, or set VM_NAME=other-name." >&2
  exit 1
fi

mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

cat > user-data <<EOF
#cloud-config
hostname: $VM_NAME
manage_etc_hosts: true

users:
  - name: $VM_USER
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys:
      - $(cat "$SSH_PUBKEY")

package_update: true
package_upgrade: false

packages:
  - dkms
  - build-essential
  - linux-headers-generic
  - linux-modules-extra-generic
  - git
  - iw
  - tcpdump
  - aircrack-ng
  - python3-scapy
  - openssh-server

runcmd:
  - systemctl enable --now ssh
  - git clone --depth 1 https://github.com/aircrack-ng/rtl8812au.git /opt/rtl8812au
  - cd /opt/rtl8812au && make dkms_install
  - modprobe cfg80211 && modprobe 88XXau
  - echo "devourer-testrig provisioned $(date -u +%Y-%m-%dT%H:%M:%SZ)" > /etc/motd
EOF

cat > meta-data <<EOF
instance-id: ${VM_NAME}-001
local-hostname: $VM_NAME
EOF

xorriso -as mkisofs -output seed.iso -volid cidata -joliet -rock user-data meta-data 2>/dev/null
sudo cp seed.iso "$LIBVIRT_IMAGES/${VM_NAME}-seed.iso"
sudo chown libvirt-qemu:libvirt-qemu "$LIBVIRT_IMAGES/${VM_NAME}-seed.iso"

sudo qemu-img create -f qcow2 -F qcow2 \
  -b "$BASE_IMAGE" \
  "$LIBVIRT_IMAGES/${VM_NAME}.qcow2" "${VM_DISK_GB}G" >/dev/null
sudo chown libvirt-qemu:libvirt-qemu "$LIBVIRT_IMAGES/${VM_NAME}.qcow2"

sudo virt-install \
  --name "$VM_NAME" \
  --memory "$VM_RAM_MB" \
  --vcpus "$VM_VCPUS" \
  --disk "path=$LIBVIRT_IMAGES/${VM_NAME}.qcow2,format=qcow2,bus=virtio" \
  --disk "path=$LIBVIRT_IMAGES/${VM_NAME}-seed.iso,device=cdrom" \
  --os-variant ubuntu22.04 \
  --network network=default,model=virtio \
  --controller usb,model=qemu-xhci,index=0 \
  --graphics none \
  --noautoconsole \
  --import >/dev/null

echo "VM created: $VM_NAME"
echo "waiting for DHCP lease..."
for i in $(seq 1 30); do
  ip=$(vm_ip)
  if [ -n "$ip" ]; then
    echo "got IP: $ip"
    break
  fi
  sleep 3
done

if [ -z "${ip:-}" ]; then
  echo "no IP after 90s; check 'virsh console $VM_NAME'" >&2
  exit 1
fi

echo "waiting for cloud-init to finish (installs aircrack-ng driver, ~5-10 min)..."
ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 \
    -o UserKnownHostsFile=/dev/null \
    $VM_USER@"$ip" "cloud-init status --wait" 2>&1 | tail -3

echo
echo "=== VM ready ==="
echo "ssh $VM_USER@$ip"
echo
echo "Verify aircrack-ng driver:"
echo "  ssh $VM_USER@$ip 'sudo modprobe 88XXau && lsmod | grep 88XXau'"
echo
echo "Hot-plug a DUT into the VM (example for 8814AU):"
echo "  cat > /tmp/usb-8814.xml << 'XML'"
echo "  <hostdev mode='subsystem' type='usb' managed='yes'>"
echo "    <source>"
echo "      <vendor id='0x0bda'/>"
echo "      <product id='0x8813'/>"
echo "    </source>"
echo "  </hostdev>"
echo "  XML"
echo "  sudo virsh attach-device $VM_NAME /tmp/usb-8814.xml --live"
echo
echo "Teardown:    $0 --teardown"
