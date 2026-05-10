#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

BROKER_PORT=5675
BROKER_URL="amqp://localhost:${BROKER_PORT}"
TEST="${1:-}"

echo "=== Building sdr-controller:integ ==="
podman build --target runtime -t sdr-controller:integ .

echo "=== Building sdr-controller:test-integ ==="
podman build --target test-integ -t sdr-controller:test-integ .

echo "=== Starting broker ==="
podman run -d --rm --name ctrl-integ-broker \
  -e ARTEMIS_USER=sdr_ctrl -e ARTEMIS_PASSWORD=test_password \
  -p "${BROKER_PORT}:5672" apache/activemq-artemis:2.36.0

echo -n "Waiting for broker..."
for i in $(seq 1 30); do
  podman exec ctrl-integ-broker \
    /var/lib/artemis-instance/bin/artemis check node --up >/dev/null 2>&1 && echo " ready" && break
  echo -n "."; sleep 2
done

echo "=== Starting controller ==="
podman run -d --rm --name ctrl-integ \
  --network=host \
  -v "$(pwd)/test-env/config/devices-integ.xml:/etc/sdr-controller/devices.xml:ro,z" \
  -e SDR_LOG_LEVEL=info \
  localhost/sdr-controller:integ
sleep 4

echo "=== Running integration tests ==="
if [ -n "$TEST" ]; then
  podman run --rm --network=host sdr-controller:test-integ \
    python3 /e2e_test.py --broker "$BROKER_URL" --test "$TEST"
else
  podman run --rm --network=host sdr-controller:test-integ \
    python3 /e2e_test.py --broker "$BROKER_URL"
fi
RC=$?

echo "=== Tearing down ==="
podman stop ctrl-integ ctrl-integ-broker 2>/dev/null || true
exit $RC
