#!/bin/sh
# entrypoint-pi4.sh — Start broker, GPS daemon, and SDR services.
# Set ENABLE_RECON=1 to also start sdr_recon.py (recon image).
# Runs as PID 1 via tini. Exits if any critical service dies.
set -e

GPS_DEVICE="${GPS_DEVICE:-/dev/ttyACM0}"
SDR_LOG_LEVEL="${SDR_LOG_LEVEL:-info}"
ENABLE_RECON="${ENABLE_RECON:-0}"
RECON_OUT="${RECON_OUT:-/recon}"
RECON_SNR_MIN="${RECON_SNR_MIN:-15}"
RECON_CAPTURE_S="${RECON_CAPTURE_S:-10}"
RECON_GAIN="${RECON_GAIN:-40}"
RECON_COOLDOWN="${RECON_COOLDOWN:-60}"
RECON_FORMAT="${RECON_FORMAT:-cf32}"
RECON_DB="${RECON_DB:-}"

log() { echo "[entrypoint] $*"; }

# ── 1. Start qdrouterd ────────────────────────────────────────────────────
log "Starting qdrouterd..."
qdrouterd -c /etc/qdrouterd/qdrouterd.conf &
BROKER_PID=$!

i=0
until nc -z 127.0.0.1 5672 2>/dev/null; do
    i=$((i+1))
    if [ $i -ge 30 ]; then
        log "ERROR: qdrouterd did not start within 15s"
        exit 1
    fi
    sleep 0.5
done
log "Broker ready on :5672"

# ── 2. Start gpsd (optional — only if GPS device present) ────────────────
GPS_ENABLED=0
if [ -e "$GPS_DEVICE" ]; then
    log "GPS device $GPS_DEVICE found — starting gpsd..."
    gpsd -n -G "$GPS_DEVICE"
    GPS_ENABLED=1
    sleep 1
    log "gpsd started"
else
    log "No GPS device at $GPS_DEVICE — skipping gpsd and sdr_gps"
fi

# ── 3. sdr_controller ─────────────────────────────────────────────────────
log "Starting sdr_controller..."
SDR_LOG_LEVEL="$SDR_LOG_LEVEL" \
    /usr/local/bin/sdr_controller /etc/sdr-controller/devices.xml &
CTRL_PID=$!
sleep 1

# ── 4. sdr_acquisition ────────────────────────────────────────────────────
log "Starting sdr_acquisition..."
SDR_LOG_LEVEL="$SDR_LOG_LEVEL" \
    /usr/local/bin/sdr_acquisition /etc/sdr-acquisition/scanner.xml &
ACQ_PID=$!

# ── 5. sdr_recon (optional — recon image only) ───────────────────────────
RECON_PID=""
if [ "$ENABLE_RECON" = "1" ]; then
    mkdir -p "$RECON_OUT"
    log "Starting sdr_recon (snr_min=${RECON_SNR_MIN} capture=${RECON_CAPTURE_S}s out=${RECON_OUT})..."
    _recon_db_arg=""
    [ -n "$RECON_DB" ] && _recon_db_arg="--db $RECON_DB"
    python3 /usr/local/bin/sdr_recon.py \
        --out       "$RECON_OUT" \
        --snr-min   "$RECON_SNR_MIN" \
        --capture-s "$RECON_CAPTURE_S" \
        --gain      "$RECON_GAIN" \
        --cooldown  "$RECON_COOLDOWN" \
        --format    "$RECON_FORMAT" \
        ${_recon_db_arg} \
        &
    RECON_PID=$!
fi

# ── 6. sdr_gps (only if GPS daemon running) ──────────────────────────────
GPS_PID=""
if [ $GPS_ENABLED -eq 1 ]; then
    log "Starting sdr_gps..."
    /usr/local/bin/sdr_gps /etc/sdr-gps/gps.xml &
    GPS_PID=$!
fi

log "All services running. PIDs: broker=$BROKER_PID ctrl=$CTRL_PID acq=$ACQ_PID recon=${RECON_PID:-none} gps=${GPS_PID:-none}"

wait -n $BROKER_PID $CTRL_PID $ACQ_PID ${RECON_PID:+$RECON_PID} ${GPS_PID:+$GPS_PID}
log "A service exited — shutting down."
kill $BROKER_PID $CTRL_PID $ACQ_PID ${RECON_PID:+$RECON_PID} ${GPS_PID:+$GPS_PID} 2>/dev/null || true
wait
