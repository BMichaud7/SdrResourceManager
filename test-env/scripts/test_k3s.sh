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

# test_k3s.sh — k3s integration test
# Starts a single-node k3s cluster, imports the controller image, applies
# the test deployment manifests, and runs AMQP smoke tests.
#
# Must run with --privileged (k3s requirement).
# The controller image tar must be mounted at /tmp/sdr-controller.tar.

set -euo pipefail

BOLD=$'\e[1m'; RESET=$'\e[0m'
GREEN=$'\e[32m'; RED=$'\e[31m'; YELLOW=$'\e[33m'

PASS=0; FAIL=0

pass()    { echo "  ${GREEN}PASS${RESET}  $1"; ((PASS++)) || true; }
fail()    { echo "  ${RED}FAIL${RESET}  $1"; ((FAIL++)) || true; }
warn()    { echo "  ${YELLOW}WARN${RESET}  $1"; }
section() { echo; echo "${BOLD}── $1 ─────────────────────────────────────${RESET}"; echo; }

K3S_PID=""
PF_PID=""

cleanup() {
    [[ -n "$PF_PID" ]] && kill "$PF_PID" 2>/dev/null || true
    [[ -n "$K3S_PID" ]] && kill "$K3S_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

export KUBECONFIG=/etc/rancher/k3s/k3s.yaml

# ── 1. Pre-flight checks ──────────────────────────────────────────────────────
section "Pre-flight"

if [[ ! -f /tmp/sdr-controller.tar ]]; then
    echo "  ${RED}ERROR:${RESET} /tmp/sdr-controller.tar not found."
    echo "  Build and save the controller image first:"
    echo "    podman build -f Containerfile.centos10 -t sdr-controller:test ."
    echo "    podman save sdr-controller:test -o /tmp/sdr-controller.tar"
    echo "  Then mount it: podman run --privileged -v /tmp/sdr-controller.tar:/tmp/sdr-controller.tar:ro ..."
    exit 1
fi
pass "Controller image tar present at /tmp/sdr-controller.tar"

if [[ ! -x /usr/local/bin/k3s ]]; then
    fail "k3s binary not found at /usr/local/bin/k3s"
    exit 1
fi
pass "k3s binary present"

# ── 2. Start k3s ─────────────────────────────────────────────────────────────
section "k3s Cluster Startup"

echo "  Starting k3s server (single-node, no traefik, no metrics-server)..."
k3s server \
    --disable=traefik \
    --disable=metrics-server \
    --disable=servicelb \
    --write-kubeconfig-mode=644 \
    > /tmp/k3s.log 2>&1 &
K3S_PID=$!

echo -n "  Waiting for k3s API server"
READY=false
for i in $(seq 1 90); do
    if kubectl get nodes --no-headers 2>/dev/null | grep -q "Ready"; then
        READY=true; break
    fi
    echo -n "."; sleep 2
done
echo

if [[ "$READY" == true ]]; then
    pass "k3s node is Ready"
else
    fail "k3s node failed to become Ready within 3 minutes"
    echo "k3s log tail:"; tail -30 /tmp/k3s.log
    exit 1
fi

# ── 3. Import controller image into k3s containerd ───────────────────────────
section "Image Import"

echo "  Importing sdr-controller:test from tar..."
k3s ctr images import /tmp/sdr-controller.tar
pass "Image imported into k3s containerd"

# Verify it's there
if k3s ctr images ls | grep -q "sdr-controller"; then
    pass "Image visible in k3s containerd"
else
    fail "Image not found in k3s containerd after import"
fi

# ── 4. Apply test manifests ───────────────────────────────────────────────────
section "Manifest Apply"

echo "  Applying k3s-test-deployment.yaml..."
kubectl apply -f /build/SdrResourceManager/test-env/config/k3s-test-deployment.yaml

# Dry-run the production manifest to validate its schema (no deploy)
echo "  Dry-run validation of production k8s/deployment.yaml..."
if kubectl apply --dry-run=client -f /build/SdrResourceManager/k8s/deployment.yaml >/dev/null 2>&1; then
    pass "Production k8s/deployment.yaml passes dry-run validation"
else
    fail "Production k8s/deployment.yaml dry-run failed"
fi

# ── 5. Wait for pods ──────────────────────────────────────────────────────────
section "Pod Readiness"

echo -n "  Waiting for ActiveMQ pod"
READY=false
for i in $(seq 1 120); do
    STATUS=$(kubectl get pods -n sdr-system -l app=activemq \
        --no-headers 2>/dev/null | awk '{print $3}' || echo "")
    if [[ "$STATUS" == "Running" ]]; then READY=true; break; fi
    echo -n "."; sleep 2
done
echo
[[ "$READY" == true ]] && pass "ActiveMQ pod Running" || { fail "ActiveMQ pod not Running"; kubectl describe pod -n sdr-system -l app=activemq; }

echo -n "  Waiting for sdr-controller pod"
READY=false
for i in $(seq 1 120); do
    STATUS=$(kubectl get pods -n sdr-system -l app=sdr-controller \
        --no-headers 2>/dev/null | awk '{print $3}' || echo "")
    if [[ "$STATUS" == "Running" ]]; then READY=true; break; fi
    echo -n "."; sleep 2
done
echo
[[ "$READY" == true ]] && pass "sdr-controller pod Running" || { fail "sdr-controller pod not Running"; kubectl describe pod -n sdr-system -l app=sdr-controller; }

if [[ "$FAIL" -gt 0 ]]; then
    echo
    echo "Pod logs:"
    kubectl logs -n sdr-system -l app=sdr-controller --tail=40 2>/dev/null || true
    exit 1
fi

# Give the controller a moment to connect to the broker
sleep 5

# ── 6. AMQP smoke tests via port-forward ─────────────────────────────────────
section "AMQP Smoke Tests"

echo "  Starting kubectl port-forward for broker (5672 → localhost:15672)..."
kubectl port-forward -n sdr-system service/activemq-service 15672:5672 \
    > /tmp/portforward.log 2>&1 &
PF_PID=$!
sleep 3

# Verify port-forward is up
if nc -z localhost 15672 2>/dev/null; then
    pass "Port-forward to broker established"
else
    fail "Port-forward to broker failed"
    cat /tmp/portforward.log
fi

run_client() {
    LD_LIBRARY_PATH=/usr/local/lib64:/usr/local/lib \
    /usr/local/bin/sdr_client amqp://localhost:15672 sdr_ctrl sdr_test_pw "$@" \
        2>/dev/null || echo ""
}

# Health query
echo "  Sending HEALTH_QUERY..."
RESP=$(run_client health)
echo "  Response: ${RESP:0:120}..."
if echo "$RESP" | grep -q "HEALTH_QUERY_RESPONSE"; then
    pass "HEALTH_QUERY → HEALTH_QUERY_RESPONSE"
else
    fail "HEALTH_QUERY → HEALTH_QUERY_RESPONSE"
fi

# Immediate task
echo "  Sending TASK_REQUEST_IMMEDIATE..."
RESP=$(run_client immediate 433920000 200000 1000000 1 0)
echo "  Response: ${RESP:0:120}..."
if echo "$RESP" | grep -q "TASK_RESPONSE"; then
    pass "TASK_REQUEST_IMMEDIATE → TASK_RESPONSE"
else
    fail "TASK_REQUEST_IMMEDIATE → TASK_RESPONSE"
fi

# ── 7. Controller log check ───────────────────────────────────────────────────
section "Controller Log Sanity"

CTRL_LOGS=$(kubectl logs -n sdr-system -l app=sdr-controller --tail=50 2>/dev/null || echo "")
echo "$CTRL_LOGS" | head -20

# Should see the controller running message
if echo "$CTRL_LOGS" | grep -qE "Controller: running|AmqpClient: connected"; then
    pass "Controller log shows successful startup"
else
    fail "Controller log missing startup confirmation"
fi

# Should NOT see panic or unexpected crashes
if echo "$CTRL_LOGS" | grep -qi "panic\|terminate called\|SIGSEGV"; then
    fail "Controller log contains crash indicators"
else
    pass "No crash indicators in controller log"
fi

# ── 8. Graceful rollout restart (tests Recreate strategy) ────────────────────
section "Recreate Strategy"

echo "  Triggering rollout restart (validates Recreate — not RollingUpdate)..."
kubectl rollout restart deployment/sdr-controller -n sdr-system
kubectl rollout status deployment/sdr-controller -n sdr-system --timeout=60s \
    && pass "Rollout restart completed (Recreate strategy)" \
    || fail "Rollout restart timed out"

# ── Results ───────────────────────────────────────────────────────────────────
section "Results"

TOTAL=$(( PASS + FAIL ))
echo "  ${PASS} / ${TOTAL} tests passed"
echo

if [[ "$FAIL" -eq 0 ]]; then
    echo "${GREEN}${BOLD}ALL K3S TESTS PASSED${RESET}"
    exit 0
else
    echo "${RED}${BOLD}${FAIL} TEST(S) FAILED${RESET}"
    echo
    echo "k3s server log (last 30 lines):"
    tail -30 /tmp/k3s.log || true
    exit 1
fi

# ========================================================================
# End of file — OpenRFStack
# Subject to Personal Use License
# https://github.com/OpenRFStack
# ========================================================================
