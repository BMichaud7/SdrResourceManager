# INTERFACE CONTROL DOCUMENT (ICD)
## SDR Radio Resource Task Manager
### Document: SDR-RRTM-ICD-002  |  Version: 2.2  |  Status: Released

---

## 1. PURPOSE AND SCOPE

This ICD defines all AMQP 1.0 message interfaces between:
- **Producers**: DSP Kubernetes pods requesting RF resources
- **Consumer / Controller**: `sdr-controller` Radio Resource Task Manager
- **Transport**: AMQP 1.0 over Apache ActiveMQ Artemis

---

## 2. HARDWARE CONTEXT (informative)

The controller supports any SoapySDR-compatible hardware. Device capabilities are
declared per-board in `devices.xml`. Two coherence modes are supported:

**`shared_lo=true`** (e.g. AD9361, LimeSDR MIMO): all RX channels on a board share
one LO and one RF window. All tasks on the same device during overlapping windows
MUST share `center_frequency` and `sample_rate`.

**`shared_lo=false`** (e.g. RTL-SDR, HackRF, USRP B210): each channel tunes
independently; concurrent tasks may use different center frequencies on the same board.

**Cross-board coherence**: boards that share an external reference clock are assigned
the same `coherency_group` in `devices.xml`. A task can request channels coherently
across all boards in a group by setting `coherency_group` in the `rf` block — the
controller allocates the requested `rx_count` channels distributed across every online
board in that group. All boards in the group must be conflict-free or the entire
request is rejected.

---

## 3. TRANSPORT LAYER

| Parameter        | Value                                                           |
|------------------|-----------------------------------------------------------------|
| Protocol         | AMQP 1.0 (ISO/IEC 19464:2014)                                  |
| Broker           | Apache ActiveMQ Artemis ≥ 2.36                                 |
| Default URL      | `amqp://activemq-service.sdr-system.svc.cluster.local:5672`   |
| Authentication   | SASL PLAIN                                                      |
| Message encoding | UTF-8 JSON string in AMQP 1.0 message body                    |
| Content-Type     | `application/json`                                              |

### 3.1 Address Definitions

| Address              | Type  | Direction           | Purpose                        |
|----------------------|-------|---------------------|--------------------------------|
| `sdr.task.request`   | Queue | Client → Controller | All task and query requests    |
| `sdr.task.response`  | Queue | Controller → Client | Accept/reject per request      |
| `sdr.status`         | Topic | Controller → All    | State-change events per task   |
| `sdr.health`         | Topic | Controller → All    | Device and controller heartbeats |

### 3.2 Correlation

- Every request carries `request_id` (UUID v4, client-generated)
- Every response echoes `request_id` and `correlation_id`
- Upon acceptance, controller assigns `task_id` (UUID v4, stable for lifetime)

---

## 4. COMMON ENVELOPE

Every message in both directions MUST include:

```json
{
  "msg_type":       "string",
  "schema_version": "2.0",
  "timestamp_ms":   1700000000000,
  "request_id":     "uuid-v4",
  "correlation_id": "client-opaque-optional"
}
```

### 4.1 Required `rank` field for task requests

All task-creation messages (`TASK_REQUEST_*`) **MUST** include a top-level `rank` field.
Requests that omit it are rejected by the decoder — no `TASK_RESPONSE` is sent and an
AMQP warn-level log entry is generated.

```json
{
  "msg_type":   "TASK_REQUEST_CONTINUOUS",
  "rank":       1,
  ...
}
```

| Value | Semantics |
|-------|-----------|
| `0` | Lowest tier — never preempts anything; can be displaced by any rank > 0 task |
| `1`–`N` | Higher numbers take precedence over lower numbers on the same device |

`TASK_STOP`, `TASK_CANCEL`, and `HEALTH_QUERY` do **not** carry `rank`.

### 4.2 Rank-based preemption

When a new task cannot fit on any available device because lower-rank tasks occupy the
required spectrum or channels, the controller:

1. Identifies all blocking tasks whose `rank < new_task.rank`
2. Cancels each one with `terminal_reason = "PREEMPTED_BY_HIGHER_RANK rank=N request=<id>"`
3. Re-evaluates device fit — accepts the new task if it now fits

If any blocking task has `rank >= new_task.rank`, the entire device is skipped
(preemption is all-or-nothing per device). If no device can be cleared, the request
is rejected with `reject_code = NO_DEVICE_AVAILABLE`.

Preempted tasks receive a `TASK_STATUS` event (see §8.8) with:

```json
{
  "state":           "CANCELLED",
  "terminal_reason": "PREEMPTED_BY_HIGHER_RANK rank=3 request=550e8400-..."
}
```

---

## 5. MSG_TYPE ENUMERATION

### Client → Controller (`sdr.task.request`)

| `msg_type`                 | Description                              |
|----------------------------|------------------------------------------|
| `TASK_REQUEST_SCHEDULED`   | Fixed-tune with explicit time window     |
| `TASK_REQUEST_IMMEDIATE`   | Fixed-tune starting now                  |
| `TASK_REQUEST_CONTINUOUS`  | Fixed-tune running until TASK_STOP       |
| `TASK_REQUEST_SCAN`        | Frequency dwell-plan, retuning over time |
| `TASK_REQUEST_SNAPSHOT`    | FFT power survey, no IQ streaming        |
| `TASK_REQUEST_TRIGGERED`   | IQ capture gated on signal detection     |
| `TASK_REQUEST_CALIBRATION` | Multi-device coherent calibration tune   |
| `TASK_STOP`                | Stop a CONTINUOUS or SCAN task           |
| `TASK_CANCEL`              | Cancel any non-terminal task             |
| `HEALTH_QUERY`             | Request current resource status          |

### Controller → Client

| `msg_type`              | Destination      | Description                          |
|-------------------------|------------------|--------------------------------------|
| `TASK_RESPONSE`         | response queue   | Accept or reject                     |
| `TASK_STATUS`           | status topic     | State change notification            |
| `SNAPSHOT_RESULT`       | response queue   | FFT power bins                       |
| `DEVICE_HEALTH`         | health topic     | Per-device hardware metrics (periodic) |
| `CONTROLLER_HEALTH`     | health topic     | Process-level metrics (periodic)     |
| `HEALTH_QUERY_RESPONSE` | response queue   | Response to HEALTH_QUERY             |

---

## 6. RF PARAMETERS OBJECT

Used inside task requests under key `"rf"`:

```json
"rf": {
  "center_freq_hz":   915000000.0,
  "bandwidth_hz":     10000000.0,
  "sample_rate_sps":  10000000.0,
  "rx_count":         2,
  "tx_count":         0,
  "rx_gain_db":       [30.0, 30.0],
  "rx_agc":           [false, false],
  "tx_atten_db":      [],
  "preferred_device": "pluto-0",
  "coherency_group":  ""
}
```

| Field              | Type     | Required | Description                                                              |
|--------------------|----------|----------|--------------------------------------------------------------------------|
| `center_freq_hz`   | double   | YES      | Within range of at least one loaded device                               |
| `bandwidth_hz`     | double   | YES      | Within `bandwidth_max` of at least one loaded device                     |
| `sample_rate_sps`  | double   | YES      | Within `sample_rate_max` of at least one loaded device                   |
| `rx_count`         | int      | YES      | Total RX channels requested (may span multiple boards)                   |
| `tx_count`         | int      | YES      | 0–N, default 0                                                           |
| `rx_gain_db`       | double[] | NO       | Length = rx_count. Default 30.0                                          |
| `rx_agc`           | bool[]   | NO       | Length = rx_count. Default false                                         |
| `tx_atten_db`      | double[] | NO       | Length = tx_count. Default 0.0                                           |
| `preferred_device` | string   | NO       | Device id hint for single-device path; advisory (falls back on conflict) |
| `coherency_group`  | string   | NO       | If set, allocates `rx_count` across all boards in this group atomically  |

---

## 7. STREAMING DESTINATION OBJECT

```json
"streaming": {
  "dest_ip":    "10.0.1.10",
  "dest_ports": [5000, 5001]
}
```

One port per requested channel, ordered: RX0…RXn, TX0…TXn.

---

## 8. MESSAGE DEFINITIONS

### 8.1 TASK_REQUEST_SCHEDULED

Reserves a specific time window. The controller sets the task to `SCHEDULED` state
until `start_time_epoch_ms` is reached, then begins streaming.

```json
{
  "msg_type":       "TASK_REQUEST_SCHEDULED",
  "schema_version": "2.0",
  "timestamp_ms":   1700000000000,
  "request_id":     "550e8400-e29b-41d4-a716-446655440000",
  "correlation_id": "df-job-001",
  "task_type":      "DF",
  "priority":       7,
  "rank":           2,
  "schedule": {
    "mode":                "SCHEDULED",
    "start_time_epoch_ms": 1700001000000,
    "end_time_epoch_ms":   1700001060000
  },
  "rf": {
    "center_freq_hz":  915000000.0,
    "bandwidth_hz":    10000000.0,
    "sample_rate_sps": 10000000.0,
    "rx_count":        2,
    "tx_count":        0,
    "rx_gain_db":      [30.0, 30.0]
  },
  "streaming": {
    "dest_ip":    "10.0.1.10",
    "dest_ports": [5000, 5001]
  },
  "task_params": {
    "df": {
      "algorithm":      "MUSIC",
      "num_sources":    1,
      "snapshot_count": 512,
      "angular_res_deg": 0.5
    }
  }
}
```

### 8.2 TASK_REQUEST_IMMEDIATE

Starts now. Same as SCHEDULED with `schedule.mode = "IMMEDIATE"`.
`start_time_epoch_ms` is ignored; duration is inferred from `end_time_epoch_ms`.

```json
{
  "msg_type":       "TASK_REQUEST_IMMEDIATE",
  "schema_version": "2.0",
  "timestamp_ms":   1700000000000,
  "request_id":     "660e8400-e29b-41d4-a716-446655440001",
  "task_type":      "NARROWBAND",
  "priority":       5,
  "rank":           1,
  "schedule": {
    "mode":               "IMMEDIATE",
    "end_time_epoch_ms":  1700000300000
  },
  "rf": {
    "center_freq_hz":  162400000.0,
    "bandwidth_hz":    200000.0,
    "sample_rate_sps": 250000.0,
    "rx_count":        1,
    "tx_count":        0,
    "rx_gain_db":      [40.0]
  },
  "streaming": {
    "dest_ip":    "10.0.1.11",
    "dest_ports": [5100]
  },
  "task_params": {
    "narrowband": {
      "demod":          "FM",
      "squelch_dbfs":   -70.0,
      "output_rate_sps": 48000
    }
  }
}
```

### 8.3 TASK_REQUEST_CONTINUOUS

Runs indefinitely until `TASK_STOP` is received. No `end_time_epoch_ms`.

```json
{
  "msg_type":       "TASK_REQUEST_CONTINUOUS",
  "schema_version": "2.0",
  "timestamp_ms":   1700000000000,
  "request_id":     "770e8400-e29b-41d4-a716-446655440002",
  "task_type":      "WIDEBAND",
  "rank":           0,
  "schedule":       { "mode": "CONTINUOUS" },
  "rf": {
    "center_freq_hz":  2400000000.0,
    "bandwidth_hz":    20000000.0,
    "sample_rate_sps": 20000000.0,
    "rx_count":        1,
    "tx_count":        0,
    "rx_gain_db":      [25.0]
  },
  "streaming": {
    "dest_ip":    "10.0.1.12",
    "dest_ports": [5200]
  },
  "task_params": {
    "wideband": {
      "record_raw_iq":         true,
      "detect_threshold_dbfs": -60.0,
      "fft_size":              2048
    }
  }
}
```

### 8.4 TASK_REQUEST_SCAN

Cycles through a dwell-plan, retuning between entries. IQ streams continuously;
receiver MUST check `IQ_FLAG_DWELL_CHANGE` on each packet to track the current
center frequency. The `scan` block replaces the `rf` block.

```json
{
  "msg_type":       "TASK_REQUEST_SCAN",
  "schema_version": "2.0",
  "timestamp_ms":   1700000000000,
  "request_id":     "880e8400-e29b-41d4-a716-446655440003",
  "task_type":      "WIDEBAND",
  "rank":           0,
  "schedule":       { "mode": "CONTINUOUS" },
  "scan": {
    "repeat":   true,
    "rx_count": 1,
    "tx_count": 0,
    "rx_gain_db": [30.0],
    "entries": [
      { "step": 1, "center_freq_hz": 915000000.0,  "bandwidth_hz": 5000000.0,  "sample_rate_sps": 5000000.0,  "dwell_ms": 1000 },
      { "step": 2, "center_freq_hz": 2400000000.0, "bandwidth_hz": 10000000.0, "sample_rate_sps": 10000000.0, "dwell_ms": 2000 },
      { "step": 3, "center_freq_hz": 433920000.0,  "bandwidth_hz": 2000000.0,  "sample_rate_sps": 2000000.0,  "dwell_ms": 500  }
    ]
  },
  "streaming": {
    "dest_ip":    "10.0.1.13",
    "dest_ports": [5300]
  }
}
```

**Dwell transition**: on each retune the next packet sets `IQ_FLAG_DWELL_CHANGE` (bit 2)
and updates `center_freq_hz` in the packet header. The `sample_rate_sps` header field
also updates if the new entry uses a different rate.

### 8.5 TASK_REQUEST_SNAPSHOT

One-shot FFT power survey. No IQ streaming — result arrives as `SNAPSHOT_RESULT` on the
response queue. The `snapshot` block replaces the `rf` block.

```json
{
  "msg_type":       "TASK_REQUEST_SNAPSHOT",
  "schema_version": "2.0",
  "timestamp_ms":   1700000000000,
  "request_id":     "990e8400-e29b-41d4-a716-446655440004",
  "snapshot": {
    "center_freq_hz":   2400000000.0,
    "bandwidth_hz":     20000000.0,
    "sample_rate_sps":  20000000.0,
    "fft_size":         4096,
    "n_averages":       16,
    "preferred_device": "pluto-0"
  }
}
```

`preferred_device` is advisory: if the named device is busy or offline the controller
picks another eligible device.

### 8.6 TASK_REQUEST_TRIGGERED

Runs a power-threshold monitor. IQ streaming is gated: the streamer starts only when
`rms_dbfs ≥ threshold_dbfs`, captures `post_trigger_ms` of IQ after the event, then
stops. Repeats until `max_captures` is reached (`0` = unlimited, runs until TASK_STOP).

```json
{
  "msg_type":       "TASK_REQUEST_TRIGGERED",
  "schema_version": "2.0",
  "timestamp_ms":   1700000000000,
  "request_id":     "aab00000-e29b-41d4-a716-446655440005",
  "schedule":       { "mode": "CONTINUOUS" },
  "rf": {
    "center_freq_hz":  433920000.0,
    "bandwidth_hz":    2000000.0,
    "sample_rate_sps": 2000000.0,
    "rx_count":        1,
    "rx_gain_db":      [50.0]
  },
  "trigger": {
    "type":            "POWER_THRESHOLD",
    "threshold_dbfs":  -60.0,
    "pre_trigger_ms":  50,
    "post_trigger_ms": 200,
    "max_captures":    5
  },
  "streaming": {
    "dest_ip":    "10.0.1.10",
    "dest_ports": [5400]
  }
}
```

| Trigger field       | Description                                                       |
|---------------------|-------------------------------------------------------------------|
| `type`              | Always `"POWER_THRESHOLD"` in v2.0                               |
| `threshold_dbfs`    | RMS power level that arms the streamer                            |
| `pre_trigger_ms`    | Pre-trigger ring buffer duration (not yet streamed, reserved)    |
| `post_trigger_ms`   | Duration of IQ capture after trigger event                        |
| `max_captures`      | Stop after N captures; `0` = unlimited                           |

### 8.7 TASK_REQUEST_CALIBRATION

Synchronously tunes all boards in a coherency group to the same frequency for a fixed
duration. Useful for phase alignment and noise floor measurement. Always starts
immediately (no schedule block needed). Streams IQ from every device × channel.

```json
{
  "msg_type":       "TASK_REQUEST_CALIBRATION",
  "schema_version": "2.0",
  "timestamp_ms":   1700000000000,
  "request_id":     "bbc00000-e29b-41d4-a716-446655440006",
  "calibration": {
    "center_freq_hz":      1000000000.0,
    "bandwidth_hz":        10000000.0,
    "sample_rate_sps":     10000000.0,
    "duration_ms":         5000,
    "rx_count_per_device": 2,
    "coherency_group":     "refclk-group-0",
    "devices":             []
  },
  "streaming": {
    "dest_ip":    "10.0.1.15",
    "dest_ports": [6000, 6001, 6002, 6003]
  }
}
```

`devices`: if empty, all online boards in `coherency_group` are used.
Ports are ordered board-by-board: `[dev0-RX0, dev0-RX1, dev1-RX0, dev1-RX1]`.

### 8.8 TASK_STOP

Stops a task that is CONTINUOUS or SCAN (still running). The task transitions to
`CANCELLED`. Port pool and spectrum slot are released immediately.

```json
{
  "msg_type":       "TASK_STOP",
  "schema_version": "2.0",
  "timestamp_ms":   1700001200000,
  "request_id":     "ccd00000-e29b-41d4-a716-446655440007",
  "task_id":        "aabbccdd-1234-5678-abcd-000000000001",
  "reason":         "Monitoring complete"
}
```

### 8.9 TASK_CANCEL

Cancels any non-terminal task regardless of state (`SCHEDULED`, `PENDING`, `RUNNING`).
Resources are freed immediately.

```json
{
  "msg_type":       "TASK_CANCEL",
  "schema_version": "2.0",
  "timestamp_ms":   1700001200000,
  "request_id":     "dde00000-e29b-41d4-a716-446655440008",
  "task_id":        "aabbccdd-1234-5678-abcd-000000000001",
  "reason":         "Emergency stop"
}
```

### 8.10 HEALTH_QUERY

Requests an immediate snapshot of all device and controller state.
Response arrives as `HEALTH_QUERY_RESPONSE` on the response queue.

```json
{
  "msg_type":       "HEALTH_QUERY",
  "schema_version": "2.0",
  "timestamp_ms":   1700000000000,
  "request_id":     "eef00000-e29b-41d4-a716-446655440009"
}
```

### 8.11 TASK_RESPONSE — ACCEPTED

```json
{
  "msg_type":              "TASK_RESPONSE",
  "schema_version":        "2.0",
  "timestamp_ms":          1700000000050,
  "request_id":            "550e8400-e29b-41d4-a716-446655440000",
  "correlation_id":        "df-job-001",
  "status":                "ACCEPTED",
  "task_id":               "aabbccdd-1234-5678-abcd-000000000001",
  "schedule_mode":         "SCHEDULED",
  "actual_start_epoch_ms": 1700001000000,
  "actual_stop_epoch_ms":  1700001060000,
  "streams": [
    {
      "stream_id":       "aabbccdd-1234-5678-abcd-000000000001-RX-pluto-0-0",
      "device_id":       "pluto-0",
      "channel_type":    "RX",
      "channel_index":   0,
      "udp_ip":          "10.0.0.10",
      "udp_port":        30000,
      "center_freq_hz":  915000000.0,
      "slice_offset_hz": 0.0,
      "slice_bw_hz":     10000000.0,
      "sample_rate_sps": 10000000.0,
      "format":          "CF32"
    },
    {
      "stream_id":       "aabbccdd-1234-5678-abcd-000000000001-RX-pluto-0-1",
      "device_id":       "pluto-0",
      "channel_type":    "RX",
      "channel_index":   1,
      "udp_ip":          "10.0.0.10",
      "udp_port":        30001,
      "center_freq_hz":  915000000.0,
      "slice_offset_hz": 0.0,
      "slice_bw_hz":     10000000.0,
      "sample_rate_sps": 10000000.0,
      "format":          "CF32"
    }
  ]
}
```

`actual_stop_epoch_ms` is `0` for CONTINUOUS tasks (no end time).
`slice_offset_hz` is non-zero when the task occupies a sub-band within the device's
RF window (e.g., two tasks sharing the same LO).

### 8.12 TASK_RESPONSE — REJECTED

```json
{
  "msg_type":       "TASK_RESPONSE",
  "schema_version": "2.0",
  "timestamp_ms":   1700000000050,
  "request_id":     "550e8400-e29b-41d4-a716-446655440000",
  "status":         "REJECTED",
  "task_id":        "",
  "reject_code":    "RETUNE_CONFLICT",
  "reject_reason":  "Device pluto-0 has active task at 915 MHz; requested 2400 MHz during overlapping window."
}
```

### Reject Codes

| Code                     | Meaning                                                      |
|--------------------------|--------------------------------------------------------------|
| `INVALID_REQUEST`        | Malformed JSON or missing required fields                    |
| `INVALID_TASK_TYPE`      | Unknown `task_type`                                          |
| `INVALID_SCHEDULE`       | `start_time ≥ stop_time`, or window already elapsed          |
| `FREQ_OUT_OF_RANGE`      | `center_freq_hz` outside every loaded device's range        |
| `BW_EXCEEDED`            | `bandwidth_hz` exceeds every loaded device's maximum        |
| `SAMPLE_RATE_EXCEEDED`   | `sample_rate_sps` exceeds every loaded device's maximum     |
| `CHANNEL_COUNT_EXCEEDED` | `rx_count` or `tx_count` exceeds available channels          |
| `SPECTRUM_CONFLICT`      | Requested RF slice overlaps an existing task's slice         |
| `TIME_CONFLICT`          | Time window overlaps an incompatible task                    |
| `RETUNE_CONFLICT`        | Requested CF ≠ active CF on a `shared_lo=true` device        |
| `NO_DEVICE_AVAILABLE`    | No single device satisfies all constraints                   |
| `COHERENCY_UNAVAILABLE`  | Coherent group is not fully available or undersized          |
| `TASK_LIMIT_REACHED`     | `max_concurrent_tasks` in policy is exhausted                |
| `TASK_NOT_FOUND`         | `task_id` unknown to the controller                          |
| `TASK_NOT_STOPPABLE`     | Task type does not support `TASK_STOP`                       |
| `TASK_ALREADY_TERMINAL`  | Task is already `COMPLETED`, `FAILED`, or `CANCELLED`        |
| `PORT_POOL_EXHAUSTED`    | UDP port pool is exhausted                                   |
| `SCAN_ENTRY_INVALID`     | A scan entry has invalid or missing fields                   |
| `DEVICE_OFFLINE`         | A required device is not reachable via SoapySDR              |
| `INTERNAL_ERROR`         | Unexpected controller fault                                  |

### 8.13 TASK_STATUS

Published on topic `sdr.status` whenever a task changes state. Also carries live
stream metrics while the task is RUNNING.

```json
{
  "msg_type":              "TASK_STATUS",
  "schema_version":        "2.0",
  "timestamp_ms":          1700001000050,
  "request_id":            "status-aabbccdd",
  "task_id":               "aabbccdd-1234-5678-abcd-000000000001",
  "state":                 "RUNNING",
  "task_type":             "DF",
  "schedule_mode":         "SCHEDULED",
  "priority":              7,
  "rank":                  2,
  "terminal_reason":       "",
  "device_ids":            ["pluto-0"],
  "actual_start_epoch_ms": 1700001000000,
  "actual_stop_epoch_ms":  1700001060000,
  "streams": [
    {
      "stream_id":     "aabbccdd-1234-5678-abcd-000000000001-RX-pluto-0-0",
      "channel_type":  "RX",
      "channel_index": 0,
      "udp_port":      30000,
      "metrics": {
        "samples_total":    10000000,
        "packets_sent":     9766,
        "overflows":        0,
        "rssi_dbfs":        -45.2,
        "throughput_mbps":  78.6
      }
    },
    {
      "stream_id":     "aabbccdd-1234-5678-abcd-000000000001-RX-pluto-0-1",
      "channel_type":  "RX",
      "channel_index": 1,
      "udp_port":      30001,
      "metrics": {
        "samples_total":    10000000,
        "packets_sent":     9766,
        "overflows":        0,
        "rssi_dbfs":        -44.8,
        "throughput_mbps":  78.5
      }
    }
  ]
}
```

**State values**: `EVALUATING` → `SCHEDULED` → `PENDING` → `RUNNING` → `COMPLETING`
→ `COMPLETED` | `FAILED` | `CANCELLED`

When `state = CANCELLED` due to rank preemption, `terminal_reason` is set to
`"PREEMPTED_BY_HIGHER_RANK rank=N request=<id>"`.  Clients that need to distinguish
preemption from operator cancellation should check this field.

See §4.2 for rank preemption rules and §9 for the full lifecycle state machine.

### 8.14 SNAPSHOT_RESULT

Delivered to the response queue after a `TASK_REQUEST_SNAPSHOT` completes.

```json
{
  "msg_type":           "SNAPSHOT_RESULT",
  "schema_version":     "2.0",
  "timestamp_ms":       1700000001000,
  "request_id":         "990e8400-e29b-41d4-a716-446655440004",
  "status":             "COMPLETED",
  "device_id":          "pluto-0",
  "center_freq_hz":     2400000000.0,
  "bandwidth_hz":       20000000.0,
  "sample_rate_sps":    20000000.0,
  "fft_size":           4096,
  "n_averages":         16,
  "freq_resolution_hz": 4882.8,
  "freq_axis_start_hz": 2390000000.0,
  "freq_axis_step_hz":  4882.8,
  "power_bins":         [-82.3, -81.1, -80.5, "...4096 values total..."],
  "error_msg":          ""
}
```

`power_bins`: float64 array of length `fft_size`, in dBFS, frequency-shifted so
index 0 = `freq_axis_start_hz` (lowest frequency), index `fft_size/2` = `center_freq_hz`.

On failure: `status = "FAILED"`, `error_msg` describes the reason, `power_bins` is empty.

### 8.15 DEVICE_HEALTH

Published on topic `sdr.health` every `heartbeat_interval_ms`. One entry per device.

```json
{
  "msg_type":        "DEVICE_HEALTH",
  "schema_version":  "2.0",
  "timestamp_ms":    1700000030000,
  "request_id":      "heartbeat-00000001",
  "devices": [
    {
      "device_id":          "pluto-0",
      "online":             true,
      "active_tasks":       1,
      "center_freq_hz":     915000000.0,
      "sample_rate_sps":    10000000.0,
      "rx_allocated_bw_hz": 10000000.0,
      "rx_free_bw_hz":      46000000.0,
      "temperature_c":      52.3,
      "driver":             "remote",
      "uri":                "soapy://pluto-sdr-0.sdr-hardware.svc.cluster.local:55132",
      "coherency_group":    "refclk-group-0"
    },
    {
      "device_id":          "pluto-1",
      "online":             true,
      "active_tasks":       0,
      "center_freq_hz":     0.0,
      "sample_rate_sps":    0.0,
      "rx_allocated_bw_hz": 0.0,
      "rx_free_bw_hz":      56000000.0,
      "temperature_c":      49.1,
      "driver":             "remote",
      "uri":                "soapy://pluto-sdr-1.sdr-hardware.svc.cluster.local:55132",
      "coherency_group":    "refclk-group-0"
    }
  ]
}
```

`rx_free_bw_hz` reflects remaining schedulable bandwidth at the current time instant.
`temperature_c` comes from the SoapySDR `temp0` sensor; `0.0` if not supported.

### 8.16 CONTROLLER_HEALTH

Published on topic `sdr.health` every `heartbeat_interval_ms`, alongside DEVICE_HEALTH.

```json
{
  "msg_type":         "CONTROLLER_HEALTH",
  "schema_version":   "2.0",
  "timestamp_ms":     1700000030000,
  "request_id":       "heartbeat-00000001",
  "version":          "2.0.0",
  "uptime_sec":       3600,
  "total_devices":    2,
  "online_devices":   2,
  "scheduled_tasks":  1,
  "pending_tasks":    0,
  "running_tasks":    2,
  "total_tasks":      3,
  "udp_ports_in_use": 3,
  "udp_ports_free":   1997
}
```

### 8.17 HEALTH_QUERY_RESPONSE

Delivered to the response queue in reply to `HEALTH_QUERY`. Combines controller and
device state in a single message.

```json
{
  "msg_type":       "HEALTH_QUERY_RESPONSE",
  "schema_version": "2.0",
  "timestamp_ms":   1700000000100,
  "request_id":     "eef00000-e29b-41d4-a716-446655440009",
  "controller": {
    "msg_type":         "CONTROLLER_HEALTH",
    "version":          "2.0.0",
    "uptime_sec":       3600,
    "total_devices":    2,
    "online_devices":   2,
    "scheduled_tasks":  0,
    "pending_tasks":    0,
    "running_tasks":    1,
    "total_tasks":      1,
    "udp_ports_in_use": 2,
    "udp_ports_free":   1998
  },
  "devices": [
    { "device_id": "pluto-0", "online": true, "active_tasks": 1, "..." : "..." },
    { "device_id": "pluto-1", "online": true, "active_tasks": 0, "..." : "..." }
  ]
}
```

---

## 9. TASK LIFECYCLE

### 9.1 State Machine

```
                     ┌──────────────┐
         request in  │  EVALUATING  │  (synchronous, <1 ms)
         ─────────── └──────┬───────┘
                            │ accepted
               ┌────────────┴────────────┐
               │ start_time in future    │ start_time now / IMMEDIATE / CONTINUOUS
               ▼                         ▼
          ┌───────────┐           ┌─────────┐
          │ SCHEDULED │           │ PENDING │  (tuning hardware)
          └─────┬─────┘           └────┬────┘
                │ scheduler tick       │ tune complete
                └──────────┬───────────┘
                           ▼
                      ┌─────────┐
                      │ RUNNING │◄─ UDP IQ streaming active
                      └────┬────┘
            ┌──────────────┼──────────────┐
            │              │              │
     stop_time      TASK_STOP /     hardware error
     reached        TASK_CANCEL
            │              │              │
            ▼              ▼              ▼
       ┌──────────┐  ┌───────────┐  ┌────────┐
       │COMPLETED │  │ CANCELLED │  │ FAILED │
       └──────────┘  └───────────┘  └────────┘
```

States `COMPLETED`, `CANCELLED`, `FAILED` are terminal. Resources (spectrum slice,
UDP ports) are released upon entering a terminal state.

### 9.2 State Descriptions

| State        | Description                                                          |
|--------------|----------------------------------------------------------------------|
| `EVALUATING` | Controller is running admission checks; not yet stored               |
| `SCHEDULED`  | Accepted; waiting for `start_time_epoch_ms` (scheduler polls 200 ms) |
| `PENDING`    | Start time reached; hardware is being tuned                          |
| `RUNNING`    | IQ streaming active; `TASK_STATUS` published with live metrics       |
| `COMPLETING` | Watchdog triggered stop; cleaning up                                 |
| `COMPLETED`  | Task ended normally at `stop_time`                                   |
| `CANCELLED`  | Stopped by `TASK_STOP` or `TASK_CANCEL`                             |
| `FAILED`     | Hardware error after 20 consecutive `readStream` failures            |

---

## 10. IQ UDP PACKET FORMAT

### 10.1 Header (32 bytes, little-endian)

```
Offset  Size  Type    Field
──────  ────  ──────  ─────────────────────────────────────────────────────
0       4     uint32  magic = 0x49515030 ("IQP0")
4       4     uint32  sequence_number  (monotonically increasing per stream)
8       8     uint64  timestamp_ns     (nanoseconds from task start)
16      8     uint64  center_freq_hz   (updated on each scan dwell change)
24      4     uint32  sample_rate_sps
28      2     uint16  num_samples
30      1     uint8   channel_index    (0 or 1 on a 2-channel device)
31      1     uint8   flags
                        bit 0: IQ_FLAG_OVERFLOW     (hardware FIFO overflow)
                        bit 1: IQ_FLAG_FIRST_PACKET (first packet for this stream)
                        bit 2: IQ_FLAG_DWELL_CHANGE (scan retune; CF updated)
```

### 10.2 Payload

```
[I₀ f32][Q₀ f32][I₁ f32][Q₁ f32] … [Iₙ f32][Qₙ f32]
```

- Sample format: CF32 (IEEE 754 float32 little-endian, interleaved I/Q)
- Default `num_samples`: 1024 per packet
- Default packet size: 32 + 1024×8 = **8224 bytes** (requires jumbo frames)
- MTU-safe (1500-byte Ethernet): set `iq_packet_samples ≤ 183` in `devices.xml`

### 10.3 Flag Behaviour

| Flag                 | When set                                                     | Action required                          |
|----------------------|--------------------------------------------------------------|------------------------------------------|
| `IQ_FLAG_FIRST_PACKET` | First packet after stream start or resume                 | Reset DSP state / buffers                |
| `IQ_FLAG_OVERFLOW`   | Hardware FIFO overflow; `num_samples` may be 0             | Log gap; do not use as clean IQ          |
| `IQ_FLAG_DWELL_CHANGE` | Scan retune just completed; `center_freq_hz` is new CF   | Update frequency context in DSP          |

### 10.4 Gap Detection

A missing `sequence_number` indicates a dropped UDP packet. The receiver must handle
gaps gracefully (e.g., insert zeros, re-sync correlators).

---

## 11. SCENARIOS

Each scenario shows the key JSON fields; UUID and timestamp values are abbreviated.

---

### 11.1 Non-Coherent Single-Channel Continuous

Use case: one narrowband receiver, any free board, runs until stopped.

**Request →**
```json
{
  "msg_type": "TASK_REQUEST_CONTINUOUS",
  "request_id": "req-001",
  "task_type": "NARROWBAND",
  "schedule": { "mode": "CONTINUOUS" },
  "rf": {
    "center_freq_hz": 162400000.0,
    "bandwidth_hz": 200000.0,
    "sample_rate_sps": 250000.0,
    "rx_count": 1
  },
  "streaming": { "dest_ip": "10.0.1.11", "dest_ports": [5100] }
}
```

**Response ← (ACCEPTED)**
```json
{
  "status": "ACCEPTED",
  "task_id": "task-001",
  "schedule_mode": "CONTINUOUS",
  "actual_start_epoch_ms": 1700000000100,
  "actual_stop_epoch_ms": 0,
  "streams": [
    {
      "device_id": "pluto-0",
      "channel_type": "RX", "channel_index": 0,
      "udp_port": 30000,
      "center_freq_hz": 162400000.0,
      "sample_rate_sps": 250000.0
    }
  ]
}
```

**Status → topic** `state: RUNNING` immediately.

**Stop →**
```json
{ "msg_type": "TASK_STOP", "task_id": "task-001", "reason": "done" }
```
**Status → topic** `state: CANCELLED`.

---

### 11.2 Coherent 4-Channel DF Across Both Boards

Use case: all 4 RX channels phase-locked for direction-finding.
Both boards must be in `coherency_group = "refclk-group-0"` in `devices.xml`.

**Request →**
```json
{
  "msg_type": "TASK_REQUEST_SCHEDULED",
  "request_id": "req-002",
  "task_type": "DF",
  "priority": 9,
  "schedule": {
    "mode": "SCHEDULED",
    "start_time_epoch_ms": 1700001000000,
    "end_time_epoch_ms":   1700001060000
  },
  "rf": {
    "center_freq_hz":  915000000.0,
    "bandwidth_hz":    10000000.0,
    "sample_rate_sps": 10000000.0,
    "rx_count":        4,
    "coherency_group": "refclk-group-0"
  },
  "streaming": {
    "dest_ip":    "10.0.1.10",
    "dest_ports": [5000, 5001, 5002, 5003]
  },
  "task_params": {
    "df": { "algorithm": "MUSIC", "num_sources": 1, "snapshot_count": 512 }
  }
}
```

**Response ← (ACCEPTED)** — 4 streams, 2 per board:
```json
{
  "status": "ACCEPTED",
  "task_id": "task-002",
  "schedule_mode": "SCHEDULED",
  "streams": [
    { "device_id": "pluto-0", "channel_index": 0, "udp_port": 30000, "center_freq_hz": 915000000.0 },
    { "device_id": "pluto-0", "channel_index": 1, "udp_port": 30001, "center_freq_hz": 915000000.0 },
    { "device_id": "pluto-1", "channel_index": 0, "udp_port": 30002, "center_freq_hz": 915000000.0 },
    { "device_id": "pluto-1", "channel_index": 1, "udp_port": 30003, "center_freq_hz": 915000000.0 }
  ]
}
```

**Status → topic** `state: SCHEDULED`, then `state: RUNNING` at `start_time`, then
`state: COMPLETED` at `end_time`.

---

### 11.3 Non-Coherent Request — Rejection: One Board Busy at Different CF

Use case: `pluto-0` is running a task at 915 MHz (`shared_lo=true`). A second task
requests 2400 MHz on the same board during the same window.

**Existing task**: `pluto-0`, CF=915 MHz, window [T, T+60s].

**Request →**
```json
{
  "msg_type": "TASK_REQUEST_SCHEDULED",
  "rf": {
    "center_freq_hz":  2400000000.0,
    "bandwidth_hz":    5000000.0,
    "sample_rate_sps": 10000000.0,
    "rx_count":        1,
    "preferred_device": "pluto-0"
  },
  "schedule": { "mode": "SCHEDULED", "start_time_epoch_ms": T, "end_time_epoch_ms": T+60000 }
}
```

**Response ← (REJECTED)** — `pluto-0` blocked; `pluto-1` free, but `preferred_device`
was only a hint so the controller routes to `pluto-1`:
```json
{
  "status": "ACCEPTED",
  "streams": [{ "device_id": "pluto-1", "channel_index": 0, "center_freq_hz": 2400000000.0 }]
}
```

If `pluto-1` were also busy at a conflicting CF:
```json
{ "status": "REJECTED", "reject_code": "NO_DEVICE_AVAILABLE" }
```

---

### 11.4 Two Narrowband Tasks Sharing the Same Board's RF Window

Use case: `shared_lo=true` board. Both tasks request the same CF and SR — spectrum
slices are placed side-by-side within the device's RF window.

**First task →** `rx_count=1`, CF=915 MHz, BW=3 MHz, SR=10 MHz → assigned RX0.

**Second task →** `rx_count=1`, CF=915 MHz, BW=2 MHz, SR=10 MHz:

**Response ← (ACCEPTED)**
```json
{
  "status": "ACCEPTED",
  "streams": [{
    "device_id": "pluto-0",
    "channel_index": 1,
    "center_freq_hz": 915000000.0,
    "slice_offset_hz": -2500000.0,
    "slice_bw_hz": 2000000.0,
    "sample_rate_sps": 10000000.0
  }]
}
```

`slice_offset_hz` tells the DSP pod where within the 10 MHz RF window its 2 MHz
sub-band sits relative to the device LO.

---

### 11.5 Coherent Request — Rejection: One Board Has a Conflicting Task

Use case: `pluto-0` is already running at a different CF. Coherent DF request
across `refclk-group-0` fails because atomicity is required.

**Existing task**: `pluto-0`, CF=2400 MHz.

**Coherent DF request →** CF=915 MHz, `coherency_group="refclk-group-0"`, `rx_count=4`.

**Response ←**
```json
{
  "status": "REJECTED",
  "reject_code": "COHERENCY_UNAVAILABLE",
  "reject_reason": "Device pluto-0 blocked: RETUNE_CONFLICT"
}
```

---

### 11.6 Scan Task

**Request →**
```json
{
  "msg_type": "TASK_REQUEST_SCAN",
  "request_id": "req-006",
  "task_type": "WIDEBAND",
  "schedule": { "mode": "CONTINUOUS" },
  "scan": {
    "repeat": true,
    "rx_count": 1,
    "entries": [
      { "step": 1, "center_freq_hz": 915000000.0,  "bandwidth_hz": 5000000.0,  "sample_rate_sps": 5000000.0,  "dwell_ms": 1000 },
      { "step": 2, "center_freq_hz": 2400000000.0, "bandwidth_hz": 10000000.0, "sample_rate_sps": 10000000.0, "dwell_ms": 2000 }
    ]
  },
  "streaming": { "dest_ip": "10.0.1.13", "dest_ports": [5300] }
}
```

**Response ← (ACCEPTED)** — reservation uses the widest entry's bandwidth.

**IQ packet stream** — receiver sees:
```
Packet seq=0:  center_freq=915MHz,  flags=0x02 (FIRST_PACKET)
Packet seq=1:  center_freq=915MHz,  flags=0x00
...
[1000 ms dwell]
Packet seq=N:  center_freq=2400MHz, flags=0x04 (DWELL_CHANGE)
Packet seq=N+1: center_freq=2400MHz, flags=0x00
...
[2000 ms dwell → repeat from step 1]
```

**Stop →** `TASK_STOP` transitions to `CANCELLED`.

---

### 11.7 Snapshot on Preferred Device

**Request →**
```json
{
  "msg_type": "TASK_REQUEST_SNAPSHOT",
  "request_id": "req-007",
  "snapshot": {
    "center_freq_hz": 2400000000.0,
    "bandwidth_hz":   20000000.0,
    "sample_rate_sps": 20000000.0,
    "fft_size":       4096,
    "n_averages":     16,
    "preferred_device": "pluto-1"
  }
}
```

**Response ← (ACCEPTED)** — task_id returned but no streams (no IQ).

**Result → response queue**
```json
{
  "msg_type":   "SNAPSHOT_RESULT",
  "request_id": "req-007",
  "status":     "COMPLETED",
  "device_id":  "pluto-1",
  "fft_size":   4096,
  "power_bins": [-82.1, -80.4, "...4096 total..."]
}
```

If `pluto-1` is busy, the controller silently uses `pluto-0` instead.
If no device is available: `status = "FAILED"`.

---

### 11.8 Triggered Capture (Power Threshold)

**Request →**
```json
{
  "msg_type": "TASK_REQUEST_TRIGGERED",
  "request_id": "req-008",
  "schedule": { "mode": "CONTINUOUS" },
  "rf": {
    "center_freq_hz": 433920000.0,
    "bandwidth_hz":   2000000.0,
    "sample_rate_sps": 2000000.0,
    "rx_count": 1,
    "rx_gain_db": [50.0]
  },
  "trigger": {
    "type":            "POWER_THRESHOLD",
    "threshold_dbfs":  -60.0,
    "pre_trigger_ms":  50,
    "post_trigger_ms": 200,
    "max_captures":    1
  },
  "streaming": { "dest_ip": "10.0.1.10", "dest_ports": [5400] }
}
```

**Response ← (ACCEPTED)** — `state: RUNNING` immediately. IQ stream is silent until signal.

**On trigger event**: IQ stream starts with `IQ_FLAG_FIRST_PACKET`, runs for 200 ms,
then stops. After `max_captures=1` captures the task self-terminates: **TASK_STATUS**
`state: COMPLETED` is published.

With `max_captures=0` the task runs indefinitely; send `TASK_STOP` to end it.

---

### 11.9 Calibration (All Boards, Same Frequency)

**Request →**
```json
{
  "msg_type": "TASK_REQUEST_CALIBRATION",
  "request_id": "req-009",
  "calibration": {
    "center_freq_hz":      1000000000.0,
    "bandwidth_hz":        10000000.0,
    "sample_rate_sps":     10000000.0,
    "duration_ms":         5000,
    "rx_count_per_device": 2,
    "coherency_group":     "refclk-group-0",
    "devices":             []
  },
  "streaming": {
    "dest_ip":    "10.0.1.15",
    "dest_ports": [6000, 6001, 6002, 6003]
  }
}
```

Port order: `[pluto-0 RX0, pluto-0 RX1, pluto-1 RX0, pluto-1 RX1]`.

**Response ← (ACCEPTED)** — 4 streams. After 5000 ms: `state: COMPLETED`.

**Rejection**: if either board is busy at a different CF during that window:
```json
{ "status": "REJECTED", "reject_code": "COHERENCY_UNAVAILABLE" }
```

---

### 11.10 Rejection: Frequency Out of Range

**Request →** CF=10 MHz on hardware with `freq_min_hz=70 MHz`:
```json
{ "reject_code": "FREQ_OUT_OF_RANGE", "reject_reason": "10.0 MHz below minimum of all devices (70.0 MHz)" }
```

---

### 11.11 Rejection: Bandwidth Exceeded

**Request →** `bandwidth_hz=70 MHz` on hardware with `bandwidth_max_hz=56 MHz`:
```json
{ "reject_code": "BW_EXCEEDED" }
```

---

### 11.12 Rejection: Sample Rate Exceeded

**Request →** `sample_rate_sps=70 MHz` with hardware max 61.44 MSPS:
```json
{ "reject_code": "SAMPLE_RATE_EXCEEDED" }
```

---

### 11.13 Rejection: Spectrum Conflict (RF Slice Full)

Use case: `pluto-0` is running at 915 MHz with `shared_lo=true` and both RX channels
are occupied. A third task requests a channel at the same CF.

```json
{ "reject_code": "NO_DEVICE_AVAILABLE", "reject_reason": "No device satisfies constraints" }
```

Or if the RF window itself is full (no room for the requested bandwidth):
```json
{ "reject_code": "SPECTRUM_CONFLICT" }
```

---

### 11.14 Rejection: Task Limit Reached

`max_concurrent_tasks` in policy is 64; the 65th request in `{SCHEDULED,PENDING,RUNNING}`:
```json
{ "reject_code": "TASK_LIMIT_REACHED" }
```

---

### 11.15 Cancel a Scheduled Future Task

**Existing task**: `task-002`, state `SCHEDULED` (start time 30 s in the future).

**Cancel →**
```json
{ "msg_type": "TASK_CANCEL", "task_id": "task-002", "reason": "plan changed" }
```

**Response ← (ACCEPTED)** + **TASK_STATUS** `state: CANCELLED`.
Spectrum slot and ports freed immediately — that window is now available to others.

---

### 11.16 TASK_STOP vs TASK_CANCEL

| Operation     | Applicable states             | Result state | Use when                      |
|---------------|-------------------------------|--------------|-------------------------------|
| `TASK_STOP`   | `RUNNING` (CONTINUOUS / SCAN) | `CANCELLED`  | Gracefully end an active stream |
| `TASK_CANCEL` | Any non-terminal state        | `CANCELLED`  | Abort scheduled or running task |

Both release resources immediately and publish a final `TASK_STATUS`.

---

### 11.17 Health Query

**Request →**
```json
{ "msg_type": "HEALTH_QUERY", "request_id": "req-017" }
```

**Response → response queue** (see §8.17 for full format):
```json
{
  "msg_type": "HEALTH_QUERY_RESPONSE",
  "controller": {
    "online_devices": 2,
    "running_tasks": 1,
    "udp_ports_in_use": 2,
    "uptime_sec": 7200
  },
  "devices": [
    { "device_id": "pluto-0", "online": true, "active_tasks": 1, "temperature_c": 52.3 },
    { "device_id": "pluto-1", "online": true, "active_tasks": 0, "temperature_c": 49.1 }
  ]
}
```

---

## 12. OPERATIONAL FLOW DIAGRAMS

### 12.1 Scheduled Task — Normal Lifecycle

```
Client                   Broker           Controller            SoapySDR
  │──TASK_REQUEST──────────►────────────────►│                     │
  │  _SCHEDULED             │                │ admission checks     │
  │                         │                │ insert timeline slot │
  │◄─TASK_RESPONSE──────────◄────────────────│ ACCEPTED            │
  │◄─TASK_STATUS────────────◄────────────────│ SCHEDULED           │
  │                         │                │                     │
  │         [scheduler wakes every 200 ms]   │                     │
  │                         │                │──setFrequency()─────►│
  │                         │                │──setSampleRate()────►│
  │                         │                │──setupStream()──────►│
  │                         │                │──activateStream()───►│
  │◄─TASK_STATUS────────────◄────────────────│ RUNNING             │
  │                         │ UDP IQ packets ◄───────────────────────│
  │                         │                │                     │
  │         [stop_time reached — watchdog]   │                     │
  │                         │                │──deactivateStream()─►│
  │                         │                │──closeStream()──────►│
  │◄─TASK_STATUS────────────◄────────────────│ COMPLETED           │
```

### 12.2 Continuous Task

```
Client ──TASK_REQUEST_CONTINUOUS──► Controller → ACCEPTED + RUNNING
                                    Controller ──UDP IQ──► DSP Pod (indefinitely)
Client ──TASK_STOP──────────────── ► Controller
                                    Controller → stop IQ, release
                                    Controller ──TASK_STATUS(CANCELLED)──► topic
Client ◄─TASK_RESPONSE(ACCEPTED)── Controller
```

### 12.3 Scan Dwell Transitions

```
ScanExecutor ──setFrequency(step1=915 MHz)──► SoapySDR
              [1000 ms dwell]
IQStreamer    ─────UDP packets, cf=915 MHz, flag[DWELL_CHANGE]=1 on first─────►

ScanExecutor ──setFrequency(step2=2.4 GHz)──► SoapySDR
              [2000 ms dwell]
IQStreamer    ─────UDP packets, cf=2.4 GHz, flag[DWELL_CHANGE]=1 on first──►

              [repeat=true → back to step 1]
```

### 12.4 Triggered Capture

```
TriggerMonitor reads SoapySDR stream continuously:

  Level < threshold:  sample loop only, IQStreamer idle
  Level ≥ threshold:  IQStreamer.start() → UDP IQ packets → DSP Pod
                      after post_trigger_ms: IQStreamer.stop()
                      captures++
                      if captures == max_captures: task COMPLETED
```

---

## 13. VERSION HISTORY

| Version | Changes                                                                    |
|---------|----------------------------------------------------------------------------|
| 1.0     | Initial: scheduled/continuous/stop                                         |
| 2.0     | Add scan, snapshot, triggered, calibration; UDP format; multi-device; full reject codes |
| 2.1     | Add TASK_STATUS, DEVICE_HEALTH, CONTROLLER_HEALTH, HEALTH_QUERY_RESPONSE definitions; task lifecycle state machine; full scenarios section; `shared_lo` coherence mode; `coherency_group` cross-board DF |
| 2.2     | **`rank` field required** on all `TASK_REQUEST_*` messages; rank-based preemption (§4.2); `terminal_reason` and `rank` fields added to `TASK_STATUS`; `priority` field added to `TASK_STATUS` |
