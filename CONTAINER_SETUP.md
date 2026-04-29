# SDR Controller — Podman Container Setup Guide
## CentOS Stream 10

---

## What this container is

A single self-contained image built on CentOS Stream 10 that includes:

- The complete C++20 source code for the SDR Radio Resource Task Manager
- All build tools and libraries (GCC 14, CMake, SoapySDR, qpid-proton, etc.)
- Apache ActiveMQ Artemis (the AMQP 1.0 broker the controller talks to)
- The compiled `sdr_controller` and `sdr_client` binaries, ready to run
- A mock device config so everything runs without real SDR hardware

The controller will start, connect to the broker, and process requests. Since no real PlutoSDR hardware is attached, device-open will fail gracefully (logged as WARN) and tasks will be rejected with `NO_DEVICE_AVAILABLE` — but all AMQP messaging, scheduling logic, and codec paths exercise fully. When you have real hardware, you swap in the real `devices.xml`.

---

## Step 1 — Prerequisites on your CentOS 10 machine

You need `podman` installed. On CentOS Stream 10, it is in the base repos:

```bash
sudo dnf install -y podman
```

Verify it works:

```bash
podman --version
# Should print: podman version 5.x.x or similar
```

You do **not** need Docker, Docker Compose, or root access for the container itself. Podman runs rootless by default.

---

## Step 2 — Get the source code onto your machine

Copy the entire `sdr-v3/` project directory to your CentOS machine. If you are transferring from another machine:

```bash
# On your source machine — create the archive
tar -czf sdr-v3.tar.gz sdr-v3/

# Copy to CentOS machine (adjust IP/path)
scp sdr-v3.tar.gz user@your-centos-machine:~/

# On the CentOS machine — extract it
tar -xzf sdr-v3.tar.gz
cd sdr-v3
```

Your directory should look like this when you run `ls` inside `sdr-v3/`:

```
CMakeLists.txt   Containerfile.centos10   Dockerfile
README.md        client/   config/   docs/   include/   k8s/   src/   test-env/
```

**The `Containerfile.centos10` must be in that directory.**

---

## Step 3 — Build the container image

From inside the `sdr-v3/` directory, run:

```bash
podman build \
  -f Containerfile.centos10 \
  -t sdr-controller:test \
  .
```

**What this does:**
1. Downloads CentOS Stream 10 base image
2. Enables EPEL 10 and CRB repos
3. Installs GCC 14, CMake, and all library dependencies via `dnf`
4. Builds SoapySDR 0.8.1 from source (ensures the remote client module is present)
5. Builds SoapyRemote from source
6. Downloads and configures ActiveMQ Artemis 2.36.0 as the AMQP broker
7. Compiles `sdr_controller` and `sdr_client` with `cmake --build`
8. Installs both binaries to `/sdr/install/bin/`
9. Writes the mock `devices.xml` config
10. Sets up the entrypoint and helper Makefile

**Expected build time:** 10–20 minutes (downloads + compiling SoapySDR from source).  
**Expected image size:** ~2.5 GB (Java for Artemis is the main contributor).

The build will print many lines. The last lines you should see are something like:

```
-- Install configuration: "Release"
-- Installing: /sdr/install/bin/sdr_controller
-- Installing: /sdr/install/bin/sdr_client
-- Installing: /sdr/install/etc/sdr-controller/devices.xml
```

If the build fails, see the **Troubleshooting** section at the bottom.

---

## Step 4 — Run the container

```bash
podman run \
  --rm \
  -it \
  --name sdr-test \
  -p 5672:5672 \
  -p 8161:8161 \
  -p 30000-30020:30000-30020/udp \
  sdr-controller:test
```

**Port map explained:**

| Port | Protocol | What |
|------|----------|------|
| 5672 | TCP | AMQP 1.0 — the broker. Your DSP apps or test scripts on the host connect here |
| 8161 | TCP | ActiveMQ Artemis web console — open in browser to watch queues |
| 30000–30020 | UDP | IQ sample streams — the controller sends IQ packets to these ports when tasks are running |

**What you will see when the container starts:**

```
╔══════════════════════════════════════════════════════════════════════╗
║     SDR Radio Resource Task Manager — CentOS Stream 10 Container    ║
╠══════════════════════════════════════════════════════════════════════╣
║  Source:      /sdr                                                   ║
...
╚══════════════════════════════════════════════════════════════════════╝

[entrypoint] Starting ActiveMQ Artemis broker...
[entrypoint] Waiting for broker on :5672...
[entrypoint] Broker ready.

[entrypoint] Broker is up. You can now run the controller.
             Fastest path:  make -f /sdr/Makefile.test all

[root@container sdr]#
```

You now have a shell inside the container. The broker is running in the background.

---

## Step 5 — Run the full smoke test

Inside the container, run:

```bash
make -f /sdr/Makefile.test all
```

This will:
1. Confirm the broker is ready
2. Start `sdr_controller` in the background (logging to `/tmp/sdr_controller.log`)
3. Wait 3 seconds for the controller to connect to the broker
4. Fire 6 test requests using `sdr_client`:
   - Health query
   - Immediate narrowband task
   - Scheduled wideband task
   - Continuous DF task
   - Scan plan task
   - Snapshot / FFT survey

**Expected output for each test** (no real hardware attached):

```
[test] ── Test 1: HEALTH_QUERY ──────────────────────────────
Response: {"msg_type":"HEALTH_QUERY_RESPONSE", "devices": [...], ...}

[test] ── Test 2: IMMEDIATE narrowband task ─────────────────
Response: {"msg_type":"TASK_RESPONSE", "accepted":false,
           "reject_code":"NO_DEVICE_AVAILABLE", ...}
```

`NO_DEVICE_AVAILABLE` is correct — the mock config points at `127.0.0.1:55132` which has no SoapySDR server. Every other layer (AMQP transport, message parsing, scheduler logic, response encoding) worked perfectly if you see a well-formed JSON response.

---

## Step 6 — Individual commands you can run

All of these run **inside the container shell**:

### Start / stop the controller manually

```bash
# Start with debug logging
SDR_LOG_LEVEL=debug sdr_controller /etc/sdr-controller/devices.xml

# Or background it
SDR_LOG_LEVEL=debug sdr_controller /etc/sdr-controller/devices.xml &

# Stop it
kill %1
```

### Send a health query

```bash
sdr_client amqp://localhost:5672 sdr_ctrl sdr_test_pw health
```

### Send an immediate task (frequency, bandwidth, sample_rate, rx_count, tx_count)

```bash
sdr_client amqp://localhost:5672 sdr_ctrl sdr_test_pw \
    immediate 433920000 200000 1000000 1 0
```

### Send a scheduled task (adds start_time = now+10s, end_time = now+70s)

```bash
sdr_client amqp://localhost:5672 sdr_ctrl sdr_test_pw \
    scheduled 915000000 5000000 10000000 2 0 10
```

### Send a continuous task (runs until STOP)

```bash
sdr_client amqp://localhost:5672 sdr_ctrl sdr_test_pw \
    continuous 2400000000 20000000 40000000 1 0
```

### Send a scan plan

```bash
sdr_client amqp://localhost:5672 sdr_ctrl sdr_test_pw scan
```

### Send a snapshot / FFT survey

```bash
sdr_client amqp://localhost:5672 sdr_ctrl sdr_test_pw \
    snapshot 2400000000 40000000 40000000
```

### Watch the controller log in real time

```bash
tail -f /tmp/sdr_controller.log
```

### Use the Makefile targets

```bash
make -f /sdr/Makefile.test help     # show all targets
make -f /sdr/Makefile.test broker   # start broker only
make -f /sdr/Makefile.test run      # start controller
make -f /sdr/Makefile.test test     # fire all test requests
make -f /sdr/Makefile.test stop     # stop controller
make -f /sdr/Makefile.test logs     # tail controller log
```

---

## Step 7 — Recompile after editing source

The full source is at `/sdr/` inside the container. If you edit a `.cpp` or `.hpp` file and want to recompile:

```bash
make -f /sdr/Makefile.test rebuild
```

This runs `cmake --build` (incremental — only recompiles changed files) then reinstalls the binaries.

**To edit files from your host machine without rebuilding the image**, mount your source directory:

```bash
podman run \
  --rm -it \
  --name sdr-dev \
  -v /path/to/your/sdr-v3:/sdr:z \
  -p 5672:5672 \
  -p 8161:8161 \
  sdr-controller:test
```

The `:z` flag is required on SELinux-enabled systems (CentOS 10 has SELinux enforcing by default). Inside the container your edits are live; run `make -f /sdr/Makefile.test rebuild` to pick them up.

---

## Step 8 — ActiveMQ Artemis web console

While the container is running, open a browser on your CentOS machine and go to:

```
http://localhost:8161
```

Login: `sdr_ctrl` / `sdr_test_pw`

You can see:
- Queue depths for `sdr.task.request` and `sdr.task.response`
- Topic subscribers for `sdr.status` and `sdr.health`
- Live message browsing — watch requests come in and responses go out

---

## Step 9 — Connect real hardware

When you have your PlutoSDR boards accessible on the network, edit the config:

```bash
# From inside the container
vi /etc/sdr-controller/devices.xml
```

Change the `<uri>` values to your actual SoapyRemote addresses:

```xml
<device id="pluto-0">
    <driver>remote</driver>
    <uri>soapy://192.168.1.100:55132</uri>   <!-- your board IP -->
    ...
</device>
```

Then restart the controller:

```bash
make -f /sdr/Makefile.test stop
make -f /sdr/Makefile.test run
```

The controller will now open the real devices. Tasks will be `ACCEPTED` and IQ streams will flow on the UDP ports.

---

## Stopping and cleaning up

Stop the container (Ctrl+C or `exit` inside the shell):

```bash
exit
```

Since you ran with `--rm`, the container is automatically deleted when it exits. The image remains and can be re-run at any time.

To delete the image:

```bash
podman rmi sdr-controller:test
```

---

## Troubleshooting

### Build fails: `dnf: No such package spdlog-devel`

EPEL 10 might not be fully mirrored yet on your machine. Try:

```bash
podman build --no-cache -f Containerfile.centos10 -t sdr-controller:test .
```

Or temporarily set the DNF mirror:

```bash
podman build \
  --build-arg DNF_OPTS="--setopt=fastestmirror=True" \
  -f Containerfile.centos10 -t sdr-controller:test .
```

### Build fails: `CMake Error: Could not find SoapySDR`

SoapySDR was built from source and installed to `/usr/local`. The CMake call already passes `-DCMAKE_PREFIX_PATH=/usr/local`. If it still fails, the SoapySDR cmake files ended up in a different path. Get a shell in the partially-built image and check:

```bash
find /usr/local -name "SoapySDRConfig.cmake" 2>/dev/null
```

Then update the `-DSoapySDR_DIR=` line in the Containerfile to match.

### Build fails: `fmt` linking error

On some EPEL configurations, `spdlog` is header-only and needs `fmt` linked separately. The Containerfile patches `CMakeLists.txt` to add `fmt` to `target_link_libraries`. If you see a different fmt-related error, check:

```bash
pkg-config --libs spdlog fmt
```

### Container starts but broker never becomes ready

ActiveMQ Artemis needs Java. Verify Java installed:

```bash
java -version
```

If missing, the `java-21-openjdk-headless` package failed to install. Rebuild with `--no-cache`.

### Permission denied on UDP ports 30000–30020

On RHEL/CentOS, ports below 1024 are privileged, but 30000+ should be fine. If you see permission errors:

```bash
# Check if another process has these ports
ss -ulnp | grep 30000
```

### SELinux blocking the bind mount

If you mount your source with `-v` and get permission errors, ensure you used the `:z` flag:

```bash
-v /path/to/sdr-v3:/sdr:z
```

The `:z` tells Podman to relabel the directory for the container's SELinux context.

### `sdr_client` exits immediately without output

The client connects to the broker synchronously. If the broker isn't running, it exits. Make sure the broker is up first:

```bash
make -f /sdr/Makefile.test broker
nc -z localhost 5672 && echo "broker OK"
```
