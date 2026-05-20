#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════════════════════
#  deploy.sh — Deploy the full SDR stack to Kubernetes / k3s
#
#  Assumes all SDR repos are cloned as siblings of SdrResourceManager:
#    /some/path/SdrResourceManager/   ← this repo
#    /some/path/AcquisitionApp/
#    /some/path/AnalysisApp/
#    /some/path/SdrScripts/
#
#  Usage:
#    ./k8s/deploy.sh                         # deploy everything
#    ./k8s/deploy.sh --dry-run               # print what would be applied
#    AMQP_PASSWORD=s3cr3t DB_PASSWORD=s3cr3t \
#      ./k8s/deploy.sh                       # non-interactive with env vars
#
#  Teardown:
#    kubectl delete namespace sdr-system
# ══════════════════════════════════════════════════════════════════════════════
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(dirname "$SCRIPT_DIR")"          # SdrResourceManager/
ROOT_DIR="$(dirname "$PARENT_DIR")"            # sibling of all repos

ACQ_K8S="$ROOT_DIR/AcquisitionApp/deploy/k8s/deployment.yaml"
ANA_K8S="$ROOT_DIR/AnalysisApp/deploy/k8s/deployment.yaml"
LOG_K8S="$ROOT_DIR/SdrScripts/deploy/k8s/deployment.yaml"
RMG_K8S="$SCRIPT_DIR/deployment.yaml"
PG_K8S="$SCRIPT_DIR/postgres.yaml"
SEC_K8S="$SCRIPT_DIR/secrets.yaml"

DRY_RUN=false
for arg in "$@"; do
    [[ "$arg" == "--dry-run" ]] && DRY_RUN=true
done

KUBECTL="${KUBECTL:-kubectl}"
APPLY="$KUBECTL apply"
$DRY_RUN && APPLY="$KUBECTL apply --dry-run=client"

# ── Colour helpers ─────────────────────────────────────────────────────────────
GRN='\033[0;32m'; BLU='\033[0;34m'; YLW='\033[0;33m'; RED='\033[0;31m'; RST='\033[0m'
info()  { echo -e "${BLU}[deploy]${RST} $*"; }
ok()    { echo -e "${GRN}[deploy]${RST} $*"; }
warn()  { echo -e "${YLW}[deploy]${RST} $*"; }
die()   { echo -e "${RED}[deploy]${RST} $*" >&2; exit 1; }

# ── Preflight ──────────────────────────────────────────────────────────────────
command -v "$KUBECTL" >/dev/null || die "kubectl not found (set KUBECTL= if using k3s)"
info "Using: $($KUBECTL version --client --short 2>/dev/null || echo kubectl)"

for f in "$ACQ_K8S" "$ANA_K8S" "$LOG_K8S" "$RMG_K8S" "$PG_K8S"; do
    [[ -f "$f" ]] || die "Missing manifest: $f\n  (ensure all SDR repos are siblings of SdrResourceManager)"
done

# ── Credentials ────────────────────────────────────────────────────────────────
if [[ -z "${AMQP_PASSWORD:-}" ]]; then
    read -r -s -p "AMQP password (sdr-credentials): " AMQP_PASSWORD; echo
fi
if [[ -z "${DB_PASSWORD:-}" ]]; then
    read -r -s -p "DB password   (sdr-credentials): " DB_PASSWORD; echo
fi
[[ -n "$AMQP_PASSWORD" ]] || die "AMQP_PASSWORD must not be empty"
[[ -n "$DB_PASSWORD"   ]] || die "DB_PASSWORD must not be empty"

# ── Step 1: Namespace + Secret ─────────────────────────────────────────────────
info "1/6  Namespace …"
$APPLY -f - <<EOF
apiVersion: v1
kind: Namespace
metadata:
  name: sdr-system
  labels:
    app.kubernetes.io/name: sdr-system
EOF

info "1/6  sdr-credentials Secret …"
if $DRY_RUN; then
    echo "[dry-run] would create/update secret sdr-credentials"
else
    $KUBECTL create secret generic sdr-credentials \
        -n sdr-system \
        --from-literal=amqp-username=sdr_ctrl \
        --from-literal=amqp-password="$AMQP_PASSWORD" \
        --from-literal=db-username=sdr \
        --from-literal=db-password="$DB_PASSWORD" \
        --dry-run=client -o yaml | $KUBECTL apply -f -
fi

# ── Step 2: PostgreSQL ─────────────────────────────────────────────────────────
info "2/6  PostgreSQL StatefulSet …"
$APPLY -f "$PG_K8S"

if ! $DRY_RUN; then
    info "      Waiting for postgres to be ready (up to 90 s) …"
    $KUBECTL rollout status statefulset/postgres -n sdr-system --timeout=90s \
        || warn "postgres not ready yet — continuing (check: kubectl get pods -n sdr-system)"
fi

# ── Step 3: Artemis broker + controller ────────────────────────────────────────
info "3/6  Artemis + sdr-controller …"
$APPLY -f "$RMG_K8S"

# ── Step 4: AcquisitionApp ─────────────────────────────────────────────────────
info "4/6  AcquisitionApp DaemonSet …"
$APPLY -f "$ACQ_K8S"

# ── Step 5: AnalysisApp ────────────────────────────────────────────────────────
info "5/6  AnalysisApp …"
$APPLY -f "$ANA_K8S"

# ── Step 6: signal-logger ──────────────────────────────────────────────────────
info "6/6  signal-logger …"
$APPLY -f "$LOG_K8S"

# ── Status ─────────────────────────────────────────────────────────────────────
if ! $DRY_RUN; then
    echo ""
    ok "Deploy complete.  Pod status:"
    $KUBECTL get pods -n sdr-system 2>/dev/null || true
    echo ""
    ok "Node labels needed for AcquisitionApp DaemonSet:"
    echo "  kubectl label node <node-name> sdr-usb=true"
    echo ""
    ok "To watch logs:"
    echo "  kubectl logs -n sdr-system -l app=sdr-acquisition -f"
    echo "  kubectl logs -n sdr-system -l app=sdr-analysis -f"
    echo "  kubectl logs -n sdr-system -l app=signal-logger -f"
fi
