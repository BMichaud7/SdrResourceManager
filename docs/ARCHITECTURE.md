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
        ├─ TASK_STOP / TASK_CANCEL → ResourceManager::stopTask() / cancelTask()
        ├─ HEALTH_QUERY            → build HEALTH_QUERY_RESPONSE, send on response queue
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
                        ├─ SpectrumTimeline::canFit()   2D time×freq reservation check
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

## 5. IQStreamer Internals

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

## 6. ScanExecutor Internals

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

## 7. TriggerMonitor Internals

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

## 8. FftEngine Internals

`FftEngine::compute()` is called synchronously by `ResourceManager::doAcceptSnapshot()`.
It collects `fft_size × n_averages` CF32 samples, applies a Hann window to each frame,
runs FFTW3 forward DFT, averages power in linear scale, converts to dBFS, and applies
`fftshift` (DC moved to center of output array).

The FFTW3 plan is lazily allocated and reused for the same `fft_size`. If a second
snapshot requests a different FFT size, the old plan is destroyed and a new one created.
`FftEngine` is not thread-safe — it must only be called from one thread (the AMQP
event loop, under the ResourceManager lock).

---

## 9. Key Design Decisions

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

## 10. Test Architecture

Tests live in `tests/` and compile into a single `sdr_tests` binary. All tests use
GoogleTest and run without hardware via `FakeSoapyDevice`.

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

| File | Tests | Notes |
|------|-------|-------|
| `test_spectrum_timeline.cpp` | 14 | Pure logic, no threading |
| `test_message_codec.cpp` | 12 | Stateless encode/decode |
| `test_config_parser.cpp` | 6 | File I/O, uses `/tmp` |
| `test_udp_port_pool.cpp` | 6 | Pure logic |
| `test_resource_manager.cpp` | 55 | Uses FakeSoapy; starts/stops real IQStreamer threads |
| `test_iq_streamer.cpp` | 8 | Binds real UDP sockets on localhost |
| `test_fft_engine.cpp` | 7 | Calls real FFTW3 |
| `test_scan_executor.cpp` | 7 | Real threads; `waitFor(done_callback)` pattern |
| `test_trigger_monitor.cpp` | 6 | Real threads; FakeSoapy sample_value controls trigger |

### Threading note for tests

`ScanExecutor` and `TriggerMonitor` set `running_=true` in `start()` and only clear it
via `stop()`. The loop's natural completion (non-repeat mode, max_captures reached) does
not clear `running_`. Tests must wait on the `done` callback or a completion counter,
not `isRunning()`, to detect natural termination.

---

## 11. Adding a New Message Type

1. Add the `msg_type` string to `MessageCodec::decode()` dispatch
2. Add any new parameter struct to `Types.hpp` and the `TaskRequest` optional field
3. Add a `doAcceptXxx()` method to `ResourceManager`; route from `tryAccept()`
4. Add encode function to `MessageCodec` if a new response type is needed
5. Update `docs/ICD.md` with the new message definition and scenarios
6. Add unit tests in `tests/unit/test_message_codec.cpp` (decode/encode) and
   a new `test_xxx.cpp` for the subsystem logic
