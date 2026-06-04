#!/usr/bin/env bash
# ========================================================================
# Project: OpenRFStack
# Author:  Brendan Michaud
# Year:    2026
# Part of OpenRFStack (https://github.com/OpenRFStack)
#
# Licensed under the Personal Use License.
# Do not use for commercial, organizational, or military purposes.
# ========================================================================

# ══════════════════════════════════════════════════════════════════════════════
#  create-vm.sh — Create a Proxmox KVM VM for the SDR stack
#
#  Run this ON THE PROXMOX HOST (not inside a VM).
#
#  What it does:
#   1. Downloads Ubuntu 24.04 cloud image (if not cached)
#   2. Creates a KVM VM with the cloud image as its disk
#   3. Attaches a cloud-init drive with user-data from cloud-init.yaml
#   4. Sets onboot=1 so the VM starts automatically with Proxmox
#   5. Starts the VM — cloud-init runs bootstrap.sh on first boot
#
#  The bootstrap.sh inside the VM installs k3s and deploys the full SDR stack.
#
#  Usage:
#    # Copy this whole proxmox/ directory to your Proxmox host, then:
#    chmod +x create-vm.sh
#    ./create-vm.sh
#
#  Environment overrides:
#    VM_ID=200          VM ID in Proxmox (default: 200)
#    VM_NAME=sdr-node   VM hostname
#    VM_CORES=4         vCPU count
#    VM_RAM=4096        RAM in MiB
#    VM_DISK=40G        Root disk size
#    STORAGE=local-lvm  Proxmox storage pool
#    BRIDGE=vmbr0       Network bridge
#    SSH_KEY_FILE=~/.ssh/id_rsa.pub   SSH public key injected into the VM
# ══════════════════════════════════════════════════════════════════════════════
set -euo pipefail

VM_ID="${VM_ID:-200}"
VM_NAME="${VM_NAME:-sdr-node}"
VM_CORES="${VM_CORES:-4}"
VM_RAM="${VM_RAM:-4096}"
VM_DISK="${VM_DISK:-40G}"
STORAGE="${STORAGE:-local-lvm}"
BRIDGE="${BRIDGE:-vmbr0}"
SSH_KEY_FILE="${SSH_KEY_FILE:-$HOME/.ssh/id_rsa.pub}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_URL="https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img"
IMAGE_FILE="/var/lib/vz/images/noble-server-cloudimg-amd64.img"
CLOUDINIT_YAML="$SCRIPT_DIR/cloud-init.yaml"

GRN='\033[0;32m'; BLU='\033[0;34m'; YLW='\033[0;33m'; RED='\033[0;31m'; RST='\033[0m'
info() { echo -e "${BLU}[proxmox]${RST} $*"; }
ok()   { echo -e "${GRN}[proxmox]${RST} $*"; }
die()  { echo -e "${RED}[proxmox]${RST} $*" >&2; exit 1; }

# ── Preflight ─────────────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]] || die "Must run as root on the Proxmox host"
command -v qm      >/dev/null || die "qm not found — run this on the Proxmox host"
command -v pvesm   >/dev/null || die "pvesm not found — run this on the Proxmox host"
[[ -f "$CLOUDINIT_YAML" ]] || die "cloud-init.yaml not found at $CLOUDINIT_YAML"

# Abort if VM already exists
qm status "$VM_ID" &>/dev/null && die "VM $VM_ID already exists. Use a different VM_ID or: qm destroy $VM_ID"

# ── Download cloud image ──────────────────────────────────────────────────────
if [[ ! -f "$IMAGE_FILE" ]]; then
    info "Downloading Ubuntu 24.04 cloud image…"
    wget -q --show-progress -O "$IMAGE_FILE" "$IMAGE_URL"
    ok "Image downloaded to $IMAGE_FILE"
else
    info "Using cached image: $IMAGE_FILE"
fi

# ── SSH key ───────────────────────────────────────────────────────────────────
SSH_KEY=""
if [[ -f "$SSH_KEY_FILE" ]]; then
    SSH_KEY="$(cat "$SSH_KEY_FILE")"
    info "SSH key: $SSH_KEY_FILE"
else
    info "WARNING: SSH key not found at $SSH_KEY_FILE — you will need the console password to log in"
fi

# ── Create VM ─────────────────────────────────────────────────────────────────
info "Creating VM $VM_ID ($VM_NAME)…"
qm create "$VM_ID" \
    --name     "$VM_NAME" \
    --memory   "$VM_RAM" \
    --cores    "$VM_CORES" \
    --net0     "virtio,bridge=$BRIDGE" \
    --onboot   1 \
    --agent    1 \
    --ostype   l26 \
    --serial0  socket \
    --vga      serial0

# ── Import disk ───────────────────────────────────────────────────────────────
info "Importing cloud image as VM disk…"
qm importdisk "$VM_ID" "$IMAGE_FILE" "$STORAGE" --format qcow2

# Attach and resize disk
qm set "$VM_ID" \
    --scsihw virtio-scsi-pci \
    --scsi0  "${STORAGE}:vm-${VM_ID}-disk-0,cache=writeback,discard=on"
qm disk resize "$VM_ID" scsi0 "$VM_DISK"

# ── Cloud-init drive ──────────────────────────────────────────────────────────
info "Configuring cloud-init…"
qm set "$VM_ID" \
    --ide2    "${STORAGE}:cloudinit" \
    --boot    "order=scsi0" \
    --ciuser  sdr

# Set SSH key if available
[[ -n "$SSH_KEY" ]] && qm set "$VM_ID" --sshkeys <(echo "$SSH_KEY")

# Copy our cloud-init.yaml as the VM's user-data
# Proxmox stores user-data snippets in /var/lib/vz/snippets/
SNIPPET_DIR="/var/lib/vz/snippets"
mkdir -p "$SNIPPET_DIR"
cp "$CLOUDINIT_YAML" "$SNIPPET_DIR/sdr-node-user-data.yaml"
qm set "$VM_ID" --cicustom "user=local:snippets/sdr-node-user-data.yaml"

# ── Start VM ──────────────────────────────────────────────────────────────────
info "Starting VM $VM_ID…"
qm start "$VM_ID"

echo ""
ok "VM $VM_ID ($VM_NAME) created and started."
echo ""
echo "  Cloud-init is running bootstrap.sh inside the VM."
echo "  Monitor progress:"
echo "    qm terminal $VM_ID         (console access)"
echo "    tail -f /var/log/syslog    (inside the VM)"
echo ""
echo "  Once bootstrap completes (~5 min), check the SDR stack:"
echo "    ssh sdr@<vm-ip>"
echo "    kubectl get pods -n sdr-system"
echo ""
echo "  The VM auto-starts with Proxmox (onboot=1)."
echo "  k3s auto-starts via systemd — pods restart automatically on crash."

# ========================================================================
# End of file — OpenRFStack
# Subject to Personal Use License
# https://github.com/OpenRFStack
# ========================================================================
