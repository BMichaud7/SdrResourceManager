#!/usr/bin/env bash
# test_systemd.sh — Systemd integration test
# Validates service files, then runs the broker + controller under the same
# conditions systemd would (correct user, env, lifecycle), and sends AMQP
# smoke tests to verify the full stack works.

set -euo pipefail

BOLD=$'\e[1m'; RESET=$'\e[0m'
GREEN=$'\e[32m'; RED=$'\e[31m'; YELLOW=$'\e[33m'

PASS=0; FAIL=0

pass() { echo "  ${GREEN}PASS${RESET}  $1"; ((PASS++)) || true; }
fail() { echo "  ${RED}FAIL${RESET}  $1"; ((FAIL++)) || true; }
warn() { echo "  ${YELLOW}WARN${RESET}  $1"; }
section() { echo; echo "${BOLD}── $1 ─────────────────────────────────────${RESET}"; echo; }

BROKER_PID=""
CTRL_PID=""

cleanup() {
    [[ -n "$CTRL_PID" ]]   && kill "$CTRL_PID"   2>/dev/null || true
    [[ -n "$BROKER_PID" ]] && kill "$BROKER_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

# ── 1. Service file validation ────────────────────────────────────────────────
section "Service File Validation"

# systemd-analyze verify checks unit files for structural errors.
# It needs systemd libraries but not a running systemd instance.
for svc in sdr-controller activemq-artemis; do
    UNIT="/etc/systemd/system/${svc}.service"
    if [[ ! -f "$UNIT" ]]; then
        fail "Service file missing: $UNIT"
        continue
    fi

    # Check required sections exist
    if grep -q "^\[Unit\]" "$UNIT" && \
       grep -q "^\[Service\]" "$UNIT" && \
       grep -q "^\[Install\]" "$UNIT"; then
        pass "Service file structure: ${svc}.service"
    else
        fail "Service file structure: ${svc}.service"
    fi

    # Check ExecStart is present
    if grep -q "^ExecStart=" "$UNIT"; then
        pass "ExecStart present: ${svc}.service"
    else
        fail "ExecStart present: ${svc}.service"
    fi
done

# Check the sdr user was created (as setup.sh would have done)
if id sdr &>/dev/null; then
    pass "System user 'sdr' exists"
else
    fail "System user 'sdr' exists"
fi

# Check config file is owned by root:sdr with mode 640
STAT_MODE=$(stat -c "%a %U %G" /etc/sdr-controller/devices.xml 2>/dev/null || echo "missing")
if [[ "$STAT_MODE" == "640 root sdr" ]]; then
    pass "Config file permissions: 640 root:sdr"
else
    warn "Config file permissions: got '$STAT_MODE' (expected '640 root sdr')"
fi

# Check broker.env permissions
STAT_ENV=$(stat -c "%a %U %G" /etc/sdr-controller/broker.env 2>/dev/null || echo "missing")
if [[ "$STAT_ENV" == "640 root sdr" ]]; then
    pass "broker.env permissions: 640 root:sdr"
else
    warn "broker.env permissions: got '$STAT_ENV'"
fi

# ── 2. Broker startup ─────────────────────────────────────────────────────────
section "Broker Startup"

echo "  Starting ActiveMQ Artemis..."
/opt/artemis-broker/bin/artemis run > /tmp/artemis.log 2>&1 &
BROKER_PID=$!

echo -n "  Waiting for AMQP port 5672"
READY=false
for i in $(seq 1 60); do
    if nc -z localhost 5672 2>/dev/null; then READY=true; break; fi
    echo -n "."; sleep 1
done
echo

if [[ "$READY" == true ]]; then
    pass "Broker listening on :5672"
else
    fail "Broker listening on :5672 (timed out after 60s)"
    echo "  Broker log tail:"
    tail -20 /tmp/artemis.log || true
    exit 1
fi

# ── 3. Controller startup as 'sdr' user ───────────────────────────────────────
section "Controller Startup (as 'sdr' user)"

echo "  Starting sdr_controller under 'sdr' user (matching systemd User=sdr)..."
runuser -u sdr -- \
    env LD_LIBRARY_PATH=/usr/local/lib64:/usr/local/lib \
        SDR_LOG_LEVEL=info \
    /usr/local/bin/sdr_controller /etc/sdr-controller/devices.xml \
    > /tmp/sdr_controller.log 2>&1 &
CTRL_PID=$!

echo -n "  Waiting for controller to connect to broker"
CONNECTED=false
for i in $(seq 1 20); do
    if grep -qE "Controller: running|AmqpClient: connected" /tmp/sdr_controller.log 2>/dev/null; then
        CONNECTED=true; break
    fi
    echo -n "."; sleep 1
done
echo

if [[ "$CONNECTED" == true ]]; then
    pass "Controller connected to broker"
else
    fail "Controller connected to broker (timed out)"
    echo "  Controller log:"
    cat /tmp/sdr_controller.log
    exit 1
fi

# ── 4. AMQP smoke tests ───────────────────────────────────────────────────────
section "AMQP Smoke Tests"

run_client() {
    /usr/local/bin/sdr_client amqp://localhost:5672 sdr_ctrl sdr_test_pw "$@" 2>/dev/null || echo ""
}

# Health query
echo "  Sending HEALTH_QUERY..."
RESP=$(run_client health)
echo "  Response: ${RESP:0:100}..."
if echo "$RESP" | grep -q "HEALTH_QUERY_RESPONSE"; then
    pass "HEALTH_QUERY → HEALTH_QUERY_RESPONSE"
else
    fail "HEALTH_QUERY → HEALTH_QUERY_RESPONSE"
fi

# Immediate task (no hardware → expect TASK_RESPONSE with accept:false)
echo "  Sending TASK_REQUEST_IMMEDIATE..."
RESP=$(run_client immediate 433920000 200000 1000000 1 0)
echo "  Response: ${RESP:0:100}..."
if echo "$RESP" | grep -q "TASK_RESPONSE"; then
    pass "TASK_REQUEST_IMMEDIATE → TASK_RESPONSE"
else
    fail "TASK_REQUEST_IMMEDIATE → TASK_RESPONSE"
fi

# Snapshot
echo "  Sending TASK_REQUEST_SNAPSHOT..."
RESP=$(run_client snapshot 2400000000 40000000 40000000)
echo "  Response: ${RESP:0:100}..."
if echo "$RESP" | grep -q "TASK_RESPONSE"; then
    pass "TASK_REQUEST_SNAPSHOT → TASK_RESPONSE"
else
    fail "TASK_REQUEST_SNAPSHOT → TASK_RESPONSE"
fi

# ── 5. Clean shutdown timing ──────────────────────────────────────────────────
section "Clean Shutdown (matching systemd TimeoutStopSec=35s)"

echo "  Sending SIGTERM to controller (PID $CTRL_PID)..."
T0=$(date +%s)
kill -SIGTERM "$CTRL_PID"
wait "$CTRL_PID" 2>/dev/null || true
CTRL_PID=""
T1=$(date +%s)
ELAPSED=$(( T1 - T0 ))

echo "  Controller exited in ${ELAPSED}s"
if [[ "$ELAPSED" -lt 35 ]]; then
    pass "Clean shutdown within TimeoutStopSec (${ELAPSED}s < 35s)"
else
    fail "Controller took ${ELAPSED}s to exit — exceeds TimeoutStopSec=35s"
fi

# ── Results ───────────────────────────────────────────────────────────────────
section "Results"

TOTAL=$(( PASS + FAIL ))
echo "  ${PASS} / ${TOTAL} tests passed"
echo

if [[ "$FAIL" -eq 0 ]]; then
    echo "${GREEN}${BOLD}ALL SYSTEMD TESTS PASSED${RESET}"
    exit 0
else
    echo "${RED}${BOLD}${FAIL} TEST(S) FAILED${RESET}"
    echo
    echo "Controller log:"
    cat /tmp/sdr_controller.log
    exit 1
fi
