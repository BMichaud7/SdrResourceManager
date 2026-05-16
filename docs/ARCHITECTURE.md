# Architecture Reference
## SDR Radio Resource Task Manager — Internal Design

---

## 1. Component Map

```
main.cpp
  └─ Controller                 orchestrator; owns the AMQP thread + 3 background threads
       ├─ AmqpClient            qpid-proton AMQP 1.0; auto-reconnect; async send queue
       ├─ MessageCodec          stateless JSON encode/decode for every msg_type
       └─ ResourceManager       scheduling brain; owns all hardware state
            ├─ SpectrumTimeline (one per device)
            │     2D time×frequency reservation; canFit() is read-only (no side effects)
            ├─ RadioDevice      (one per device)
            │     thin SoapySDR wrapper: open/tune/gain/stream lifecycle
            ├─ UdpPortPool      allocates UDP ports from the configured [start, end] range
            ├─ FftEngine        FFTW3-based snapshot: collect samples → power bins
            └─ per active task:
                 ├─ IQStreamer  one per channel; reads SoapySDR, sends UDP CF32 packets
                 ├─ ScanExecutor drives dwell-plan retune; signals IQStreamer to update CF
                 └─ TriggerMonitor monitors RMS power; gates IQStreamer on threshold
```

---

## 2. Threading Model

| Thread | Period | Responsibility |
|--------|--------|---------------|
| AMQP thread (qpid-proton) | event-driven | Receive/send AMQP messages; calls `Controller::onMessage()` |
| `schedulerLoop` | 200 ms | Start SCHEDULED tasks whose `start_time ≤ now` |
| `watchdogLoop` | 1000 ms | Expire tasks whose `stop_time ≤ now`; call `stopTask()` |
| `heartbeatLoop` | configurable | Publish `DEVICE_HEALTH` + `CONTROLLER_HEALTH` to `sdr.health` |
| IQStreamer thread (per channel) | continuous | `readStream()` → `sendmsg()` UDP; updates metrics under mutex |
| ScanExecutor thread (per scan task) | event-driven | Dwell timer; calls `RadioDevice::retune()` + `IQStreamer::updateCenterFreq()` |
| TriggerMonitor thread (per triggered task) | continuous | `readStream()` → RMS check → gate IQStreamer |

All public `ResourceManager` methods take `std::lock_guard<std::mutex> lock(mu_)` at entry.
The single registry mutex (`reg_mu_`) protects the task registry separately from the
scheduling mutex (`mu_`) to avoid holding the big lock during SoapySDR I/O.

**Rank preemption locking**: `tryPreemptConflicting()` acquires `reg_mu_` briefly to
snapshot the task IDs to preempt, then releases it before calling `deactivateTask()`
(which acquires `reg_mu_` internally). This avoids deadlock while keeping the
check-then-act window safe — only one AMQP thread runs `tryAccept()` at a time.

---

## 3. Request Lifecycle

```
AMQP message arrives
        │
        ▼
MessageCodec::decode()          parse JSON → TaskRequest
        │
        ▼
Controller::onMessage()         dispatch on msg_type
        │
        ├─ TASK_STOP / TASK_CANCEL  → ResourceManager::stopTask() / cancelTask()
        ├─ HEALTH_QUERY             → build HEALTH_QUERY_RESPONSE, send on response queue
        ├─ DEVICE_TEMP_QUERY        → read all device temperature sensors; DEVICE_TEMP_RESPONSE
        └─ TASK_REQUEST_*
                │
                ▼
        ResourceManager::tryAccept()           holds mu_ for entire check+commit
                │
                ├─ rf.coherency_group set? ──► coherent path (§4.2)
                │
                └─ single-device path
                        │
                        ├─ findBestDevice()        capacity + freq/bw/sr range check
                        │    └─ canFit() honours preferred_channel:
                        │         • -1 (any)  → pick next free channel (existing behaviour)
                        │         • ≥0        → assign that specific channel; shared-LO
                        │                       devices allow reuse of an occupied channel
                        │                       (activateTask borrows the live IQStreamer)
                        ├─ (if no fit AND req.rank > 0) tryPreemptConflicting()
                        │    └─ for each device: slotsOverlapping() → check all rank < req.rank
                        │       → deactivateTask(CANCELLED, "PREEMPTED_BY_HIGHER_RANK …")
                        │    └─ retry findBestDevice() after preemption
                        ├─ tryRetuneCombined()     expand shared-LO window to cover new task
                        │    ├─ preferred_channel=-1, different CF → widen + DDC (§5)
                        │    ├─ preferred_channel≥0, same CF      → same-CF subscribe fast-path:
                        │    │     verify slice fits existing window; return existing CF/SR;
                        │    │     no hardware retune; activateTask borrows IQStreamer
                        │    └─ preferred_channel≥0, different CF → widen + DDC, filtered
                        │                                            to preferred channel
                        ├─ UdpPortPool::allocateN()     port allocation
                        ├─ SpectrumTimeline::insert()   commit reservation
                        └─ scheduleTask() or activateTask()
                                │
                                ▼
                        TaskResponse → AMQP response queue
                        TASK_STATUS (SCHEDULED or RUNNING) → sdr.status topic
```

---

## 4. Coherence Paths

### 4.1 Per-device coherence (`shared_lo` in devices.xml)

Controlled by `SpectrumTimeline::canFit(..., shared_lo)`.

- `shared_lo=true`: the timeline tracks one LO per device. All concurrent tasks must
  agree on `center_freq_hz` and `sample_rate_sps`. If a new task requests a different
  CF while the device is active, `canFit()` returns `RETUNE_CONFLICT`.
  Tasks share the RF window; the timeline carves spectrum slices within it.

- `shared_lo=false`: no LO constraint. Each channel tracks only channel occupancy.
  Multiple tasks at different frequencies can run simultaneously.

### 4.2 Cross-device coherent path (`coherency_group` in request)

Triggered when `rf.coherency_group` is non-empty.

```
1. Collect all online devices whose config.coherency_group matches
2. Distribute rx_count greedily across them (fill each to its cap)
3. For each device: SpectrumTimeline::canFit() — ALL must pass or reject atomically
4. UdpPortPool::allocateN(total_rx_count)
5. For each device: SpectrumTimeline::insert(), RadioDevice::tune(), IQStreamer::start()
6. Single TaskRecord with multiple DeviceAllocations
```

This is the mechanism for phase-coherent 4-channel DF across two AD9361 boards.

---

## 5. Digital Channelization

When two tasks request the same device at different center frequencies, the
scheduler can widen the hardware window to cover both bands simultaneously and
apply a **Digital Down-Converter (DDC)** per sub-band task. This avoids opening
a second SoapySDR stream and keeps hardware utilization at one stream per device.

### Activation path

```
Task A accepted: cf=100 MHz, sr=50 kHz  →  device tuned to 100 MHz, IQStreamer starts
                                             Task A is a raw addDest consumer
Task B accepted: cf=101 MHz, sr=50 kHz  →  tryRetuneCombined() widens device to
                                             combined_cf=100.5 MHz, combined_sr=2 MHz
                                          →  All running IQStreamers get pauseForRetune +
                                             updateCenterFreq + updateSampleRate
                                          →  [NEW] Existing raw fans upgraded in-place:
                                               Task A → removeDest() + addSubBand()
                                               is_subband_consumer flag set in TaskRuntime
                                          →  Task B's activation: borrowed path detected,
                                             output_sample_rate_sps=50kHz set in alloc
                                          →  IQStreamer::addSubBand() called with DDC params
```

**Without the upgrade step**, Task A would start receiving wideband (2 MHz) packets the
moment the hardware is widened — exposing spectrum it never requested. With the upgrade,
Task A continues to see exactly its original 50 kHz band; only the packet source changes
from raw fan-out to DDC sub-band.

### In-flight upgrade (tryRetuneCombined)

The upgrade runs in four lock-safe steps, all inside `tryRetuneCombined`, immediately
after the hardware retune and before the registry `sample_rate_sps` update:

1. **Under `rt_mu_`** — collect raw fan-out tasks on the shared IQStreamer
2. **Under `reg_mu_`** — read each task's `slice_lo/hi_hz` (→ task CF) and original
   `sample_rate_sps` (before it is overwritten with `combined_sr`); check integer decimation
3. **No lock** — `removeDest(task_id)` + `addSubBand(task_id, …)` on the IQStreamer
4. **Under `rt_mu_`** — set `is_subband_consumer = true` in each upgraded `TaskRuntime`

Tasks where `wideband_sr / task_sr` is not within 0.1% of an integer are left as raw
fan-out (unchanged behaviour — they will receive the wideband stream).

### DDC (class `Ddc`)

Located in `include/Ddc.hpp` / `src/Ddc.cpp`.

Per sub-band task:
1. **Mix**: multiply each input CF32 sample by `exp(-j·2π·offset·t)` where `offset = task_cf - wideband_cf`
2. **Filter**: real FIR low-pass (windowed sinc, Hamming window, 64 taps), cutoff = `0.45/decim`
3. **Decimate**: output one sample every `decim = round(wideband_sr / task_sr)` input samples

State is per-instance (phase accumulator, FIR ring buffer), so each sub-band task has its own `Ddc` object.

### Optional channel selection (`preferred_channel`)

`RfRequest.preferred_channel` (default -1 = any) lets a task pin to a specific physical
hardware channel (antenna port). Four outcomes:

| Situation | Result |
|-----------|--------|
| Channel is free | `canFit` assigns it directly; new IQStreamer opened |
| Channel occupied, **same CF/SR** (shared-LO) | `tryRetuneCombined` same-CF fast-path: verifies the slice fits in the existing window, returns existing CF/SR, **no hardware retune**; `activateTask` borrows the live IQStreamer (raw fan-out or DDC) |
| Channel occupied, **different CF** (shared-LO) | `tryRetuneCombined` widens the window and sets up DDC on that channel |
| Channel occupied, independent-LO device | Rejected (`CHANNEL_COUNT_EXCEEDED`) |

When `preferred_channel = -1` (the default) the scheduler picks the next available channel
and falls back to `tryRetuneCombined` for different-CF collisions — identical to the
pre-existing behaviour.

**Wire format** — set `preferred_channel` in the `rf` block of any task request:

```json
{
  "msg_type": "TASK_REQUEST_CONTINUOUS",
  "request_id": "req-1",
  "task_type": "NARROWBAND",
  "rf": {
    "center_freq_hz": 101000000,
    "bandwidth_hz":   50000,
    "sample_rate_sps": 50000,
    "rx_count": 1,
    "preferred_channel": 0
  },
  "streaming": { "dest_ip": "10.0.0.20", "dest_ports": [50001] }
}
```

Omit `preferred_channel` or set it to `-1` for the default any-channel behaviour.

### Constraints

- Decimation ratio must be a **positive integer**. If `wideband_sr / task_sr` is not within
  0.1% of an integer, the task falls back to raw fan-out (no DDC).
- DDC sub-bands only apply to the **primary hardware channel** (index 0 of the shared
  IQStreamer's `soapy_bufs`). Requesting multiple RX channels via a DDC sub-band is not
  supported; such tasks fall back to raw fan-out.
- Only `shared_lo=true` devices participate in combined-window retune + DDC.
- `preferred_channel` only constrains the first RX channel (`rx_count=1`); ignored for
  multi-channel requests.

### Packet headers for sub-band streams

Sub-band packets use the **task's own** center frequency and decimated sample rate in the
header — not the wideband values. The consumer doesn't need to know the wideband capture
is happening.

| Field | Value |
|-------|-------|
| `center_freq_hz` | task's requested center frequency |
| `sample_rate` | task's requested (decimated) sample rate |
| `num_samples` | `input_samples / decim` (varies per packet) |
| `channel_index` | as requested by the task |

### Lifecycle

```
IQStreamer::addSubBand(task_id, …, cf, output_sr, wideband_sr)
    → opens UDP socket, creates Ddc, allocates out_buf
IQStreamer::removeSubBand(task_id)
    → closes socket, removes sub-band state
    → returns totalConsumers() = destCount() + subBandCount()

deactivateTask:
    if rt.is_subband_consumer → streamer->removeSubBand(task_id)
    else                      → streamer->removeDest(task_id)
    if totalConsumers() == 0  → streamer->stop(), hardware stream closed
```

---

## 6. Device Temperature Query

The `DEVICE_TEMP_QUERY` / `DEVICE_TEMP_RESPONSE` message pair provides a lightweight way to
read hardware temperatures without pulling the full `HEALTH_QUERY` payload.

### SoapySDR sensor support

SoapySDR exposes sensors via `Device::listSensors()` + `Device::readSensor(name)`.
`RadioDevice::listTemperatures()` filters to names containing `"temp"`, so it automatically
picks up every temperature sensor the driver exposes:

| Board | Sensors |
|-------|---------|
| ADALM-PLUTO (AD9361) | `xadc_temp0` (Zynq FPGA), `ad9361-phy_temp0` (RF chip) |
| Generic / other | any sensor name matching `"temp"` from `listSensors()` |

If `listSensors()` throws (driver doesn't implement it), the code falls back to reading
`"temp0"` directly — the de-facto standard name used by most SoapySDR drivers.

### Wire format

**Request** — send to the task request queue:
```json
{
  "msg_type":   "DEVICE_TEMP_QUERY",
  "request_id": "req-temp-1"
}
```

**Response** — received on the response queue:
```json
{
  "msg_type":      "DEVICE_TEMP_RESPONSE",
  "schema_version": "2.0",
  "request_id":    "req-temp-1",
  "timestamp_ms":  1747408590000,
  "devices": [
    {
      "device_id": "pluto-0",
      "online":    true,
      "sensors": [
        { "name": "xadc_temp0",      "value_c": 47.86 },
        { "name": "ad9361-phy_temp0","value_c": 1.75  }
      ]
    },
    {
      "device_id": "pluto-1",
      "online":    false,
      "sensors":   []
    }
  ]
}
```

`value_c` is `null` (JSON null) when the read fails (sensor present but unreadable).
Offline devices always have an empty `sensors` array.

### Implementation

```
Controller::onMessage("DEVICE_TEMP_QUERY")
    → handleTempQuery()
        for each device_id in rm_->deviceIds():
            dev = rm_->getDevice(id)
            dev->listTemperatures()     ← listSensors() + readSensor() per sensor
        → MessageCodec::encodeTempResponse()
    → amqp_->sendResponse()
```

No task is created; the query is synchronous on the AMQP event thread. The response
is sent to the same reply queue as task responses.

---

## 7. IQStreamer Internals


Each active RX channel gets one `IQStreamer`. It runs a background thread that:

```cpp
while (running_) {
    ret = dev_->readStream(stream_, bufs, N, flags, hw_ts, 500ms_timeout)

    if (ret == TIMEOUT)  continue;           // doesn't count as error
    if (ret == OVERFLOW) pkt_flags |= IQ_FLAG_OVERFLOW; errs++;
    if (ret < 0)         errs++; if (errs >= 20) → on_error_ callback; break;
    else                 errs = 0;

    if (first)           pkt_flags |= IQ_FLAG_FIRST_PACKET;
    if (dwell_changed_)  pkt_flags |= IQ_FLAG_DWELL_CHANGE;

    sendmsg(udp_fd_, header + cf32_samples, MSG_DONTWAIT);
    update metrics (packets_sent, samples_total, rssi_dbfs)
}
```

Key behaviors:
- `on_error_` fires after 20 consecutive non-timeout errors → `ResourceManager` terminates the task
- `updateCenterFreq(cf)` is called by `ScanExecutor` on each retune; sets `dwell_changed_` atomically
- UDP socket is opened with `SO_SNDBUF=8MB` and `MSG_DONTWAIT` — drops silently if the network is full
- `stop()` is idempotent: safe to call multiple times

---

## 8. ScanExecutor Internals

```cpp
for each entry in params.entries (repeat=true → loop forever):
    step_.store(i)
    retune_(entry.center_freq_hz, entry.sample_rate_sps)    // calls RadioDevice::retune()
    for each IQStreamer: updateCenterFreq(entry.center_freq_hz)
    sleep until dwell_ms elapsed (in 50ms increments, checking running_)

→ done_(task_id, ok=true)
```

The 50ms poll granularity means minimum effective dwell ≈ 50ms regardless of `dwell_ms`
config. Production scan plans should use `dwell_ms ≥ 100` for reliable timing.

---

## 9. TriggerMonitor Internals

```cpp
while (running_):
    readStream(BLK=256 samples, timeout=200ms)
    if TIMEOUT or error: continue

    rms = 10*log10(sum(i^2+q^2)/N)

    if not triggered:
        ring.push(block)            // pre-trigger ring buffer
        if rms >= threshold:
            triggered = true
            post_rem = (post_trigger_ms/1000) * rate
            streamer.start()

    else:  // post-trigger capture
        post_rem -= n_samples
        if post_rem <= 0:
            triggered = false
            streamer.stop()
            captures++
            if max_captures > 0 and captures >= max_captures: break

→ done_(task_id, ok=true)
```

The pre-trigger ring buffer retains blocks but the streamer does not start until
threshold is crossed — pre-trigger IQ is not currently streamed (reserved for future).

---

## 10. FftEngine Internals

`FftEngine::compute()` is called synchronously by `ResourceManager::doAcceptSnapshot()`.
It collects `fft_size × n_averages` CF32 samples, applies a Hann window to each frame,
runs FFTW3 forward DFT, averages power in linear scale, converts to dBFS, and applies
`fftshift` (DC moved to center of output array).

The FFTW3 plan is lazily allocated and reused for the same `fft_size`. If a second
snapshot requests a different FFT size, the old plan is destroyed and a new one created.
`FftEngine` is not thread-safe — it must only be called from one thread (the AMQP
event loop, under the ResourceManager lock).

---

## 11. Key Design Decisions

### Single-instance hardware ownership
SoapySDR Remote does not support concurrent clients. The Kubernetes deployment uses
`strategy: Recreate` — the old pod fully terminates before the new one starts.
`terminationGracePeriodSeconds: 30` allows active IQ streams to drain.

### Lock hierarchy
```
mu_ (ResourceManager)          — protects device/timeline state; held during tryAccept
  └─ reg_mu_ (task registry)   — protects registry_ map; held separately to avoid
                                  holding mu_ during SoapySDR I/O inside activateTask()
IQStreamer::mu_                — protects metrics_; per-streamer
```

Never hold both `mu_` and a SoapySDR call simultaneously.

### UDP delivery semantics
IQ packets are sent with `MSG_DONTWAIT`. The controller never blocks waiting for the
network. If the DSP pod is slow or the network is congested, packets are dropped
silently; the DSP pod detects gaps via sequence number discontinuities.

### Jumbo frames by default
Default `iq_packet_samples=1024` → 8224-byte packets. This dramatically reduces
packet-per-second rate at high sample rates (e.g., 61.44 MSPS → 60k pps vs 750k pps
for 150-byte payloads), reducing interrupt load on both ends. Requires jumbo frame
support on all switches in the path. Set `iq_packet_samples=183` for standard MTU.

### No partial coherent allocation
If any board in a `coherency_group` fails `canFit()`, the entire coherent request is
rejected. There is no fallback to fewer boards — partial coherence produces incorrect
DF results.

---

## 12. Test Architecture

Tests live in `tests/` and compile into a single `sdr_tests` binary (205 tests total).
All tests use GoogleTest and run without hardware via `FakeSoapyDevice`.

### FakeSoapyDevice

Registered as SoapySDR driver `"fake"` via `SoapySDR::Registry`. Tests control its
behavior through `FakeSoapy::` atomics:

| Atomic | Effect |
|--------|--------|
| `fail_open` | `Device::make()` throws |
| `fail_read` | `readStream()` → `SOAPY_SDR_NOT_SUPPORTED` every call |
| `overflow_next` | `readStream()` → `SOAPY_SDR_OVERFLOW` once |
| `timeout_count` | `readStream()` → `SOAPY_SDR_TIMEOUT` for N calls |
| `samples_per_read` | number of samples returned per `readStream()` call |
| `sample_value` | constant I/Q fill value (used to set RMS for TriggerMonitor) |
| `reported_temp` | value returned by `readSensor("temp0")` |
| `read_delay_us` | µs sleep inside `readStream()` to throttle IQStreamer threads |

Call `FakeSoapy::reset()` at the start of each test to restore defaults.

### Test files

| File | Suite(s) | Tests | Notes |
|------|----------|-------|-------|
| `test_spectrum_timeline.cpp` | SpectrumTimeline | 34 | Pure logic, no threading; includes preferred_channel variants |
| `test_message_codec.cpp` | MessageCodec | 25 | Stateless encode/decode; includes DEVICE_TEMP_QUERY/RESPONSE |
| `test_config_parser.cpp` | ConfigParser | 12 | File I/O, uses `/tmp` |
| `test_udp_port_pool.cpp` | UdpPortPool | 10 | Pure logic |
| `test_resource_manager.cpp` | ResourceManager, RtlSdr, HackRf, LimeSdr, UsrpB210, PlutoSdr | 75 | Uses FakeSoapy; real IQStreamer threads; preferred_channel, CombinedWindow, device-profile, temperature tests |
| `test_iq_streamer.cpp` | IQStreamer | 10 | Binds real UDP sockets on localhost |
| `test_ddc.cpp` | Ddc | 7 | DDC mixer, FIR, decimation unit tests — no threading |
| `test_channelization.cpp` | Channelization | 7 | Sub-band fan-out; in-flight dest→DDC upgrade; real UDP sockets |
| `test_fft_engine.cpp` | FftEngine | 11 | Calls real FFTW3 |
| `test_scan_executor.cpp` | ScanExecutor | 10 | Real threads; `waitFor(done_callback)` pattern |
| `test_trigger_monitor.cpp` | TriggerMonitor | 6 | Real threads; FakeSoapy sample_value controls trigger |

### Threading note for tests

`ScanExecutor` and `TriggerMonitor` set `running_=true` in `start()` and only clear it
via `stop()`. The loop's natural completion (non-repeat mode, max_captures reached) does
not clear `running_`. Tests must wait on the `done` callback or a completion counter,
not `isRunning()`, to detect natural termination.

---

## 13. Adding a New Message Type

1. Add the `msg_type` string to `MessageCodec::decode()` dispatch
2. Add any new parameter struct to `Types.hpp` and the `TaskRequest` optional field
3. Add a `doAcceptXxx()` method to `ResourceManager`; route from `tryAccept()`
4. Add encode function to `MessageCodec` if a new response type is needed
5. Update `docs/ICD.md` with the new message definition and scenarios
6. Add unit tests in `tests/unit/test_message_codec.cpp` (decode/encode) and
   a new `test_xxx.cpp` for the subsystem logic
