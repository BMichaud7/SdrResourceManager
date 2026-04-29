# SDR Radio Resource Task Manager
## Version 2.1 — Production Deployment Guide

---

## Quick Start

```bash
git clone https://github.com/YOUR_USERNAME/SdrResourceManager.git
cd SdrResourceManager
./setup.sh          # guided setup: config, build, broker
```

`setup.sh` asks for your board IPs, broker password, and network details, writes `config/devices.xml`, optionally builds the project, and optionally starts the ActiveMQ broker via Docker or Podman. No manual config editing required.

---

## Documentation

| Document | Description |
|----------|-------------|
| **README.md** (this file) | Hardware setup, build, deployment, operations |
| [docs/ICD.md](docs/ICD.md) | Complete AMQP message interface — all request types, responses, scenarios, IQ packet format |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Internal code architecture for contributors |
| [docs/HARDWARE.md](docs/HARDWARE.md) | Supported hardware, per-board configuration reference |

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Hardware RF Setup](#2-hardware-rf-setup)
3. [Reference Clock Distribution](#3-reference-clock-distribution)
4. [Antenna Setup for DF](#4-antenna-setup-for-df)
5. [SMA Port Mapping](#5-sma-port-mapping)
6. [Network Layout](#6-network-layout)
7. [Software Dependencies](#7-software-dependencies)
8. [Building from Source](#8-building-from-source)
9. [Running Tests](#9-running-tests)
10. [Running Locally Without Kubernetes](#10-running-locally-without-kubernetes)
11. [Systemd Service Deployment](#11-systemd-service-deployment)
12. [Kubernetes Deployment](#12-kubernetes-deployment)
13. [Adding More SDR Boards](#13-adding-more-sdr-boards)
14. [Example Workflow](#14-example-workflow)
15. [Troubleshooting](#15-troubleshooting)

---

## 1. System Overview

This application is a **Radio Resource Task Manager** that runs inside a
Kubernetes cluster and manages a pool of SoapySDR-compatible SDR boards. DSP
application pods (Direction Finding, Narrowband, Wideband) request RF
resources via AMQP messages. The controller decides ACCEPT or REJECT, then
streams IQ samples via UDP to the requesting pod.

```
DSP Pods ──AMQP──► sdr-controller ──SoapySDR Remote──► SDR Boards
                         │
                         └──UDP IQ streams──► DSP Pods
```

### Key architectural facts

**Frequency range, bandwidth, and sample rate** are configured per-device in
`devices.xml` under `<capabilities>`. The scheduler rejects any request that
falls outside the union of all loaded device ranges.

**Channel coherence** is controlled per-device by `shared_lo` in `devices.xml`:

- `shared_lo=true` (e.g. AD9361, LimeSDR MIMO): all RX channels on a board
  share one LO and one RF window. The scheduler allocates **spectrum slices
  inside this window** — all concurrent tasks on the device must share
  `center_frequency` and `sample_rate`.
- `shared_lo=false` (e.g. RTL-SDR, HackRF, USRP B210): each channel tunes
  independently; concurrent tasks may use different center frequencies.

**Cross-board coherence**: boards that share an external reference clock
belong to the same `coherency_group`. A DF task can request channels
coherently across all boards in a group by setting `coherency_group` in
the `rf` block.

---

## 2. Hardware RF Setup

### What you need

- N × SoapySDR-compatible SDR boards (see `devices.xml` for supported types)
- 1 × reference clock source shared across all boards (required for coherent
  multi-board DF; GPS-disciplined oscillator or Rubidium standard recommended;
  a good TCXO also works for short sessions). Single-board setups do not
  require an external clock.
- 1 × power splitter: 1-to-N SMA splitter for the reference clock signal
- Cat6 or better Ethernet cable per board (for SoapySDR Remote)
- Appropriate power supply per board (see board datasheet)
- Antennas: see Section 4

### Step-by-step wiring

```
Step 1. Mount boards on DIN rail or in chassis with adequate airflow.
        The FPGA heatsink requires ≥2 cm clearance above.

Step 2. Connect power:
        - USB-C 5V 3A to each board, OR
        - PoE injector if your board supports PoE
        - Do NOT mix USB power with PoE on the same board.

Step 3. Connect Ethernet:
        - One Cat6 cable per board → your cluster network switch
        - Recommended: dedicated 1 GbE switch for SDR traffic
          (IQ bandwidth = sample_rate × 8 bytes/sample, e.g. 61.44 MSPS → ~492 Mbps)

Step 4. Connect reference clock:
        See Section 3.

Step 5. Connect antennas:
        See Sections 4 and 5.

Step 6. Power on boards one at a time.
        Verify each board's IP address from your DHCP server.

Step 7. Install SoapySDRServer on each board's host OS (see Section 6).
```

### Power considerations

- Power draw varies by board — consult your hardware datasheet
- Budget power for RFIC, FPGA/FPGA-less logic, and network interface combined
- Add 20% margin for thermal headroom
- Ensure chassis airflow: most SDR boards will throttle above 70–85°C

---

## 3. Reference Clock Distribution

This is the most important hardware step for coherent operation.

### Why it matters

All boards must phase-lock to the same 10 MHz reference. Without this,
phase measurements between boards are meaningless, and DF/beamforming
will fail.

### Wiring diagram

```
┌──────────────────────────────────────────────────────┐
│  10 MHz Reference Source                              │
│  (GPSDO / Rubidium / TCXO)                           │
│  Output: 0 dBm typical, SMA female                   │
└──────────────────────┬───────────────────────────────┘
                       │ RG-316 coax (keep ≤30 cm)
                       ▼
          ┌────────────────────────┐
          │  1-to-N Power Splitter │  Minicircuits ZFSC-2-1 (2-way)
          │  (SMA, 0–1000 MHz)     │  or ZFSC-4-1 (4-way)
          └────────┬──────┬────────┘
                   │      │         (and more for N boards)
         ┌─────────┘      └──────────┐
         ▼                           ▼
   ┌──────────┐                ┌──────────┐
   │ Board 0  │                │ Board 1  │
   │ REF IN   │                │ REF IN   │
   │ (SMA)    │                │ (SMA)    │
   └──────────┘                └──────────┘
```

### Cable matching

All reference clock cables from splitter to boards MUST be the same
physical length (within ±1 cm). Use a single reel of RG-316 and cut
identically. Phase mismatch from unequal cable length introduces a fixed
bias in your DF solution that is very difficult to calibrate out.

### Reference source requirements

| Parameter         | Minimum       | Recommended          |
|-------------------|---------------|----------------------|
| Frequency         | 10.000 MHz    | 10.000 000 MHz ±1 ppb|
| Output level      | -10 to +5 dBm | 0 dBm                |
| Harmonics         | < -30 dBc     | < -40 dBc            |
| Phase noise       | < -130 dBc/Hz | < -145 dBc/Hz at 1 kHz |
| Connector         | SMA or BNC    | SMA                  |

### Verify lock

After powering the system, run the health query (see Section 12) and
verify `temperature_c` is stable and the device responds. If the board
fails to lock its PLL, SoapySDR will throw an exception during `tune()`.

---

## 4. Antenna Setup for DF

### Coherent aperture requirements

For Direction Finding using phase comparison (MUSIC, ESPRIT, etc.), the
antenna array must satisfy:

1. **Element spacing**: λ/2 at the highest frequency of interest
   - At 915 MHz: spacing = 16.4 cm
   - At 2.4 GHz: spacing = 6.25 cm
   - At 433 MHz: spacing = 34.6 cm

2. **All elements identical**: Same antenna type, same cable length to
   the corresponding RX port. Cable length mismatch shifts the phase
   center and corrupts bearings.

3. **Ground plane**: Use a conductive ground plane (aluminum sheet ≥30×30 cm)
   for vertically polarized antennas.

### Simple 2-element array (1 board)

```
                    d = λ/2
    ┌───────────────────────────────┐
    │                               │
  ANT_0                           ANT_1
   RX0                             RX1
   (Board 0, SMA1)                (Board 0, SMA2)

  Bearing resolution: ~5–10° with MUSIC, N=1024 snapshots
```

### 4-element array (2 boards)

```
    d = λ/2         d = λ/2         d = λ/2
  ┌───────────────────────────────────────────────┐
  │               │               │               │
ANT_0           ANT_1           ANT_2           ANT_3
 RX0             RX1             RX0             RX1
(Board 0)      (Board 0)      (Board 1)       (Board 1)

  Bearing resolution: ~2–3° with MUSIC, N=1024 snapshots
  Requires cross-device coherency (shared 10 MHz reference)
```

### Cable length matching

Measure cable lengths with a VNA or TDR. Lengths must match within
±1 ns of electrical delay (~20 cm of RG-316). If you do not have a VNA,
use a coax phase match kit and cut from the same reel.

---

## 5. RF Port Mapping

Port labeling varies by hardware. The SoapySDR channel index used in
`AssignedStream.channel_index` maps directly to `SOAPY_SDR_RX, N`.

**Example: AD9361-based board (4 SMA connectors)**

```
  ┌─────────────────────────────┐
  │  J1   J2   J3   J4          │
  │  RX0  RX1  TX0  TX1         │
  └─────────────────────────────┘
```

| SoapySDR channel    | Direction | Typical use                        |
|---------------------|-----------|------------------------------------|
| SOAPY_SDR_RX, 0     | Receive   | Primary RX, DF element 0           |
| SOAPY_SDR_RX, 1     | Receive   | Secondary RX, DF element 1         |
| SOAPY_SDR_TX, 0     | Transmit  | TX channel 0                       |
| SOAPY_SDR_TX, 1     | Transmit  | TX channel 1                       |

Consult your board's hardware manual for the physical connector layout.
**Warning**: RX ports on most SDRs are not protected against TX power.
Do not apply more than the board's specified maximum input power to any RX port.

---

## 6. Network Layout

```
┌─────────────────────────────────────────────────────────────────┐
│  Kubernetes Cluster Network (10.0.0.0/24)                       │
│                                                                  │
│  sdr-controller pod    DSP pod (DF)    DSP pod (NB)             │
│  10.0.0.50             10.0.1.10       10.0.1.11                │
│       │                     ▲               ▲                   │
│       │ UDP IQ streams       │               │                   │
│       └─────────────────────┴───────────────┘                   │
└────────────────────────────────────────────────────────────────-┘
         │ SoapySDR Remote TCP connections
         │
┌────────▼────────────────────────────────────────────────────────┐
│  SDR Hardware Network (192.168.10.0/24)                         │
│                                                                  │
│  SDR node 0               SDR node 1                            │
│  192.168.10.100           192.168.10.101                        │
│  SoapySDRServer:55132     SoapySDRServer:55132                  │
│  [SDR board 0]            [SDR board 1]                         │
└─────────────────────────────────────────────────────────────────┘
```

### Network requirements

- SDR hardware nodes must be reachable from the Kubernetes worker node
  running the sdr-controller pod.
- UDP IQ streams go directly from sdr-controller pod to DSP pods.
  Ensure no firewall blocks UDP ports 30000–31999 within the cluster.
- If IQ streaming uses jumbo frames (default 8224-byte packets), enable
  jumbo frames on all switches in the path: `ip link set eth0 mtu 9000`

---

## 7. Software Dependencies

### On SDR hardware nodes (bare metal)

```bash
# Ubuntu 22.04 / 24.04 — base SoapySDR remote server
apt-get install -y soapysdr-tools soapysdr-module-remote

# Install the SoapySDR module for your specific hardware, e.g.:
apt-get install -y soapysdr-module-plutosdr    # PlutoSDR / AD9361 MIMO
apt-get install -y soapysdr-module-rtlsdr      # RTL-SDR
apt-get install -y soapysdr-module-hackrf      # HackRF One
apt-get install -y soapysdr-module-uhd         # USRP (UHD-based)
# See https://github.com/pothosware for all available modules

# Example: build SoapyPlutoSDR from source for AD9361 MIMO support:
git clone https://github.com/pothosware/SoapyPlutoSDR
cd SoapyPlutoSDR && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release && make -j4 && make install

# Start the remote server (runs on port 55132 by default):
SoapySDRServer --bind="0.0.0.0:55132"

# Or as a systemd service:
cat > /etc/systemd/system/soapy-server.service << 'EOF'
[Unit]
Description=SoapySDR Remote Server
After=network.target

[Service]
ExecStart=/usr/local/bin/SoapySDRServer --bind=0.0.0.0:55132
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
systemctl enable --now soapy-server
```

### On the build machine

```bash
# Ubuntu 24.04
apt-get install -y \
    build-essential cmake pkg-config git \
    libsoapysdr-dev soapysdr-module-remote \
    libqpid-proton-cpp12-dev \
    libtinyxml2-dev \
    nlohmann-json3-dev \
    libspdlog-dev libfmt-dev \
    libfftw3-dev \
    uuid-dev \
    tini
```

---

## 8. Building from Source

```bash
# 1. Clone the repository
git clone https://github.com/YOUR_USERNAME/SdrResourceManager.git
cd sdr-controller

# 2. Configure (Release build)
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/opt/sdr-controller

# 3. Build
cmake --build build --parallel $(nproc)

# 4. Install (optional, or run directly from build/)
cmake --install build

# Binaries produced:
#   build/sdr_controller      — main controller
#   build/client/sdr_client   — example AMQP client
#   build/tests/sdr_tests     — GTest unit test binary
```

**CentOS Stream 10** — use the alternate CMakeLists (pkg-config names differ):
```bash
cp CMakeLists.centos.txt CMakeLists.txt
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)
```

**Debug build** (AddressSanitizer + UBSan enabled):
```bash
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --parallel $(nproc)
```

### Build the container image

```bash
# CentOS 10 test image — builds everything and runs GTest on start
podman build -f Containerfile.centos10 -t sdr-controller:test .

# Production Docker image
docker build -t ghcr.io/YOUR_USERNAME/sdr-controller:2.1.0 .
docker push ghcr.io/YOUR_USERNAME/sdr-controller:2.1.0
```

---

## 9. Running Tests

All unit tests use GoogleTest and run without hardware via `FakeSoapyDevice`, an
in-process SoapySDR driver registered as `driver=fake`. Tests cover 121 cases across
all subsystems. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for what each test file covers.

### Run tests locally (Ubuntu)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)
ctest --test-dir build --output-on-failure -V
```

### Run tests in a container (CentOS 10, no hardware needed)

```bash
podman build -f Containerfile.centos10 -t sdr-controller:test .
podman run --rm sdr-controller:test           # exits 0 on pass
podman run --rm sdr-controller:test ctest --output-on-failure -V  # verbose
```

### Test coverage summary

| Test file | Subsystem | Key scenarios |
|---|---|---|
| `test_spectrum_timeline.cpp` | SpectrumTimeline | shared/independent LO, retune conflict, canFit logic |
| `test_message_codec.cpp` | MessageCodec | all request/response encode+decode |
| `test_config_parser.cpp` | ConfigParser | XML parsing, error cases |
| `test_udp_port_pool.cpp` | UdpPortPool | alloc/release/exhaustion |
| `test_resource_manager.cpp` | ResourceManager | scheduling, coherent DF, hardware profiles |
| `test_iq_streamer.cpp` | IQStreamer | packet headers, sequence numbers, overflow flag, error callback |
| `test_fft_engine.cpp` | FftEngine | tone peaks, averaging, plan lifecycle |
| `test_scan_executor.cpp` | ScanExecutor | step order, done callback, repeat mode, stop |
| `test_trigger_monitor.cpp` | TriggerMonitor | threshold trigger, max_captures, done callback |

---

## 10. Running Locally Without Kubernetes

Useful for development and hardware bring-up.

### Prerequisites

1. SoapySDRServer running on each hardware node (see Section 7)
2. ActiveMQ Artemis broker running locally or in Docker:

```bash
docker run -d \
    --name activemq \
    -p 5672:5672 \
    -p 8161:8161 \
    -e ARTEMIS_USER=admin \
    -e ARTEMIS_PASSWORD=admin \
    apache/activemq-artemis:2.36.0
```

3. Edit `config/devices.xml` to point to your hardware:

```xml
<uri>soapy://192.168.10.100:55132</uri>
<streaming_source_ip>YOUR_LOCAL_IP</streaming_source_ip>
```

```xml
<url>amqp://localhost:5672</url>
<username>admin</username>
<password>admin</password>
```

### Run the controller

```bash
export SDR_LOG_LEVEL=debug
./build/sdr_controller config/devices.xml
```

Expected output:
```
[2024-01-01 12:00:00.000] [info] SDR Radio Resource Task Manager v2.1.0
[2024-01-01 12:00:00.010] [info] Config: config/devices.xml
[2024-01-01 12:00:00.050] [info] [pluto-0] Opening: driver=remote uri=soapy://192.168.10.100:55132
[2024-01-01 12:00:01.200] [info] [pluto-0] OK hw=AD9361 drv=remote
[2024-01-01 12:00:01.250] [info] AmqpClient: connected to amqp://localhost:5672
[2024-01-01 12:00:01.260] [info] Controller: running
```

### Run the example client

```bash
# In a separate terminal
./build/client/sdr_client amqp://localhost:5672 127.0.0.1
```

This sends all task types (scheduled, continuous, scan, snapshot, triggered,
health query) and prints responses.

---

## 11. Systemd Service Deployment

The recommended way to run the controller on a bare-metal or VM host (no Kubernetes needed).
`setup.sh` handles all of the steps below automatically when you choose option 2.

### What gets installed

| Path | Contents |
|------|----------|
| `/usr/local/bin/sdr_controller` | Controller binary |
| `/etc/sdr-controller/devices.xml` | Your generated config (root:sdr, 640) |
| `/etc/sdr-controller/broker.env` | Broker credentials (root:sdr, 640) |
| `/etc/systemd/system/sdr-controller.service` | Controller service unit |
| `/etc/systemd/system/activemq-artemis.service` | Broker service unit (if broker is local) |

A `sdr` system user is created and the controller runs as that user.

### Manual install (without setup.sh)

```bash
# 1. Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)

# 2. Create system user and directories
sudo useradd --system --no-create-home --shell /usr/sbin/nologin sdr
sudo mkdir -p /etc/sdr-controller

# 3. Install binary and config
sudo install -m 755 build/sdr_controller /usr/local/bin/sdr_controller
sudo install -m 640 -o root -g sdr config/devices.xml /etc/sdr-controller/devices.xml

# 4. Write broker credentials (used by activemq-artemis.service)
sudo bash -c 'cat > /etc/sdr-controller/broker.env' <<'EOF'
ARTEMIS_USER=sdr_ctrl
ARTEMIS_PASSWORD=your_real_password
EOF
sudo chmod 640 /etc/sdr-controller/broker.env
sudo chown root:sdr /etc/sdr-controller/broker.env

# 5. Install service files
sudo install -m 644 systemd/sdr-controller.service  /etc/systemd/system/
sudo install -m 644 systemd/activemq-artemis.service /etc/systemd/system/

# 6. If broker is on this machine, wire the dependency
sudo sed -i \
    -e 's/^#After=activemq-artemis/After=activemq-artemis/' \
    -e 's/^#Requires=activemq-artemis/Requires=activemq-artemis/' \
    /etc/systemd/system/sdr-controller.service

# 7. Enable and start
sudo systemctl daemon-reload
sudo systemctl enable --now activemq-artemis   # only if broker is local
sudo systemctl enable --now sdr-controller
```

### Managing the service

```bash
# Logs (live tail)
journalctl -u sdr-controller -f

# Status
systemctl status sdr-controller

# Stop / start / restart
systemctl stop    sdr-controller
systemctl start   sdr-controller
systemctl restart sdr-controller

# Change log level without editing the service file
systemctl edit sdr-controller
# Add these lines, save, then: systemctl restart sdr-controller
#   [Service]
#   Environment=SDR_LOG_LEVEL=debug

# Update config after changing devices.xml
sudo cp config/devices.xml /etc/sdr-controller/devices.xml
systemctl restart sdr-controller
```

### Broker web console

When the broker runs locally via the `activemq-artemis` service, the Artemis
web console is available at `http://localhost:8161`. Log in with the username
and password you set during setup to watch queue depths and live messages.

---

## 12. Kubernetes Deployment

### Step 1: Create the sdr-hardware namespace

```bash
kubectl apply -f - <<EOF
apiVersion: v1
kind: Namespace
metadata:
  name: sdr-hardware
EOF
```

### Step 2: Create ExternalName services pointing to your SDR nodes

Edit `k8s/deployment.yaml` to replace the `externalName` values with your
actual SDR node IPs:

```yaml
# Board 0
externalName: 192.168.10.100   # ← your actual IP

# Board 1
externalName: 192.168.10.101   # ← your actual IP
```

Then apply:
```bash
kubectl apply -f k8s/deployment.yaml
```

### Step 3: Create the ConfigMap from your devices.xml

The ConfigMap is embedded in `k8s/deployment.yaml`. To update it from
your local file:

```bash
kubectl create configmap sdr-config \
    --from-file=devices.xml=config/devices.xml \
    -n sdr-system \
    --dry-run=client -o yaml | kubectl apply -f -
```

### Step 4: Push your Docker image

```bash
# Edit k8s/deployment.yaml:
# image: ghcr.io/YOUR_USERNAME/sdr-controller:2.1.0

docker build -t ghcr.io/YOUR_USERNAME/sdr-controller:2.1.0 .
docker push ghcr.io/YOUR_USERNAME/sdr-controller:2.1.0
```

### Step 5: Deploy

```bash
kubectl apply -f k8s/deployment.yaml

# Watch rollout
kubectl rollout status deployment/sdr-controller -n sdr-system

# Verify pod is running
kubectl get pods -n sdr-system
```

### Step 6: Check logs

```bash
kubectl logs -f deployment/sdr-controller -n sdr-system
```

### Why Recreate strategy?

The sdr-controller has **exclusive ownership** of the SDR hardware via
SoapySDR Remote. Two simultaneous instances would:
- Both try to call `SoapySDR::Device::make()` on the same board
- Corrupt hardware state (LO frequency, gain settings)
- Produce corrupted IQ streams

`Recreate` guarantees the old pod fully terminates (SoapySDR connections
closed, streams released) before the new pod starts. The 30-second
`terminationGracePeriodSeconds` allows in-flight tasks to complete.

### How DSP pods interact

DSP pods connect to the AMQP broker and send requests to `sdr.task.request`.
They receive responses on `sdr.task.response` (using their own receiver)
and status events from `sdr.status` (topic, fan-out).

Example DSP pod service account permissions (RBAC not required — all
communication is over AMQP, not Kubernetes API).

---

## 13. Adding More SDR Boards

No code changes required. Follow these steps:

### Hardware

1. Connect the new board to the 10 MHz reference splitter (add a port to
   the splitter if needed — Minicircuits ZFSC-8-1 for 8-way split)
2. Connect Ethernet
3. Install and start SoapySDRServer on the host node
4. Note the board's IP address

### Configuration

Edit `config/devices.xml`, add a new `<device>` block:

```xml
<!-- Set all capability values to match your actual hardware datasheet. -->
<device id="sdr-2">
  <driver>remote</driver>
  <uri>soapy://192.168.10.102:55132</uri>
  <label>SDR Board Unit 2</label>
  <streaming_source_ip>10.0.0.12</streaming_source_ip>

  <!-- shared_lo=true: channels share one LO (AD9361, LimeSDR MIMO).
       shared_lo=false: channels tune independently (RTL-SDR, HackRF, USRP B210). -->
  <shared_lo>true</shared_lo>

  <!-- Assign to a coherency_group if this board shares a reference clock
       with other boards and you want coherent multi-board DF tasks. -->
  <coherency_group>refclk-group-0</coherency_group>

  <!-- All values from hardware datasheet — these are the only constraints
       the scheduler enforces; there are no global hardcoded limits. -->
  <capabilities>
    <rx_channels>2</rx_channels>
    <tx_channels>2</tx_channels>
    <freq_min_hz>70000000</freq_min_hz>       <!-- Hz; AD9361: 70 MHz, HackRF: 1 MHz -->
    <freq_max_hz>6000000000</freq_max_hz>      <!-- Hz; AD9361/HackRF: 6 GHz -->
    <bandwidth_max_hz>56000000</bandwidth_max_hz>      <!-- Hz; AD9361: 56 MHz, LimeSDR: 130 MHz -->
    <sample_rate_max_sps>61440000</sample_rate_max_sps> <!-- sps; AD9361: 61.44 MSPS -->
    <rx_gain_min_db>-3</rx_gain_min_db>
    <rx_gain_max_db>71</rx_gain_max_db>
    <tx_atten_min_db>0</tx_atten_min_db>
    <tx_atten_max_db>89</tx_atten_max_db>
  </capabilities>
</device>
```

Add an ExternalName service for the new board in `k8s/deployment.yaml`.

### Redeploy

```bash
# Update the ConfigMap
kubectl create configmap sdr-config \
    --from-file=devices.xml=config/devices.xml \
    -n sdr-system --dry-run=client -o yaml | kubectl apply -f -

# Roll the deployment (Recreate will stop old pod, start new)
kubectl rollout restart deployment/sdr-controller -n sdr-system

# Verify the new device appears in health
kubectl logs -f deployment/sdr-controller -n sdr-system | grep pluto-2
```

The scheduler automatically incorporates the new device into the resource
pool. No other changes needed.

---

## 14. Example Workflow

### Start the system

```bash
# Terminal 1: Watch controller logs
kubectl logs -f deployment/sdr-controller -n sdr-system

# Terminal 2: Run operations
```

### Request a scheduled DF task

```bash
# From inside a DSP pod or using the example client:
kubectl run sdr-client --rm -it --restart=Never \
    --image=ghcr.io/YOUR_USERNAME/sdr-controller:2.1.0 \
    --namespace=sdr-system \
    -- sdr_client amqp://activemq-service:5672 10.0.1.10
```

The client will:
1. Send `HEALTH_QUERY` → observe device status
2. Send `TASK_REQUEST_SCHEDULED` for 915 MHz, 2 RX, 60s window
3. Receive `TASK_RESPONSE(ACCEPTED)` with `udp_ip:udp_port` for each stream
4. IQ samples begin flowing to UDP port 5000, 5001
5. Send `TASK_REQUEST_CONTINUOUS` for narrowband FM at 162.4 MHz
6. Receive `TASK_RESPONSE(ACCEPTED)` → IQ streams to port 5100
7. Send `TASK_STOP` → streams stop, resources released
8. Send `TASK_REQUEST_SCAN` → 3-step frequency plan
9. Send `TASK_REQUEST_SNAPSHOT` → receive FFT power bins
10. Send `TASK_REQUEST_TRIGGERED` → capture on signal detection
11. Send `TASK_CANCEL`

### Observe IQ stream (with netcat)

On the DSP pod, verify IQ data is arriving:

```bash
# Listen on UDP 5000 and dump 10 packets
nc -u -l 5000 | hexdump -C | head -100
```

Expected first 32 bytes of each packet:
```
49 51 50 30   = "IQP0" magic
xx xx xx xx   = sequence number
xx xx xx xx xx xx xx xx = timestamp_ns
xx xx xx xx xx xx xx xx = center_freq_hz
xx xx xx xx   = sample_rate
xx xx         = num_samples (= 0x0400 = 1024)
xx            = channel_index
02            = flags (IQ_FLAG_FIRST_PACKET set on first packet)
```

### Run a continuous task and stop it

```bash
# Send continuous task via AMQP (using Python or the example client)
python3 - <<'EOF'
import json, uuid, time
from proton.utils import BlockingConnection

conn = BlockingConnection("amqp://activemq-service.sdr-system:5672",
                          user="sdr_ctrl", password="YOUR_BROKER_PASSWORD")
sender = conn.create_sender("sdr.task.request")
receiver = conn.create_receiver("sdr.task.response")

# Send continuous task
task_req = {
    "msg_type": "TASK_REQUEST_CONTINUOUS",
    "schema_version": "2.0",
    "timestamp_ms": int(time.time()*1000),
    "request_id": str(uuid.uuid4()),
    "task_type": "NARROWBAND",
    "schedule": {"mode": "CONTINUOUS"},
    "rf": {
        "center_freq_hz": 162400000.0,
        "bandwidth_hz": 200000.0,
        "sample_rate_sps": 250000.0,
        "rx_count": 1,
        "rx_gain_db": [40.0]
    },
    "streaming": {"dest_ip": "10.0.1.11", "dest_ports": [5100]}
}
sender.send(json.dumps(task_req))
resp = json.loads(receiver.receive(timeout=5).body)
print("Response:", json.dumps(resp, indent=2))
task_id = resp.get("task_id")

print(f"\nTask {task_id} is running. Press Enter to stop...")
input()

# Stop the task
stop_req = {
    "msg_type": "TASK_STOP",
    "schema_version": "2.0",
    "timestamp_ms": int(time.time()*1000),
    "request_id": str(uuid.uuid4()),
    "task_id": task_id
}
sender.send(json.dumps(stop_req))
resp = json.loads(receiver.receive(timeout=5).body)
print("Stop response:", json.dumps(resp, indent=2))
conn.close()
EOF
```

---

## 15. Troubleshooting

### Controller fails to open device

```
[error] [pluto-0] open() exception: SoapySDR::Device::make() failed
```

- Verify SoapySDRServer is running on the hardware node: `nc -zv 192.168.10.100 55132`
- Check ExternalName service DNS: `kubectl exec -it sdr-controller -n sdr-system -- nslookup pluto-sdr-0.sdr-hardware`
- Verify the board is powered and has an IP address

### AMQP connection refused

```
[error] AmqpClient: transport error: Connection refused
```

- Verify ActiveMQ is running: `kubectl get pods -n sdr-system | grep activemq`
- Check broker URL in ConfigMap matches the actual service name

### Task rejected with RETUNE_CONFLICT

The device is already tuned to a different frequency for an active task.
Either wait for the active task to complete, or request a task on the same
center frequency, or use a different device.

### IQ stream not arriving at DSP pod

1. Verify `dest_ip` in the request is the actual pod IP, not a service IP
2. Check MTU: if using default 1024-sample packets (8224 bytes), jumbo frames
   are required. Reduce `iq_packet_samples` to 183 in `devices.xml` for standard MTU.
3. Verify no NetworkPolicy blocks UDP: `kubectl describe networkpolicy -n sdr-system`

### Overflow errors in IQ stream

```
[warn] IQStreamer [task001-RX-pluto-0-0] overflow #1
```

The SoapySDR read buffer overflowed. This means the processing thread
cannot keep up. Solutions:
1. Use a faster network (10 GbE) or reduce sample rate
2. Enable jumbo frames to reduce packet count per second
3. Check for CPU contention on the controller pod — increase CPU limits

### Device temperature too high

Monitor via health topic. If `temperature_c > 70°C`:
- Improve airflow around the FPGA heatsink
- Reduce sample rate (lower power draw)
- In Kubernetes, add a resource limit for CPU to prevent the FPGA from being
  driven too hard by concurrent tasks

---

## Support and Contact

This system is designed and maintained by the SDR Engineering Team.
For issues, file a ticket referencing the ICD document version (SDR-RRTM-ICD-002 v2.1).
