#pragma once
// ════════════════════════════════════════════════════════════════════════
//  Types.hpp  —  SDR Radio Resource Task Manager
//  Single source of truth. No external deps except <std>.
// ════════════════════════════════════════════════════════════════════════
#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <chrono>
#include <limits>
#include <functional>
#include <atomic>

namespace sdr {

// ─── constants ──────────────────────────────────────────────────────────
inline constexpr uint32_t    IQ_PACKET_MAGIC       = 0x49515030u;  // "IQP0"
inline constexpr uint32_t    IQ_PACKET_HEADER_SIZE = 32u;
inline constexpr int64_t     TIME_INFINITE          = std::numeric_limits<int64_t>::max();
inline constexpr const char* SCHEMA_VERSION        = "2.0";
inline constexpr const char* APP_VERSION           = "2.0.0";

inline constexpr uint8_t IQ_FLAG_OVERFLOW      = 0x01u;
inline constexpr uint8_t IQ_FLAG_FIRST_PACKET  = 0x02u;
inline constexpr uint8_t IQ_FLAG_DWELL_CHANGE  = 0x04u;

// ─── task types ─────────────────────────────────────────────────────────
enum class TaskType : uint8_t {
    DF, NARROWBAND, WIDEBAND,
    SCAN, SNAPSHOT, TRIGGERED, CALIBRATION,
    UNKNOWN
};
inline TaskType taskTypeFromString(std::string_view s) noexcept {
    if (s == "DF")          return TaskType::DF;
    if (s == "NARROWBAND")  return TaskType::NARROWBAND;
    if (s == "WIDEBAND")    return TaskType::WIDEBAND;
    if (s == "SCAN")        return TaskType::SCAN;
    if (s == "SNAPSHOT")    return TaskType::SNAPSHOT;
    if (s == "TRIGGERED")   return TaskType::TRIGGERED;
    if (s == "CALIBRATION") return TaskType::CALIBRATION;
    return TaskType::UNKNOWN;
}
inline std::string taskTypeToString(TaskType t) {
    switch(t){
        case TaskType::DF:          return "DF";
        case TaskType::NARROWBAND:  return "NARROWBAND";
        case TaskType::WIDEBAND:    return "WIDEBAND";
        case TaskType::SCAN:        return "SCAN";
        case TaskType::SNAPSHOT:    return "SNAPSHOT";
        case TaskType::TRIGGERED:   return "TRIGGERED";
        case TaskType::CALIBRATION: return "CALIBRATION";
        default:                    return "UNKNOWN";
    }
}

// ─── schedule modes ─────────────────────────────────────────────────────
enum class ScheduleMode : uint8_t { SCHEDULED, IMMEDIATE, CONTINUOUS };
inline ScheduleMode scheduleModeFromString(std::string_view s) noexcept {
    if (s == "IMMEDIATE")  return ScheduleMode::IMMEDIATE;
    if (s == "CONTINUOUS") return ScheduleMode::CONTINUOUS;
    return ScheduleMode::SCHEDULED;
}
inline std::string scheduleModeToString(ScheduleMode m) {
    switch(m){
        case ScheduleMode::SCHEDULED:  return "SCHEDULED";
        case ScheduleMode::IMMEDIATE:  return "IMMEDIATE";
        case ScheduleMode::CONTINUOUS: return "CONTINUOUS";
    }
    return "SCHEDULED";
}

// ─── task states ────────────────────────────────────────────────────────
enum class TaskState : uint8_t {
    EVALUATING, SCHEDULED, PENDING, RUNNING,
    COMPLETING, COMPLETED, FAILED, CANCELLED
};
inline std::string taskStateToString(TaskState s) {
    switch(s){
        case TaskState::EVALUATING:  return "EVALUATING";
        case TaskState::SCHEDULED:   return "SCHEDULED";
        case TaskState::PENDING:     return "PENDING";
        case TaskState::RUNNING:     return "RUNNING";
        case TaskState::COMPLETING:  return "COMPLETING";
        case TaskState::COMPLETED:   return "COMPLETED";
        case TaskState::FAILED:      return "FAILED";
        case TaskState::CANCELLED:   return "CANCELLED";
    }
    return "UNKNOWN";
}
inline bool isTerminalState(TaskState s) {
    return s==TaskState::COMPLETED || s==TaskState::FAILED || s==TaskState::CANCELLED;
}

// ─── reject codes ────────────────────────────────────────────────────────
enum class RejectCode : uint8_t {
    NONE, INVALID_REQUEST, INVALID_TASK_TYPE, INVALID_SCHEDULE,
    FREQ_OUT_OF_RANGE, BW_EXCEEDED, SAMPLE_RATE_EXCEEDED,
    CHANNEL_COUNT_EXCEEDED, SPECTRUM_CONFLICT, TIME_CONFLICT,
    RETUNE_CONFLICT, NO_DEVICE_AVAILABLE, COHERENCY_UNAVAILABLE,
    TASK_LIMIT_REACHED, TASK_NOT_FOUND, TASK_NOT_STOPPABLE,
    TASK_ALREADY_TERMINAL, PORT_POOL_EXHAUSTED, SCAN_ENTRY_INVALID,
    DEVICE_OFFLINE, INTERNAL_ERROR
};
inline std::string rejectCodeToString(RejectCode c) {
    switch(c){
        case RejectCode::NONE:                   return "NONE";
        case RejectCode::INVALID_REQUEST:        return "INVALID_REQUEST";
        case RejectCode::INVALID_TASK_TYPE:      return "INVALID_TASK_TYPE";
        case RejectCode::INVALID_SCHEDULE:       return "INVALID_SCHEDULE";
        case RejectCode::FREQ_OUT_OF_RANGE:      return "FREQ_OUT_OF_RANGE";
        case RejectCode::BW_EXCEEDED:            return "BW_EXCEEDED";
        case RejectCode::SAMPLE_RATE_EXCEEDED:   return "SAMPLE_RATE_EXCEEDED";
        case RejectCode::CHANNEL_COUNT_EXCEEDED: return "CHANNEL_COUNT_EXCEEDED";
        case RejectCode::SPECTRUM_CONFLICT:      return "SPECTRUM_CONFLICT";
        case RejectCode::TIME_CONFLICT:          return "TIME_CONFLICT";
        case RejectCode::RETUNE_CONFLICT:        return "RETUNE_CONFLICT";
        case RejectCode::NO_DEVICE_AVAILABLE:    return "NO_DEVICE_AVAILABLE";
        case RejectCode::COHERENCY_UNAVAILABLE:  return "COHERENCY_UNAVAILABLE";
        case RejectCode::TASK_LIMIT_REACHED:     return "TASK_LIMIT_REACHED";
        case RejectCode::TASK_NOT_FOUND:         return "TASK_NOT_FOUND";
        case RejectCode::TASK_NOT_STOPPABLE:     return "TASK_NOT_STOPPABLE";
        case RejectCode::TASK_ALREADY_TERMINAL:  return "TASK_ALREADY_TERMINAL";
        case RejectCode::PORT_POOL_EXHAUSTED:    return "PORT_POOL_EXHAUSTED";
        case RejectCode::SCAN_ENTRY_INVALID:     return "SCAN_ENTRY_INVALID";
        case RejectCode::DEVICE_OFFLINE:         return "DEVICE_OFFLINE";
        case RejectCode::INTERNAL_ERROR:         return "INTERNAL_ERROR";
    }
    return "UNKNOWN";
}

// ─── task-type parameter blocks ─────────────────────────────────────────
struct DfParams {
    std::string algorithm       = "MUSIC";
    int         num_sources     = 1;
    int         snapshot_count  = 1024;
    double      angular_res_deg = 1.0;
};
struct NarrowbandParams {
    std::string demod            = "FM";
    double      squelch_dbfs     = -80.0;
    int         output_rate_sps  = 48000;
};
struct WidebandParams {
    bool   record_raw_iq         = true;
    double detect_threshold_dbfs = -60.0;
    int    fft_size              = 2048;
};
struct ScanEntry {
    int    step            = 0;
    double center_freq_hz  = 0.0;
    double bandwidth_hz    = 0.0;
    double sample_rate_sps = 0.0;
    int    dwell_ms        = 1000;
};
struct ScanParams {
    bool                   repeat  = true;
    std::vector<ScanEntry> entries;
};
struct SnapshotParams {
    double      center_freq_hz   = 0.0;
    double      bandwidth_hz     = 0.0;
    double      sample_rate_sps  = 0.0;
    int         fft_size         = 4096;
    int         n_averages       = 16;
    std::string preferred_device;
};
struct TriggerParams {
    std::string trigger_type     = "POWER_THRESHOLD";
    double      threshold_dbfs   = -60.0;
    int         pre_trigger_ms   = 50;
    int         post_trigger_ms  = 200;
    int         max_captures     = 0;
};
struct CalibrationParams {
    double      center_freq_hz      = 0.0;
    double      bandwidth_hz        = 0.0;
    double      sample_rate_sps     = 0.0;
    int         duration_ms         = 5000;
    int         rx_count_per_device = 2;
    std::string coherency_group;
    std::vector<std::string> devices;
};

// ─── RF request ──────────────────────────────────────────────────────────
struct RfRequest {
    double              center_freq_hz   = 0.0;
    double              bandwidth_hz     = 0.0;
    double              sample_rate_sps  = 0.0;
    int                 rx_count         = 0;
    int                 tx_count         = 0;
    std::vector<double> rx_gain_db;
    std::vector<bool>   rx_agc;
    std::vector<double> tx_atten_db;
    std::string         preferred_device;
    // If set, rx_count channels are allocated coherently across ALL online devices
    // in this group (must match coherency_group in devices.xml for each board).
    std::string         coherency_group;
};

struct StreamingDest {
    std::string      dest_ip;
    std::vector<int> dest_ports;
};

// ─── decoded task request ───────────────────────────────────────────────
struct TaskRequest {
    std::string   msg_type;
    std::string   schema_version;
    std::string   request_id;
    std::string   correlation_id;
    int64_t       timestamp_ms    = 0;

    TaskType      task_type       = TaskType::UNKNOWN;
    ScheduleMode  schedule_mode   = ScheduleMode::SCHEDULED;
    int           priority        = 0;

    int64_t       start_time_ms   = 0;
    int64_t       end_time_ms     = TIME_INFINITE;
    int64_t       duration_ms     = 0;

    // STOP / CANCEL
    std::string   task_id;
    std::string   reason;

    RfRequest     rf;
    StreamingDest streaming;

    std::optional<DfParams>          df_params;
    std::optional<NarrowbandParams>  nb_params;
    std::optional<WidebandParams>    wb_params;
    std::optional<ScanParams>        scan_params;
    std::optional<SnapshotParams>    snapshot_params;
    std::optional<TriggerParams>     trigger_params;
    std::optional<CalibrationParams> cal_params;
};

// ─── assigned stream (in response) ──────────────────────────────────────
struct AssignedStream {
    std::string stream_id;
    std::string device_id;
    std::string channel_type;   // "RX" or "TX"
    int         channel_index  = 0;
    std::string udp_ip;
    int         udp_port       = 0;
    double      center_freq_hz = 0.0;
    double      slice_offset_hz= 0.0;
    double      slice_bw_hz    = 0.0;
    double      sample_rate_sps= 0.0;
    std::string format         = "CF32";
};

// ─── task response ───────────────────────────────────────────────────────
struct TaskResponse {
    std::string  request_id;
    std::string  correlation_id;
    bool         accepted        = false;
    std::string  task_id;
    std::string  schedule_mode;
    int64_t      actual_start_ms = 0;
    int64_t      actual_stop_ms  = 0;
    RejectCode   reject_code     = RejectCode::NONE;
    std::string  reject_reason;
    std::vector<AssignedStream> streams;
};

// ─── stream metrics ──────────────────────────────────────────────────────
struct StreamMetrics {
    std::string stream_id;
    std::string channel_type;
    int         channel_index   = 0;
    int         udp_port        = 0;
    uint64_t    samples_total   = 0;
    uint64_t    packets_sent    = 0;
    uint32_t    overflows       = 0;
    float       rssi_dbfs       = 0.0f;
    double      throughput_mbps = 0.0;
};

// ─── IQ packet header — binary, LE, 32 bytes ─────────────────────────────
#pragma pack(push, 1)
struct IqPacketHeader {
    uint32_t magic;           // IQ_PACKET_MAGIC
    uint32_t sequence;
    uint64_t timestamp_ns;    // ns from task start
    uint64_t center_freq_hz;  // updated on scan dwell
    uint32_t sample_rate;
    uint16_t num_samples;
    uint8_t  channel_index;
    uint8_t  flags;           // IQ_FLAG_* bits
};
static_assert(sizeof(IqPacketHeader) == IQ_PACKET_HEADER_SIZE);
#pragma pack(pop)

// ─── internal task record ────────────────────────────────────────────────
struct TaskRecord {
    std::string   task_id;
    std::string   request_id;
    std::string   correlation_id;
    TaskType      task_type     = TaskType::UNKNOWN;
    ScheduleMode  schedule_mode = ScheduleMode::SCHEDULED;
    TaskState     state         = TaskState::EVALUATING;
    int           priority      = 0;
    int64_t       start_time_ms = 0;
    int64_t       stop_time_ms  = TIME_INFINITE;

    struct DeviceAllocation {
        std::string      device_id;
        std::vector<int> rx_channels;
        std::vector<int> tx_channels;
        std::vector<int> udp_ports;
        double           center_freq_hz  = 0.0;
        double           sample_rate_sps = 0.0;
        double           slice_lo_hz     = 0.0;
        double           slice_hi_hz     = 0.0;
    };
    std::vector<DeviceAllocation> allocations;

    StreamingDest streaming;

    std::optional<ScanParams>        scan_params;
    std::optional<TriggerParams>     trigger_params;
    std::optional<CalibrationParams> cal_params;

    std::chrono::steady_clock::time_point accepted_at;
    std::vector<StreamMetrics>            stream_metrics;
    std::string                           terminal_reason;
};

// ─── callbacks ───────────────────────────────────────────────────────────
using TaskStateChangedCb = std::function<void(const TaskRecord&)>;
using TaskErrorCb        = std::function<void(const std::string& task_id,
                                               const std::string& error)>;

// ─── snapshot result (from FftEngine) ────────────────────────────────────
struct SnapshotResult {
    std::string         device_id;
    double              center_freq_hz     = 0.0;
    double              bandwidth_hz       = 0.0;
    double              sample_rate_sps    = 0.0;
    int                 fft_size           = 0;
    int                 n_averages         = 0;
    double              freq_resolution_hz = 0.0;
    double              freq_axis_start_hz = 0.0;
    double              freq_axis_step_hz  = 0.0;
    std::vector<double> power_bins;
    bool                success            = false;
    std::string         error_msg;
};

} // namespace sdr
