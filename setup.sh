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

# setup.sh — Interactive first-time setup for SDR Radio Resource Manager
# Generates config/devices.xml from your answers, then optionally builds
# and starts the ActiveMQ broker.

set -euo pipefail

# ── Colours ──────────────────────────────────────────────────────────────────
BOLD=$'\e[1m'; DIM=$'\e[2m'; RESET=$'\e[0m'
GREEN=$'\e[32m'; CYAN=$'\e[36m'; YELLOW=$'\e[33m'; RED=$'\e[31m'

# ── Helpers ───────────────────────────────────────────────────────────────────
ask() {
    # ask VARNAME "Prompt text" "default value"
    local var="$1" prompt="$2" default="${3:-}"
    if [[ -n "$default" ]]; then
        printf "%s %s[%s]%s " "$prompt" "$DIM" "$default" "$RESET"
    else
        printf "%s " "$prompt"
    fi
    local answer
    read -r answer
    printf -v "$var" '%s' "${answer:-$default}"
}

ask_secret() {
    # ask_secret VARNAME "Prompt text"
    local var="$1" prompt="$2"
    printf "%s " "$prompt"
    local answer
    read -rs answer
    echo
    printf -v "$var" '%s' "$answer"
}

confirm() {
    # confirm "Question?" — returns 0 for yes, 1 for no
    local prompt="$1"
    printf "%s %s[Y/n]%s " "$prompt" "$DIM" "$RESET"
    local answer
    read -r answer
    [[ "${answer,,}" != "n" ]]
}

banner() {
    echo
    echo "${BOLD}${CYAN}══════════════════════════════════════════════════════${RESET}"
    echo "${BOLD}${CYAN}  $1${RESET}"
    echo "${BOLD}${CYAN}══════════════════════════════════════════════════════${RESET}"
    echo
}

section() {
    echo
    echo "${BOLD}── $1 ──────────────────────────────────────────────────${RESET}"
    echo
}

ok()   { echo "  ${GREEN}✓${RESET}  $1"; }
warn() { echo "  ${YELLOW}⚠${RESET}  $1"; }
err()  { echo "  ${RED}✗${RESET}  $1"; }

# ── Hardware presets ──────────────────────────────────────────────────────────
# Fields: rx tx shared_lo freq_min freq_max bw_max sr_max gain_min gain_max tx_atten_min tx_atten_max
declare -A HW_RX HW_TX HW_SHARED_LO HW_FMIN HW_FMAX HW_BW HW_SR HW_GMIN HW_GMAX HW_TMIN HW_TMAX

set_preset() {
    local k=$1
    HW_RX[$k]=$2;  HW_TX[$k]=$3;  HW_SHARED_LO[$k]=$4
    HW_FMIN[$k]=$5; HW_FMAX[$k]=$6; HW_BW[$k]=$7;  HW_SR[$k]=$8
    HW_GMIN[$k]=$9; HW_GMAX[$k]=${10}; HW_TMIN[$k]=${11}; HW_TMAX[$k]=${12}
}

# Name              rx tx shared  fmin        fmax         bw_max    sr_max    gmin gmax  tmin tmax
set_preset plutosdr  2  2  true   70000000    6000000000  56000000  61440000   -3   71     0   89
set_preset hackrf    1  1  false   1000000    6000000000  20000000  20000000    0   47     0   47
set_preset rtlsdr    1  0  false  24000000    1766000000   3200000   3200000    0   50     0    0
set_preset limesdr   2  2  true  100000      3800000000 130000000  61440000  -12   61     0   60
set_preset usrp      2  2  false  70000000    6000000000  56000000  61440000  -3   89     0   89

PRESET_NAMES=("PlutoSDR / AD9361" "HackRF One" "RTL-SDR" "LimeSDR" "USRP B210/B200" "Custom")
PRESET_KEYS=(plutosdr hackrf rtlsdr limesdr usrp custom)

# ── Main ──────────────────────────────────────────────────────────────────────
clear
banner "SDR Radio Resource Manager — Setup"

cat <<'EOF'
This script will:
  1. Ask for your broker and network details
  2. Ask about each SDR board you want to register
  3. Write config/devices.xml
  4. Optionally build the project and start the broker

Press Enter to accept the value shown in [brackets].

EOF

# ─────────────────────────────────────────────────────────────────────────────
section "AMQP Broker"

echo "The controller talks to an ActiveMQ Artemis broker over AMQP 1.0."
echo "If you don't have one running yet, this script can start it with Docker/Podman."
echo

ask BROKER_HOST  "  Broker hostname or IP:" "localhost"
ask BROKER_PORT  "  Broker AMQP port:"      "5672"
ask BROKER_USER  "  Broker username:"       "sdr_ctrl"
ask_secret BROKER_PASS "  Broker password (hidden):"
while [[ -z "$BROKER_PASS" ]]; do
    err "Password cannot be empty."
    ask_secret BROKER_PASS "  Broker password (hidden):"
done

BROKER_URL="amqp://${BROKER_HOST}:${BROKER_PORT}"

# ─────────────────────────────────────────────────────────────────────────────
section "Streaming Network"

echo "When the controller accepts a task it sends IQ samples (UDP) from its own IP."
echo "Enter the IP address of the machine that will run sdr_controller."
echo

ask STREAMING_IP "  Controller / streaming source IP:" ""
while [[ -z "$STREAMING_IP" ]]; do
    err "Streaming IP cannot be empty."
    ask STREAMING_IP "  Controller / streaming source IP:" ""
done

# ─────────────────────────────────────────────────────────────────────────────
section "IQ Packet Size"

echo "Default: 1024 samples/packet = 8224-byte packets (requires jumbo frames, MTU ≥ 9000)."
echo "If your network uses standard 1500-byte MTU, use 183 samples/packet instead."
echo

ask IQ_SAMPLES "  Samples per IQ packet (183 for standard MTU, 1024 for jumbo):" "1024"

# ─────────────────────────────────────────────────────────────────────────────
section "SDR Boards"

ask NUM_BOARDS "  How many SDR boards do you want to register?" "1"
while ! [[ "$NUM_BOARDS" =~ ^[1-9][0-9]*$ ]]; do
    err "Enter a number ≥ 1."
    ask NUM_BOARDS "  How many SDR boards?" "1"
done

DEVICES_XML=""

for (( i=0; i<NUM_BOARDS; i++ )); do
    IDX=$i
    NUM=$((i+1))
    echo
    echo "${BOLD}  Board $NUM of $NUM_BOARDS${RESET}"
    echo "  ──────────────────────────────────────────"

    ask DEV_ID    "    Device ID (short name, no spaces):" "sdr-${IDX}"
    ask DEV_LABEL "    Label (human-readable name):"       "SDR Board Unit ${IDX}"
    ask DEV_IP    "    SoapySDRServer IP address:"         ""
    while [[ -z "$DEV_IP" ]]; do
        err "    IP cannot be empty."
        ask DEV_IP "    SoapySDRServer IP address:" ""
    done
    ask DEV_PORT  "    SoapySDRServer port:"               "55132"

    echo
    echo "    Hardware type:"
    for j in "${!PRESET_NAMES[@]}"; do
        printf "      %d) %s\n" "$((j+1))" "${PRESET_NAMES[$j]}"
    done
    ask HW_CHOICE "    Choose [1-${#PRESET_NAMES[@]}]:" "1"
    while ! [[ "$HW_CHOICE" =~ ^[1-6]$ ]]; do
        err "    Enter a number between 1 and ${#PRESET_NAMES[@]}."
        ask HW_CHOICE "    Choose [1-${#PRESET_NAMES[@]}]:" "1"
    done
    PRESET_KEY="${PRESET_KEYS[$((HW_CHOICE-1))]}"

    if [[ "$PRESET_KEY" == "custom" ]]; then
        ask DEV_RX_CH  "    RX channels:"                "2"
        ask DEV_TX_CH  "    TX channels:"                "2"
        ask DEV_SHARED "    Channels share one LO? (true/false):" "true"
        ask DEV_FMIN   "    Min frequency (Hz):"         "70000000"
        ask DEV_FMAX   "    Max frequency (Hz):"         "6000000000"
        ask DEV_BW     "    Max bandwidth (Hz):"         "56000000"
        ask DEV_SR     "    Max sample rate (sps):"      "61440000"
        ask DEV_GMIN   "    RX gain min (dB):"           "-3"
        ask DEV_GMAX   "    RX gain max (dB):"           "71"
        ask DEV_TMIN   "    TX attenuation min (dB):"    "0"
        ask DEV_TMAX   "    TX attenuation max (dB):"    "89"
    else
        DEV_RX_CH="${HW_RX[$PRESET_KEY]}"
        DEV_TX_CH="${HW_TX[$PRESET_KEY]}"
        DEV_SHARED="${HW_SHARED_LO[$PRESET_KEY]}"
        DEV_FMIN="${HW_FMIN[$PRESET_KEY]}"
        DEV_FMAX="${HW_FMAX[$PRESET_KEY]}"
        DEV_BW="${HW_BW[$PRESET_KEY]}"
        DEV_SR="${HW_SR[$PRESET_KEY]}"
        DEV_GMIN="${HW_GMIN[$PRESET_KEY]}"
        DEV_GMAX="${HW_GMAX[$PRESET_KEY]}"
        DEV_TMIN="${HW_TMIN[$PRESET_KEY]}"
        DEV_TMAX="${HW_TMAX[$PRESET_KEY]}"
        ok "    Using preset: ${PRESET_NAMES[$((HW_CHOICE-1))]}"
    fi

    # Coherency group (for multi-board coherent DF)
    DEV_COHERENCY=""
    if [[ "$NUM_BOARDS" -gt 1 ]]; then
        echo
        if confirm "    Add this board to a coherency group (required for multi-board DF)?"; then
            ask DEV_COHERENCY "    Coherency group name:" "refclk-group-0"
        fi
    fi

    COHERENCY_BLOCK=""
    if [[ -n "$DEV_COHERENCY" ]]; then
        COHERENCY_BLOCK="      <coherency_group>${DEV_COHERENCY}</coherency_group>"$'\n'
    fi

    DEVICES_XML+=$(cat <<BLOCK

    <device id="${DEV_ID}">
      <driver>remote</driver>
      <uri>soapy://${DEV_IP}:${DEV_PORT}</uri>
      <label>${DEV_LABEL}</label>
      <streaming_source_ip>${STREAMING_IP}</streaming_source_ip>
      <shared_lo>${DEV_SHARED}</shared_lo>
${COHERENCY_BLOCK}      <capabilities>
        <rx_channels>${DEV_RX_CH}</rx_channels>
        <tx_channels>${DEV_TX_CH}</tx_channels>
        <freq_min_hz>${DEV_FMIN}</freq_min_hz>
        <freq_max_hz>${DEV_FMAX}</freq_max_hz>
        <bandwidth_max_hz>${DEV_BW}</bandwidth_max_hz>
        <sample_rate_max_sps>${DEV_SR}</sample_rate_max_sps>
        <rx_gain_min_db>${DEV_GMIN}</rx_gain_min_db>
        <rx_gain_max_db>${DEV_GMAX}</rx_gain_max_db>
        <tx_atten_min_db>${DEV_TMIN}</tx_atten_min_db>
        <tx_atten_max_db>${DEV_TMAX}</tx_atten_max_db>
      </capabilities>
    </device>
BLOCK
)
done

# ─────────────────────────────────────────────────────────────────────────────
section "Summary"

echo "  Broker:    ${BROKER_URL}  (user: ${BROKER_USER})"
echo "  Stream IP: ${STREAMING_IP}"
echo "  IQ packet: ${IQ_SAMPLES} samples/packet"
echo "  Boards:    ${NUM_BOARDS}"
echo

if ! confirm "Write config/devices.xml with these settings?"; then
    echo "Aborted. Nothing written."
    exit 0
fi

# ─────────────────────────────────────────────────────────────────────────────
# Write devices.xml
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="${SCRIPT_DIR}/config"
mkdir -p "$CONFIG_DIR"
CONFIG_FILE="${CONFIG_DIR}/devices.xml"

cat > "$CONFIG_FILE" <<XML
<?xml version="1.0" encoding="UTF-8"?>
<!--
  SDR Radio Resource Controller — Device and Policy Configuration
  Generated by setup.sh on $(date '+%Y-%m-%d %H:%M:%S')
  To add a board: copy a <device> block, change id/uri/label/ip. No code changes needed.
-->
<sdr_controller version="2.0">

  <!-- AMQP Broker -->
  <broker>
    <url>${BROKER_URL}</url>
    <username>${BROKER_USER}</username>
    <password>${BROKER_PASS}</password>
    <request_queue>sdr.task.request</request_queue>
    <response_queue>sdr.task.response</response_queue>
    <status_topic>sdr.status</status_topic>
    <health_topic>sdr.health</health_topic>
    <reconnect_interval_sec>5</reconnect_interval_sec>
    <max_reconnect_interval_sec>60</max_reconnect_interval_sec>
    <send_queue_depth>512</send_queue_depth>
  </broker>

  <!-- Scheduling Policy -->
  <policy>
    <max_concurrent_tasks>64</max_concurrent_tasks>
    <guard_band_hz>200000</guard_band_hz>
    <usable_bw_fraction>0.80</usable_bw_fraction>
    <default_task_timeout_ms>3600000</default_task_timeout_ms>
    <scheduler_tick_ms>200</scheduler_tick_ms>
    <watchdog_tick_ms>1000</watchdog_tick_ms>
    <udp_port_pool_start>30000</udp_port_pool_start>
    <udp_port_pool_end>31999</udp_port_pool_end>
    <iq_packet_samples>${IQ_SAMPLES}</iq_packet_samples>
    <retune_conflict_policy>REJECT_NEW</retune_conflict_policy>
    <heartbeat_interval_ms>30000</heartbeat_interval_ms>
  </policy>

  <!-- SDR Devices -->
  <devices>${DEVICES_XML}
  </devices>

</sdr_controller>
XML

ok "Wrote ${CONFIG_FILE}"

# ─────────────────────────────────────────────────────────────────────────────
section "Build"

BUILD_OK=false
if confirm "Build sdr_controller now?"; then
    if [[ -f "${SCRIPT_DIR}/CMakeLists.centos.txt" ]] && grep -qi "centos\|rhel\|rocky\|alma" /etc/os-release 2>/dev/null; then
        warn "CentOS/RHEL detected — using CMakeLists.centos.txt"
        cp "${SCRIPT_DIR}/CMakeLists.centos.txt" "${SCRIPT_DIR}/CMakeLists.txt"
    fi
    cmake -B "${SCRIPT_DIR}/build" -DCMAKE_BUILD_TYPE=Release -S "${SCRIPT_DIR}"
    cmake --build "${SCRIPT_DIR}/build" --parallel "$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
    ok "Build complete — binaries in build/"
    BUILD_OK=true
fi

# ─────────────────────────────────────────────────────────────────────────────
section "Deployment Method"

echo "  1) Run manually   — start the controller yourself each time"
echo "  2) systemd service — install as a system service that starts on boot"
echo
ask DEPLOY_METHOD "  Choose [1/2]:" "1"
while [[ "$DEPLOY_METHOD" != "1" && "$DEPLOY_METHOD" != "2" ]]; do
    err "Enter 1 or 2."
    ask DEPLOY_METHOD "  Choose [1/2]:" "1"
done

# ─────────────────────────────────────────────────────────────────────────────
if [[ "$DEPLOY_METHOD" == "2" ]]; then

    section "Systemd Install"

    if ! command -v systemctl &>/dev/null; then
        err "systemd not found on this system — falling back to manual mode."
        DEPLOY_METHOD=1
    elif ! sudo -v 2>/dev/null; then
        err "sudo access is required to install systemd services — falling back to manual mode."
        DEPLOY_METHOD=1
    fi
fi

if [[ "$DEPLOY_METHOD" == "2" ]]; then

    # Create system user
    if ! id sdr &>/dev/null 2>&1; then
        sudo useradd --system --no-create-home --shell /usr/sbin/nologin sdr
        ok "Created system user 'sdr'"
    else
        ok "System user 'sdr' already exists"
    fi

    # Install binary
    if [[ "$BUILD_OK" == true && -f "${SCRIPT_DIR}/build/sdr_controller" ]]; then
        sudo install -m 755 "${SCRIPT_DIR}/build/sdr_controller" /usr/local/bin/sdr_controller
        ok "Installed /usr/local/bin/sdr_controller"
    else
        warn "Binary not built — build first, then copy to /usr/local/bin/sdr_controller"
    fi

    # Install config directory and devices.xml
    sudo mkdir -p /etc/sdr-controller
    sudo install -m 640 -o root -g sdr "${CONFIG_FILE}" /etc/sdr-controller/devices.xml
    ok "Installed /etc/sdr-controller/devices.xml"

    # Write broker.env (credentials for the activemq service)
    TMPENV=$(mktemp)
    printf 'ARTEMIS_USER=%s\nARTEMIS_PASSWORD=%s\n' "${BROKER_USER}" "${BROKER_PASS}" > "$TMPENV"
    sudo install -m 640 "$TMPENV" /etc/sdr-controller/broker.env
    sudo chown root:sdr /etc/sdr-controller/broker.env 2>/dev/null || true
    rm "$TMPENV"
    ok "Wrote /etc/sdr-controller/broker.env (mode 640, root:sdr)"

    # Install sdr-controller service
    sudo install -m 644 "${SCRIPT_DIR}/systemd/sdr-controller.service" \
        /etc/systemd/system/sdr-controller.service
    ok "Installed /etc/systemd/system/sdr-controller.service"

    # Install broker service if broker is local
    LOCAL_BROKER=false
    if [[ "$BROKER_HOST" == "localhost" || "$BROKER_HOST" == "127.0.0.1" ]]; then
        if command -v podman &>/dev/null; then     RUNTIME_BIN="$(command -v podman)"
        elif command -v docker &>/dev/null; then   RUNTIME_BIN="$(command -v docker)"
        else                                       RUNTIME_BIN=""
        fi

        if [[ -n "$RUNTIME_BIN" ]]; then
            # Patch the service file to use the detected runtime
            sed "s|/usr/bin/podman|${RUNTIME_BIN}|g" \
                "${SCRIPT_DIR}/systemd/activemq-artemis.service" \
                | sudo tee /etc/systemd/system/activemq-artemis.service >/dev/null
            ok "Installed /etc/systemd/system/activemq-artemis.service (${RUNTIME_BIN})"
            LOCAL_BROKER=true

            # Wire the dependency into the controller service
            sudo sed -i \
                -e 's/^#After=activemq-artemis/After=activemq-artemis/' \
                -e 's/^#Requires=activemq-artemis/Requires=activemq-artemis/' \
                /etc/systemd/system/sdr-controller.service
        else
            warn "No container runtime found — broker service not installed."
            warn "Start the broker separately, then: systemctl start sdr-controller"
        fi
    fi

    # Reload systemd and enable services
    sudo systemctl daemon-reload

    if [[ "$LOCAL_BROKER" == true ]]; then
        sudo systemctl enable --now activemq-artemis.service
        ok "activemq-artemis.service enabled and started"
    fi

    sudo systemctl enable --now sdr-controller.service
    ok "sdr-controller.service enabled and started"

    section "Done"

    echo "  ${BOLD}Service is running.${RESET} Useful commands:"
    echo
    echo "  View logs:        ${DIM}journalctl -u sdr-controller -f${RESET}"
    echo "  Status:           ${DIM}systemctl status sdr-controller${RESET}"
    echo "  Stop:             ${DIM}systemctl stop sdr-controller${RESET}"
    echo "  Restart:          ${DIM}systemctl restart sdr-controller${RESET}"
    echo "  Edit log level:   ${DIM}systemctl edit sdr-controller${RESET}  (add Environment=SDR_LOG_LEVEL=debug)"
    echo "  Update config:    ${DIM}sudo cp config/devices.xml /etc/sdr-controller/devices.xml${RESET}"
    echo "                    ${DIM}systemctl restart sdr-controller${RESET}"
    if [[ "$LOCAL_BROKER" == true ]]; then
        echo "  Broker console:   ${DIM}http://localhost:8161${RESET}  (${BROKER_USER} / <your password>)"
    fi
    echo
    echo "  On each SDR hardware node, make sure SoapySDRServer is running:"
    echo "    ${DIM}SoapySDRServer --bind=0.0.0.0:55132${RESET}"
    echo

# ─────────────────────────────────────────────────────────────────────────────
else  # manual deployment

    section "Broker"

    BROKER_STARTED=false
    if [[ "$BROKER_HOST" == "localhost" || "$BROKER_HOST" == "127.0.0.1" ]]; then
        if confirm "Start ActiveMQ Artemis broker locally now (needs Docker or Podman)?"; then
            if command -v podman &>/dev/null; then RUNTIME=podman
            elif command -v docker &>/dev/null; then RUNTIME=docker
            else
                err "Neither docker nor podman found — skipping broker start."
                RUNTIME=""
            fi

            if [[ -n "$RUNTIME" ]]; then
                echo "  Starting broker with $RUNTIME..."
                $RUNTIME run -d --name activemq-sdr \
                    -p "${BROKER_PORT}:5672" \
                    -p 8161:8161 \
                    -e ARTEMIS_USER="${BROKER_USER}" \
                    -e ARTEMIS_PASSWORD="${BROKER_PASS}" \
                    apache/activemq-artemis:2.36.0
                ok "Broker started (web console → http://localhost:8161)"
                BROKER_STARTED=true
            fi
        fi
    fi

    section "Done"

    echo "  ${BOLD}Next steps:${RESET}"
    echo
    if [[ "$BROKER_STARTED" != "true" ]]; then
        echo "  1. Start your ActiveMQ Artemis broker and make sure it's reachable at:"
        echo "     ${BROKER_URL}"
        echo
    fi

    if [[ -f "${SCRIPT_DIR}/build/sdr_controller" ]]; then
        echo "  • Run the controller:"
        echo "    ${DIM}export SDR_LOG_LEVEL=debug${RESET}"
        echo "    ${DIM}${SCRIPT_DIR}/build/sdr_controller ${CONFIG_FILE}${RESET}"
    else
        echo "  • Build the controller first:"
        echo "    ${DIM}cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel \$(nproc)${RESET}"
    fi

    echo
    echo "  • On each SDR hardware node, make sure SoapySDRServer is running:"
    echo "    ${DIM}SoapySDRServer --bind=0.0.0.0:55132${RESET}"
    echo
    echo "  Tip: re-run setup.sh and choose option 2 to install as a systemd service."
    echo

fi

echo "  Full docs: README.md, docs/ICD.md, docs/ARCHITECTURE.md"
echo
echo "${GREEN}${BOLD}Setup complete.${RESET}"
echo

# ========================================================================
# End of file — OpenRFStack
# Subject to Personal Use License
# https://github.com/OpenRFStack
# ========================================================================
