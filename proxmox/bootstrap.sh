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
#  bootstrap.sh — Install k3s and deploy the SDR stack on a fresh Ubuntu VM
#
#  Run once on the Proxmox VM after it first boots (cloud-init calls this
#  automatically if credentials are set; otherwise run manually).
#
#  Required environment variables:
#    SDR_DEVICE_IP   IP of the SoapySDR remote device (the machine with the PlutoSDR)
#    AMQP_PASSWORD   AMQP broker password
#    DB_PASSWORD     PostgreSQL password
#
#  Optional:
#    SDR_DEVICE_IP_1  Second SDR device IP (for multi-board setups)
#    K3S_VERSION      Pin a specific k3s version (default: latest stable)
#
#  Usage:
#    export SDR_DEVICE_IP=192.168.10.100
#    export AMQP_PASSWORD=my_amqp_pw
#    export DB_PASSWORD=my_db_pw
#    bash bootstrap.sh
# ══════════════════════════════════════════════════════════════════════════════
set -euo pipefail

GRN='\033[0;32m'; BLU='\033[0;34m'; YLW='\033[0;33m'; RED='\033[0;31m'; RST='\033[0m'
info() { echo -e "${BLU}[bootstrap]${RST} $*"; }
ok()   { echo -e "${GRN}[bootstrap]${RST} $*"; }
warn() { echo -e "${YLW}[bootstrap]${RST} $*"; }
die()  { echo -e "${RED}[bootstrap]${RST} $*" >&2; exit 1; }

SDR_DIR="/opt/sdr"
KUBECONFIG_PATH="/etc/rancher/k3s/k3s.yaml"
export KUBECONFIG="$KUBECONFIG_PATH"
KUBECTL="k3s kubectl"

# ── Gather required inputs ────────────────────────────────────────────────────
if [[ -z "${SDR_DEVICE_IP:-}" ]]; then
    read -r -p "SDR device IP (SoapySDR remote host, e.g. 192.168.10.100): " SDR_DEVICE_IP
fi
if [[ -z "${AMQP_PASSWORD:-}" ]]; then
    read -r -s -p "AMQP broker password: " AMQP_PASSWORD; echo
fi
if [[ -z "${DB_PASSWORD:-}" ]]; then
    read -r -s -p "PostgreSQL password: " DB_PASSWORD; echo
fi
[[ -n "$SDR_DEVICE_IP"  ]] || die "SDR_DEVICE_IP must not be empty"
[[ -n "$AMQP_PASSWORD"  ]] || die "AMQP_PASSWORD must not be empty"
[[ -n "$DB_PASSWORD"    ]] || die "DB_PASSWORD must not be empty"

SDR_DEVICE_IP_1="${SDR_DEVICE_IP_1:-}"   # optional second device

info "SDR device:    $SDR_DEVICE_IP${SDR_DEVICE_IP_1:+ + $SDR_DEVICE_IP_1}"
info "Repos:         $SDR_DIR"

# ── 1. Install k3s ────────────────────────────────────────────────────────────
info "1/6  Installing k3s…"
if command -v k3s &>/dev/null; then
    ok "k3s already installed ($(k3s --version | head -1))"
else
    curl -sfL https://get.k3s.io | \
        ${K3S_VERSION:+INSTALL_K3S_VERSION=$K3S_VERSION} \
        sh -s - \
        --write-kubeconfig-mode 644 \
        --disable traefik \
        --disable servicelb \
        --node-label sdr-usb=true
    # k3s systemd service is enabled and started automatically by the installer
fi

# ── 2. Wait for k3s to be ready ───────────────────────────────────────────────
info "2/6  Waiting for k3s node to be Ready…"
until $KUBECTL get nodes 2>/dev/null | grep -q Ready; do
    echo -n "."; sleep 3
done
echo ""
ok "k3s node is Ready"

# Give the local-path provisioner a moment to register
sleep 5

# ── 3. Patch SDR device IPs in the controller manifest ───────────────────────
info "3/6  Configuring SDR device IPs in controller manifest…"
CTRL_MANIFEST="$SDR_DIR/SdrResourceManager/k8s/deployment.yaml"

# Replace the first externalName placeholder with SDR_DEVICE_IP
sed -i "0,/externalName: 192\.168\.10\.100/{s/externalName: 192\.168\.10\.100/externalName: $SDR_DEVICE_IP/}" "$CTRL_MANIFEST"
info "     pluto-sdr-0 → $SDR_DEVICE_IP"

# Optionally set second device
if [[ -n "$SDR_DEVICE_IP_1" ]]; then
    sed -i "s/externalName: 192\.168\.10\.101/externalName: $SDR_DEVICE_IP_1/" "$CTRL_MANIFEST"
    info "     pluto-sdr-1 → $SDR_DEVICE_IP_1"
fi

# ── 4. Deploy the full stack ──────────────────────────────────────────────────
info "4/6  Deploying SDR stack…"
cd "$SDR_DIR/SdrResourceManager"

AMQP_PASSWORD="$AMQP_PASSWORD" \
DB_PASSWORD="$DB_PASSWORD" \
KUBECTL="k3s kubectl" \
    bash k8s/deploy.sh

# ── 5. Write kubeconfig for the sdr user ─────────────────────────────────────
info "5/6  Configuring kubectl for sdr user…"
USER_HOME=$(getent passwd sdr | cut -d: -f6)
mkdir -p "$USER_HOME/.kube"
cp "$KUBECONFIG_PATH" "$USER_HOME/.kube/config"
chown sdr:sdr "$USER_HOME/.kube/config"
chmod 600 "$USER_HOME/.kube/config"

# Make k3s kubectl available as just 'kubectl'
ln -sf /usr/local/bin/k3s /usr/local/bin/kubectl 2>/dev/null || true

# ── 6. Persist environment for future re-deploys ─────────────────────────────
info "6/6  Saving environment…"
cat > /etc/sdr-stack.env <<EOF
export SDR_DEVICE_IP=$SDR_DEVICE_IP
${SDR_DEVICE_IP_1:+export SDR_DEVICE_IP_1=$SDR_DEVICE_IP_1}
export AMQP_PASSWORD=$AMQP_PASSWORD
export DB_PASSWORD=$DB_PASSWORD
EOF
chmod 600 /etc/sdr-stack.env
chown root:root /etc/sdr-stack.env

# ── Done ──────────────────────────────────────────────────────────────────────
echo ""
ok "Bootstrap complete."
echo ""
echo "  Pod status:"
$KUBECTL get pods -n sdr-system 2>/dev/null || true
echo ""
echo "  Useful commands:"
echo "    kubectl get pods -n sdr-system            # all pods"
echo "    kubectl logs -n sdr-system -l app=sdr-acquisition -f"
echo "    kubectl logs -n sdr-system -l app=sdr-analysis -f"
echo "    kubectl logs -n sdr-system -l app=signal-logger -f"
echo ""
echo "  k3s and all pods start automatically on VM reboot."
echo ""
echo "  To redeploy after a config change:"
echo "    source /etc/sdr-stack.env"
echo "    cd /opt/sdr/SdrResourceManager"
echo "    bash k8s/deploy.sh"

# ========================================================================
# End of file — OpenRFStack
# Subject to Personal Use License
# https://github.com/OpenRFStack
# ========================================================================
