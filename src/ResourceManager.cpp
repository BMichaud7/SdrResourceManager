#include "ResourceManager.hpp"
#include "MessageCodec.hpp"
#include <spdlog/spdlog.h>
#include <uuid/uuid.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <cmath>

namespace sdr {

using namespace std::chrono;

static std::string genUuid() {
    uuid_t uu; uuid_generate_random(uu);
    char buf[37]; uuid_unparse_lower(uu, buf);
    return buf;
}

// Aggregate capability bounds across all loaded devices so validation is
// device-agnostic — a request is sane if at least one device could handle it.
struct CapBounds { double freq_min, freq_max, bw_max, sr_max; };
static CapBounds aggregateCaps(
    const std::unordered_map<std::string, std::unique_ptr<RadioDevice>>& devs)
{
    CapBounds b{ std::numeric_limits<double>::max(), 0.0, 0.0, 0.0 };
    for (auto& [id, dev] : devs) {
        const auto& c = dev->config().caps;
        b.freq_min = std::min(b.freq_min, c.freq_min_hz);
        b.freq_max = std::max(b.freq_max, c.freq_max_hz);
        b.bw_max   = std::max(b.bw_max,   c.bandwidth_max_hz);
        b.sr_max   = std::max(b.sr_max,   c.sample_rate_max_sps);
    }
    return b;
}

// ─────────────────────────────────────────────────────────────────────────
ResourceManager::ResourceManager(const AppConfig& cfg,
                                  TaskStateChangedCb on_state_change,
                                  TaskErrorCb on_error)
    : cfg_(cfg)
    , on_state_change_(std::move(on_state_change))
    , on_error_(std::move(on_error))
    , start_time_(steady_clock::now())
{
    port_pool_  = std::make_unique<UdpPortPool>(
        cfg_.policy.udp_port_pool_start, cfg_.policy.udp_port_pool_end);
    fft_engine_ = std::make_unique<FftEngine>();

    for (auto& dc : cfg_.devices) {
        devices_[dc.id]   = std::make_unique<RadioDevice>(dc);
        timelines_[dc.id] = std::make_unique<SpectrumTimeline>();
    }
}

ResourceManager::~ResourceManager() {
    closeDevices();
}

int ResourceManager::openDevices() {
    int ok = 0;
    for (auto& [id, dev] : devices_) {
        if (dev->open()) { ++ok; spdlog::info("ResourceManager: opened {}", id); }
        else             { spdlog::warn("ResourceManager: failed to open {}", id); }
    }
    return ok;
}

void ResourceManager::closeDevices() {
    // Deactivate all running tasks first
    std::vector<std::string> active_ids;
    {
        std::lock_guard lock(reg_mu_);
        for (auto& [id, rec] : registry_)
            if (!isTerminalState(rec.state)) active_ids.push_back(id);
    }
    for (auto& id : active_ids)
        deactivateTask(id, TaskState::CANCELLED, "controller shutdown");

    for (auto& [id, dev] : devices_) dev->close();
}

// ── Device selection ─────────────────────────────────────────────────────
std::optional<ResourceManager::DeviceCandidate>
ResourceManager::findBestDevice(double cf, double bw, double sr,
                                 int rx, int tx,
                                 int64_t t_start, int64_t t_stop,
                                 const std::string& preferred) const
{
    // Sort devices: preferred first, then by free BW descending
    std::vector<std::string> ordered;
    if (!preferred.empty() && devices_.count(preferred))
        ordered.push_back(preferred);
    for (auto& [id, _] : devices_)
        if (id != preferred) ordered.push_back(id);

    // Sort non-preferred by free BW
    if (ordered.size() > 1) {
        auto pref_it = (!preferred.empty() && devices_.count(preferred))
                       ? ordered.begin() : ordered.end();
        std::stable_sort(pref_it == ordered.begin() ? pref_it+1 : ordered.begin(),
                         ordered.end(),
                         [&](const std::string& a, const std::string& b) {
                             auto ta = timelines_.at(a)->allocatedBw(nowMs());
                             auto tb = timelines_.at(b)->allocatedBw(nowMs());
                             return ta < tb;   // less allocated = more free
                         });
    }

    for (auto& id : ordered) {
        auto& dev = devices_.at(id);
        if (!dev->isOnline()) continue;
        const auto& caps = dev->config().caps;

        // Capability check
        if (cf < caps.freq_min_hz || cf > caps.freq_max_hz) continue;
        if (bw > caps.bandwidth_max_hz) continue;
        if (sr > caps.sample_rate_max_sps) continue;
        if (rx > caps.rx_channels) continue;
        if (tx > caps.tx_channels) continue;

        auto fit = timelines_.at(id)->canFit(
            t_start, t_stop, cf, bw, sr, rx, tx,
            caps.rx_channels, caps.tx_channels,
            cfg_.policy.guard_band_hz,
            dev->config().shared_lo);

        if (fit.ok)
            return DeviceCandidate{dev.get(), std::move(fit)};
    }
    return std::nullopt;
}

// ── tryAccept entry point ────────────────────────────────────────────────
TaskResponse ResourceManager::tryAccept(const TaskRequest& req) {
    // Phase 0: type routing
    if (req.msg_type == "TASK_REQUEST_SNAPSHOT")
        return doAcceptSnapshot(req);
    if (req.msg_type == "TASK_REQUEST_CALIBRATION")
        return doAcceptCalibration(req);
    if (req.msg_type == "TASK_REQUEST_SCAN")
        return doAcceptScan(req);

    return doAcceptStandard(req);
}

// ── Standard fixed-tune tasks (SCHEDULED / IMMEDIATE / CONTINUOUS) ───────
TaskResponse ResourceManager::doAcceptStandard(const TaskRequest& req) {
    TaskResponse resp;
    resp.request_id    = req.request_id;
    resp.correlation_id= req.correlation_id;
    resp.schedule_mode = scheduleModeToString(req.schedule_mode);

    // Phase 1: validate against the union of all loaded device capabilities
    {
        auto caps = aggregateCaps(devices_);
        if (req.rf.center_freq_hz < caps.freq_min || req.rf.center_freq_hz > caps.freq_max) {
            resp.reject_code=RejectCode::FREQ_OUT_OF_RANGE;
            resp.reject_reason="center_freq outside all loaded device ranges"; return resp; }
        if (req.rf.bandwidth_hz <= 0 || req.rf.bandwidth_hz > caps.bw_max) {
            resp.reject_code=RejectCode::BW_EXCEEDED;
            resp.reject_reason="bandwidth exceeds all loaded device capabilities"; return resp; }
        if (req.rf.sample_rate_sps <= 0 || req.rf.sample_rate_sps > caps.sr_max) {
            resp.reject_code=RejectCode::SAMPLE_RATE_EXCEEDED;
            resp.reject_reason="sample_rate exceeds all loaded device capabilities"; return resp; }
    }
    if (req.rf.rx_count < 0 || req.rf.tx_count < 0 ||
        (req.rf.rx_count==0 && req.rf.tx_count==0)) {
        resp.reject_code=RejectCode::INVALID_REQUEST;
        resp.reject_reason="Must request ≥1 channel"; return resp; }

    // Phase 2: time window
    int64_t t_start = 0, t_stop = 0;
    int64_t now = nowMs();
    switch(req.schedule_mode) {
        case ScheduleMode::SCHEDULED:
            t_start = req.start_time_ms;
            t_stop  = req.end_time_ms;
            if (t_start >= t_stop || t_stop <= now) {
                resp.reject_code=RejectCode::INVALID_SCHEDULE;
                resp.reject_reason="Invalid time window"; return resp; }
            break;
        case ScheduleMode::IMMEDIATE:
            t_start = now;
            t_stop  = (req.duration_ms > 0) ? now + req.duration_ms
                                             : now + cfg_.policy.default_task_timeout_ms;
            break;
        case ScheduleMode::CONTINUOUS:
            t_start = now;
            t_stop  = TIME_INFINITE;
            break;
    }

    // Phase 3: global cap
    if (countByState(TaskState::SCHEDULED) +
        countByState(TaskState::PENDING)   +
        countByState(TaskState::RUNNING)   >= cfg_.policy.max_concurrent_tasks) {
        resp.reject_code=RejectCode::TASK_LIMIT_REACHED;
        resp.reject_reason="Global task cap reached"; return resp; }

    // ── Helper: build a TaskRecord and commit it ─────────────────────────────
    auto commitTask = [&](std::vector<TaskRecord::DeviceAllocation> allocs,
                          std::vector<int> all_ports) -> TaskRecord {
        TaskRecord rec;
        rec.task_id        = genUuid();
        rec.request_id     = req.request_id;
        rec.correlation_id = req.correlation_id;
        rec.task_type      = req.task_type;
        rec.schedule_mode  = req.schedule_mode;
        rec.priority       = req.priority;
        rec.start_time_ms  = t_start;
        rec.stop_time_ms   = t_stop;
        rec.allocations    = std::move(allocs);
        rec.streaming      = req.streaming;
        if (req.scan_params)    rec.scan_params    = req.scan_params;
        if (req.trigger_params) rec.trigger_params = req.trigger_params;
        rec.accepted_at    = steady_clock::now();
        bool activate_now  = (req.schedule_mode == ScheduleMode::IMMEDIATE ||
                              req.schedule_mode == ScheduleMode::CONTINUOUS);
        rec.state = activate_now ? TaskState::PENDING : TaskState::SCHEDULED;
        return rec;
    };

    // ── Multi-device coherent path ───────────────────────────────────────────
    if (!req.rf.coherency_group.empty()) {
        // Collect all online devices in the requested coherency group.
        std::vector<std::string> group_devs;
        for (auto& [id, dev] : devices_)
            if (dev->isOnline() && dev->config().coherency_group == req.rf.coherency_group)
                group_devs.push_back(id);

        if (group_devs.empty()) {
            resp.reject_code   = RejectCode::COHERENCY_UNAVAILABLE;
            resp.reject_reason = "No online devices in coherency_group '" +
                                 req.rf.coherency_group + "'";
            return resp;
        }

        // Distribute rx_count across devices greedily (fill each to its cap).
        int remaining = req.rf.rx_count;
        std::vector<std::pair<std::string,int>> plan; // {device_id, n_rx}
        for (auto& id : group_devs) {
            if (remaining <= 0) break;
            int cap = devices_.at(id)->config().caps.rx_channels;
            int n   = std::min(remaining, cap);
            plan.push_back({id, n});
            remaining -= n;
        }
        if (remaining > 0) {
            resp.reject_code   = RejectCode::COHERENCY_UNAVAILABLE;
            resp.reject_reason = "Coherency group '" + req.rf.coherency_group +
                                 "' has insufficient RX channels";
            return resp;
        }

        // Check each device can fit its share.
        std::vector<FitResult> fits;
        for (auto& [dev_id, n_rx] : plan) {
            auto& dev = devices_.at(dev_id);
            auto fit  = timelines_.at(dev_id)->canFit(
                t_start, t_stop,
                req.rf.center_freq_hz, req.rf.bandwidth_hz, req.rf.sample_rate_sps,
                n_rx, 0,
                dev->config().caps.rx_channels, dev->config().caps.tx_channels,
                cfg_.policy.guard_band_hz, dev->config().shared_lo);
            if (!fit.ok) {
                resp.reject_code   = RejectCode::COHERENCY_UNAVAILABLE;
                resp.reject_reason = "Device " + dev_id + " blocked: " + fit.reject_reason;
                return resp;
            }
            fits.push_back(fit);
        }

        // Allocate ports for all channels.
        auto ports = port_pool_->allocateN(req.rf.rx_count);
        if ((int)ports.size() < req.rf.rx_count) {
            resp.reject_code   = RejectCode::PORT_POOL_EXHAUSTED;
            resp.reject_reason = "UDP port pool exhausted"; return resp;
        }

        std::string task_id = genUuid();
        resp.task_id         = task_id;
        resp.actual_start_ms = t_start;
        resp.actual_stop_ms  = t_stop;
        resp.accepted        = true;

        int port_idx = 0;
        std::vector<TaskRecord::DeviceAllocation> allocs;
        for (int pi = 0; pi < (int)plan.size(); ++pi) {
            auto& [dev_id, n_rx] = plan[pi];
            auto& fit = fits[pi];

            TaskRecord::DeviceAllocation alloc;
            alloc.device_id       = dev_id;
            alloc.rx_channels     = fit.avail_rx;
            alloc.center_freq_hz  = fit.device_cf;
            alloc.sample_rate_sps = fit.device_rate;
            alloc.slice_lo_hz     = fit.placed_lo;
            alloc.slice_hi_hz     = fit.placed_hi;

            for (int i = 0; i < n_rx; ++i) {
                int p = ports[port_idx++];
                alloc.udp_ports.push_back(p);
                AssignedStream as;
                as.stream_id       = makeStreamId(task_id, "RX", dev_id, fit.avail_rx[i]);
                as.device_id       = dev_id;
                as.channel_type    = "RX";
                as.channel_index   = fit.avail_rx[i];
                as.udp_ip          = devices_.at(dev_id)->config().streaming_source_ip;
                as.udp_port        = p;
                as.center_freq_hz  = fit.device_cf;
                as.slice_offset_hz = (fit.placed_lo + fit.placed_hi)/2.0 - fit.device_cf;
                as.slice_bw_hz     = fit.placed_hi - fit.placed_lo;
                as.sample_rate_sps = fit.device_rate;
                resp.streams.push_back(as);
            }

            TimeFreqSlot slot;
            slot.task_id        = task_id;
            slot.t_start        = t_start;  slot.t_stop = t_stop;
            slot.center_freq_hz = fit.device_cf;
            slot.sample_rate_sps= fit.device_rate;
            slot.slice_lo_hz    = fit.placed_lo;
            slot.slice_hi_hz    = fit.placed_hi;
            slot.rx_channels    = fit.avail_rx;
            timelines_[dev_id]->insert(slot);

            allocs.push_back(std::move(alloc));
        }

        TaskRecord rec = commitTask(std::move(allocs), ports);
        rec.task_id = task_id;

        std::string dev_list;
        for (auto& [id, _] : plan) dev_list += id + " ";
        spdlog::info("ResourceManager: task {} ACCEPTED on [{}] state={} (coherent)",
                     task_id, dev_list, taskStateToString(rec.state));

        bool activate_now = (rec.state == TaskState::PENDING);
        { std::lock_guard lock(reg_mu_); registry_[task_id] = rec; }
        if (activate_now) activateTask(task_id);
        notifyStateChange(task_id);
        return resp;
    }

    // ── Single-device path ───────────────────────────────────────────────────
    auto candidate = findBestDevice(
        req.rf.center_freq_hz, req.rf.bandwidth_hz, req.rf.sample_rate_sps,
        req.rf.rx_count, req.rf.tx_count, t_start, t_stop, req.rf.preferred_device);

    if (!candidate) {
        resp.reject_code   = RejectCode::NO_DEVICE_AVAILABLE;
        resp.reject_reason = "No device satisfies constraints";
        return resp;
    }

    // Port allocation
    int n_ports = req.rf.rx_count + req.rf.tx_count;
    auto ports = port_pool_->allocateN(n_ports);
    if ((int)ports.size() < n_ports) {
        resp.reject_code   = RejectCode::PORT_POOL_EXHAUSTED;
        resp.reject_reason = "UDP port pool exhausted"; return resp;
    }

    std::string task_id = genUuid();
    resp.task_id        = task_id;
    resp.actual_start_ms= t_start;
    resp.actual_stop_ms = t_stop;
    resp.accepted       = true;

    // Build allocation record
    TaskRecord::DeviceAllocation alloc;
    alloc.device_id       = candidate->device->id();
    alloc.rx_channels     = candidate->fit.avail_rx;
    alloc.tx_channels     = candidate->fit.avail_tx;
    alloc.center_freq_hz  = candidate->fit.device_cf;
    alloc.sample_rate_sps = candidate->fit.device_rate;
    alloc.slice_lo_hz     = candidate->fit.placed_lo;
    alloc.slice_hi_hz     = candidate->fit.placed_hi;

    // Insert timeline slot
    TimeFreqSlot slot;
    slot.task_id        = task_id;
    slot.t_start        = t_start;
    slot.t_stop         = t_stop;
    slot.center_freq_hz = alloc.center_freq_hz;
    slot.sample_rate_sps= alloc.sample_rate_sps;
    slot.slice_lo_hz    = alloc.slice_lo_hz;
    slot.slice_hi_hz    = alloc.slice_hi_hz;
    slot.rx_channels    = alloc.rx_channels;
    slot.tx_channels    = alloc.tx_channels;
    timelines_[alloc.device_id]->insert(slot);

    // Build assigned stream descriptors
    int port_idx = 0;
    for (int i=0; i<(int)alloc.rx_channels.size(); ++i) {
        int p = ports[port_idx++];
        alloc.udp_ports.push_back(p);
        AssignedStream as;
        as.stream_id      = makeStreamId(task_id,"RX",alloc.device_id,alloc.rx_channels[i]);
        as.device_id      = alloc.device_id;
        as.channel_type   = "RX";
        as.channel_index  = alloc.rx_channels[i];
        as.udp_ip         = candidate->device->config().streaming_source_ip;
        as.udp_port       = p;
        as.center_freq_hz = alloc.center_freq_hz;
        as.slice_offset_hz= (alloc.slice_lo_hz+alloc.slice_hi_hz)/2.0 - alloc.center_freq_hz;
        as.slice_bw_hz    = alloc.slice_hi_hz - alloc.slice_lo_hz;
        as.sample_rate_sps= alloc.sample_rate_sps;
        resp.streams.push_back(as);
    }
    for (int i=0; i<(int)alloc.tx_channels.size(); ++i) {
        int p = ports[port_idx++];
        alloc.udp_ports.push_back(p);
        AssignedStream as;
        as.stream_id      = makeStreamId(task_id,"TX",alloc.device_id,alloc.tx_channels[i]);
        as.device_id      = alloc.device_id;
        as.channel_type   = "TX";
        as.channel_index  = alloc.tx_channels[i];
        as.udp_ip         = candidate->device->config().streaming_source_ip;
        as.udp_port       = p;
        as.center_freq_hz = alloc.center_freq_hz;
        as.slice_offset_hz= 0.0;
        as.slice_bw_hz    = alloc.slice_hi_hz - alloc.slice_lo_hz;
        as.sample_rate_sps= alloc.sample_rate_sps;
        resp.streams.push_back(as);
    }

    // Register task
    TaskRecord rec = commitTask({alloc}, ports);
    rec.task_id = task_id;

    spdlog::info("ResourceManager: task {} ACCEPTED on {} state={}",
                 task_id, alloc.device_id, taskStateToString(rec.state));

    bool activate_now = (rec.state == TaskState::PENDING);
    { std::lock_guard lock(reg_mu_); registry_[task_id] = rec; }
    if (activate_now) activateTask(task_id);
    notifyStateChange(task_id);
    return resp;
}

// ── Scan task — uses max BW entry for reservation ──────────────────────
TaskResponse ResourceManager::doAcceptScan(const TaskRequest& req) {
    TaskResponse resp;
    resp.request_id    = req.request_id;
    resp.correlation_id= req.correlation_id;
    resp.schedule_mode = scheduleModeToString(req.schedule_mode);

    if (!req.scan_params || req.scan_params->entries.empty()) {
        resp.reject_code = RejectCode::SCAN_ENTRY_INVALID;
        resp.reject_reason = "Scan entries list empty"; return resp; }

    // Validate each entry against the union of all loaded device capabilities
    {
        auto caps = aggregateCaps(devices_);
        for (auto& e : req.scan_params->entries) {
            if (e.center_freq_hz < caps.freq_min || e.center_freq_hz > caps.freq_max ||
                e.bandwidth_hz <= 0 || e.bandwidth_hz > caps.bw_max ||
                e.sample_rate_sps <= 0 || e.dwell_ms < 100) {
                resp.reject_code = RejectCode::SCAN_ENTRY_INVALID;
                resp.reject_reason = "Invalid scan entry at step "+std::to_string(e.step);
                return resp;
            }
        }
    }

    // Reserve based on first entry (scheduler models scan as time-slot)
    // Real RF scheduler could reserve per-step, but for simplicity:
    // A scan task occupies one slot with the CF/BW of step[0].
    const auto& e0 = req.scan_params->entries[0];

    // Build a synthetic RfRequest and call standard path
    TaskRequest synth = req;
    synth.rf.center_freq_hz  = e0.center_freq_hz;
    synth.rf.bandwidth_hz    = e0.bandwidth_hz;
    synth.rf.sample_rate_sps = e0.sample_rate_sps;
    synth.msg_type = "TASK_REQUEST_CONTINUOUS";
    synth.schedule_mode = req.start_time_ms > 0
        ? ScheduleMode::SCHEDULED : ScheduleMode::CONTINUOUS;

    return doAcceptStandard(synth);
}

// ── Snapshot task ────────────────────────────────────────────────────────
TaskResponse ResourceManager::doAcceptSnapshot(const TaskRequest& req) {
    TaskResponse resp;
    resp.request_id    = req.request_id;
    resp.correlation_id= req.correlation_id;
    resp.schedule_mode = "IMMEDIATE";

    if (!req.snapshot_params) {
        resp.reject_code = RejectCode::INVALID_REQUEST;
        resp.reject_reason = "snapshot params missing"; return resp;
    }
    auto& sp = *req.snapshot_params;

    auto candidate = findBestDevice(
        sp.center_freq_hz, sp.bandwidth_hz, sp.sample_rate_sps,
        1, 0, nowMs(), nowMs()+5000, sp.preferred_device);

    if (!candidate) {
        resp.reject_code = RejectCode::NO_DEVICE_AVAILABLE;
        resp.reject_reason = "No device available for snapshot"; return resp;
    }

    auto* dev = candidate->device;
    int snap_ch = candidate->fit.avail_rx[0];
    bool tune_ok = dev->config().shared_lo
                   ? dev->tune(sp.center_freq_hz, sp.sample_rate_sps)
                   : dev->tuneChannel(snap_ch, sp.center_freq_hz, sp.sample_rate_sps);
    if (!tune_ok) {
        resp.reject_code = RejectCode::INTERNAL_ERROR;
        resp.reject_reason = "Tune failed"; return resp;
    }

    std::vector<int> chans = {snap_ch};
    SoapySDR::Stream* stream = dev->openRxStream(chans);
    if (!stream) {
        resp.reject_code = RejectCode::INTERNAL_ERROR;
        resp.reject_reason = "Stream open failed"; return resp;
    }

    dev->activateStream(stream);

    int total = sp.fft_size * sp.n_averages;
    std::vector<float> buf((size_t)total * 2);
    void* bufs[1] = {buf.data()};
    int flags=0; long long hw_ts=0;
    int collected = 0;
    while (collected < total) {
        int rem = total - collected;
        int ret = dev->readStream(stream, bufs, (size_t)rem, flags, hw_ts, 1'000'000LL);
        if (ret > 0) {
            bufs[0] = buf.data() + collected*2;
            collected += ret;
        }
    }

    dev->deactivateStream(stream);
    dev->closeStream(stream);

    auto result = fft_engine_->compute(buf, sp.fft_size, sp.n_averages,
                                       sp.center_freq_hz, sp.sample_rate_sps,
                                       sp.bandwidth_hz, dev->id());

    // The SNAPSHOT_RESULT is published by Controller directly.
    // We signal success via a special task_id prefix.
    resp.accepted  = result.success;
    resp.task_id   = result.success ? "snapshot-"+genUuid() : "";
    if (!result.success) {
        resp.reject_code   = RejectCode::INTERNAL_ERROR;
        resp.reject_reason = result.error_msg;
    }
    return resp;
}

// ── Calibration task ─────────────────────────────────────────────────────
TaskResponse ResourceManager::doAcceptCalibration(const TaskRequest& req) {
    TaskResponse resp;
    resp.request_id    = req.request_id;
    resp.correlation_id= req.correlation_id;

    if (!req.cal_params) {
        resp.reject_code = RejectCode::INVALID_REQUEST;
        resp.reject_reason = "calibration params missing"; return resp;
    }
    auto& cp = *req.cal_params;

    // Find all devices in group
    std::vector<std::string> target_devs;
    if (cp.devices.empty()) {
        for (auto& [id, dev] : devices_)
            if (dev->config().coherency_group == cp.coherency_group && dev->isOnline())
                target_devs.push_back(id);
    } else {
        target_devs = cp.devices;
    }

    if (target_devs.empty()) {
        resp.reject_code = RejectCode::COHERENCY_UNAVAILABLE;
        resp.reject_reason = "No devices in group " + cp.coherency_group; return resp;
    }

    int64_t t_start = nowMs();
    int64_t t_stop  = t_start + cp.duration_ms;

    // Check all devices are free
    for (auto& dev_id : target_devs) {
        if (!timelines_.count(dev_id)) continue;
        auto fit = timelines_[dev_id]->canFit(
            t_start, t_stop, cp.center_freq_hz, cp.bandwidth_hz, cp.sample_rate_sps,
            cp.rx_count_per_device, 0,
            devices_[dev_id]->config().caps.rx_channels,
            devices_[dev_id]->config().caps.tx_channels,
            cfg_.policy.guard_band_hz,
            devices_[dev_id]->config().shared_lo);
        if (!fit.ok) {
            resp.reject_code   = RejectCode::COHERENCY_UNAVAILABLE;
            resp.reject_reason = "Device "+dev_id+" not free: "+fit.reject_reason;
            return resp;
        }
    }

    // Allocate ports
    int total_ports = (int)target_devs.size() * cp.rx_count_per_device;
    auto ports = port_pool_->allocateN(total_ports);
    if ((int)ports.size() < total_ports) {
        resp.reject_code = RejectCode::PORT_POOL_EXHAUSTED;
        resp.reject_reason = "Port pool exhausted for calibration"; return resp;
    }

    std::string task_id = genUuid();
    resp.task_id     = task_id;
    resp.accepted    = true;
    resp.actual_start_ms = t_start;
    resp.actual_stop_ms  = t_stop;

    TaskRecord rec;
    rec.task_id      = task_id;
    rec.request_id   = req.request_id;
    rec.task_type    = TaskType::CALIBRATION;
    rec.schedule_mode= ScheduleMode::IMMEDIATE;
    rec.start_time_ms= t_start;
    rec.stop_time_ms = t_stop;
    rec.streaming    = req.streaming;
    rec.cal_params   = cp;
    rec.state        = TaskState::PENDING;
    rec.accepted_at  = steady_clock::now();

    int port_idx = 0;
    for (auto& dev_id : target_devs) {
        TaskRecord::DeviceAllocation alloc;
        alloc.device_id      = dev_id;
        alloc.center_freq_hz = cp.center_freq_hz;
        alloc.sample_rate_sps= cp.sample_rate_sps;
        for (int i=0;i<cp.rx_count_per_device;++i) alloc.rx_channels.push_back(i);
        for (int i=0;i<cp.rx_count_per_device;++i) {
            int p = ports[port_idx++];
            alloc.udp_ports.push_back(p);
            AssignedStream as;
            as.stream_id     = makeStreamId(task_id,"RX",dev_id,i);
            as.device_id     = dev_id;
            as.channel_type  = "RX";
            as.channel_index = i;
            as.udp_ip        = devices_[dev_id]->config().streaming_source_ip;
            as.udp_port      = p;
            as.center_freq_hz= cp.center_freq_hz;
            as.sample_rate_sps=cp.sample_rate_sps;
            resp.streams.push_back(as);
        }
        alloc.slice_lo_hz = cp.center_freq_hz - cp.bandwidth_hz/2;
        alloc.slice_hi_hz = cp.center_freq_hz + cp.bandwidth_hz/2;

        TimeFreqSlot slot;
        slot.task_id        = task_id;
        slot.t_start        = t_start; slot.t_stop = t_stop;
        slot.center_freq_hz = cp.center_freq_hz;
        slot.sample_rate_sps= cp.sample_rate_sps;
        slot.slice_lo_hz    = alloc.slice_lo_hz;
        slot.slice_hi_hz    = alloc.slice_hi_hz;
        slot.rx_channels    = alloc.rx_channels;
        timelines_[dev_id]->insert(slot);
        rec.allocations.push_back(alloc);
    }

    {
        std::lock_guard lock(reg_mu_);
        registry_[task_id] = rec;
    }

    activateTask(task_id);
    notifyStateChange(task_id);
    return resp;
}

// ── Stop / Cancel ────────────────────────────────────────────────────────
TaskResponse ResourceManager::stopTask(const std::string& task_id,
                                        const std::string& req_id,
                                        const std::string& reason) {
    TaskResponse resp;
    resp.request_id = req_id;

    {
        std::lock_guard lock(reg_mu_);
        auto it = registry_.find(task_id);
        if (it == registry_.end()) {
            resp.reject_code = RejectCode::TASK_NOT_FOUND; return resp;
        }
        auto& rec = it->second;
        if (isTerminalState(rec.state)) {
            resp.reject_code = RejectCode::TASK_ALREADY_TERMINAL; return resp;
        }
        if (rec.schedule_mode == ScheduleMode::SCHEDULED &&
            rec.task_type != TaskType::SCAN &&
            rec.task_type != TaskType::TRIGGERED) {
            resp.reject_code = RejectCode::TASK_NOT_STOPPABLE; return resp;
        }
    }
    deactivateTask(task_id, TaskState::CANCELLED, reason);
    resp.accepted = true; resp.task_id = task_id;
    return resp;
}

TaskResponse ResourceManager::cancelTask(const std::string& task_id,
                                          const std::string& req_id,
                                          const std::string& reason) {
    TaskResponse resp; resp.request_id = req_id;
    {
        std::lock_guard lock(reg_mu_);
        if (!registry_.count(task_id)) {
            resp.reject_code = RejectCode::TASK_NOT_FOUND; return resp;
        }
        if (isTerminalState(registry_[task_id].state)) {
            resp.reject_code = RejectCode::TASK_ALREADY_TERMINAL; return resp;
        }
    }
    deactivateTask(task_id, TaskState::CANCELLED, reason);
    resp.accepted = true; resp.task_id = task_id;
    return resp;
}

// ── Activate a task (move SCHEDULED/PENDING → RUNNING) ───────────────────
void ResourceManager::activateTask(const std::string& task_id) {
    TaskRecord rec;
    {
        std::lock_guard lock(reg_mu_);
        auto it = registry_.find(task_id);
        if (it==registry_.end()) return;
        if (isTerminalState(it->second.state)) return;
        rec = it->second;
        it->second.state = TaskState::PENDING;
    }

    if (rec.allocations.empty()) {
        spdlog::error("activateTask [{}]: no allocations", task_id); return;
    }

    TaskRuntime rt;
    rt.device = nullptr; // set to first device below (used for scan/trigger)

    auto task_err_cb = [this](const std::string& tid, const std::string& err) {
        spdlog::error("IQStreamer error task={}: {}", tid, err);
        deactivateTask(tid, TaskState::FAILED, err);
    };

    const auto& streaming = rec.streaming;

    // Activate every allocation — one per device for coherent multi-board tasks.
    for (auto& alloc : rec.allocations) {
        auto dev_it = devices_.find(alloc.device_id);
        if (dev_it == devices_.end()) {
            deactivateTask(task_id, TaskState::FAILED,
                           "Device not found: " + alloc.device_id); return;
        }
        RadioDevice* dev = dev_it->second.get();
        if (!rt.device) rt.device = dev; // primary device (scan/trigger uses first)

        const bool shared_lo = dev->config().shared_lo;

        if (shared_lo) {
            // One LO, one SoapySDR stream, all channels read from it together.
            if (!dev->tune(alloc.center_freq_hz, alloc.sample_rate_sps)) {
                deactivateTask(task_id, TaskState::FAILED,
                               "Tune failed on " + alloc.device_id); return;
            }
            for (int ch : alloc.rx_channels) dev->setRxGain(ch, 30.0, false);

            SoapySDR::Stream* s = dev->openRxStream(alloc.rx_channels);
            if (!s) {
                deactivateTask(task_id, TaskState::FAILED,
                               "openRxStream failed on " + alloc.device_id); return;
            }
            if (!dev->activateStream(s)) {
                dev->closeStream(s);
                deactivateTask(task_id, TaskState::FAILED,
                               "activateStream failed on " + alloc.device_id); return;
            }
            rt.soapy_streams.push_back({dev, s});

            for (int i=0; i<(int)alloc.rx_channels.size(); ++i) {
                IQStreamer::Config sc;
                sc.task_id       = task_id;
                sc.stream_id     = makeStreamId(task_id,"RX",alloc.device_id,alloc.rx_channels[i]);
                sc.channel_index = alloc.rx_channels[i];
                sc.dest_ip       = streaming.dest_ip;
                sc.dest_port     = alloc.udp_ports[static_cast<size_t>(i)];
                sc.packet_samples= cfg_.policy.iq_packet_samples;
                sc.task_start_ms = rec.start_time_ms;
                sc.sample_rate   = alloc.sample_rate_sps;
                auto streamer = std::make_unique<IQStreamer>(sc, dev->soapyDevice(), s, task_err_cb);
                streamer->updateCenterFreq(alloc.center_freq_hz);
                streamer->start();
                rt.streamers.push_back(std::move(streamer));
            }
        } else {
            // Each channel has its own LO and its own SoapySDR stream.
            for (int i=0; i<(int)alloc.rx_channels.size(); ++i) {
                int ch = alloc.rx_channels[i];
                if (!dev->tuneChannel(ch, alloc.center_freq_hz, alloc.sample_rate_sps)) {
                    deactivateTask(task_id, TaskState::FAILED,
                                   "tuneChannel failed ch="+std::to_string(ch)); return;
                }
                dev->setRxGain(ch, 30.0, false);

                SoapySDR::Stream* s = dev->openRxStream({ch});
                if (!s) {
                    deactivateTask(task_id, TaskState::FAILED,
                                   "openRxStream failed ch="+std::to_string(ch)); return;
                }
                if (!dev->activateStream(s)) {
                    dev->closeStream(s);
                    deactivateTask(task_id, TaskState::FAILED,
                                   "activateStream failed ch="+std::to_string(ch)); return;
                }
                rt.soapy_streams.push_back({dev, s});

                IQStreamer::Config sc;
                sc.task_id       = task_id;
                sc.stream_id     = makeStreamId(task_id,"RX",alloc.device_id,ch);
                sc.channel_index = ch;
                sc.dest_ip       = streaming.dest_ip;
                sc.dest_port     = alloc.udp_ports[static_cast<size_t>(i)];
                sc.packet_samples= cfg_.policy.iq_packet_samples;
                sc.task_start_ms = rec.start_time_ms;
                sc.sample_rate   = alloc.sample_rate_sps;
                auto streamer = std::make_unique<IQStreamer>(sc, dev->soapyDevice(), s, task_err_cb);
                streamer->updateCenterFreq(alloc.center_freq_hz);
                streamer->start();
                rt.streamers.push_back(std::move(streamer));
            }
        }
    }

    // Scan and trigger only apply to single-device tasks (first allocation).
    const auto& alloc0 = rec.allocations[0];
    RadioDevice* dev0   = rt.device;

    if (rec.scan_params && dev0) {
        std::vector<IQStreamer*> ptrs;
        for (auto& s : rt.streamers) ptrs.push_back(s.get());

        ScanExecutor::RetuneFn retune_fn;
        if (dev0->config().shared_lo) {
            retune_fn = [dev0](double cf, double sr) { return dev0->tune(cf, sr); };
        } else {
            auto channels = alloc0.rx_channels;
            retune_fn = [dev0, channels](double cf, double sr) {
                for (int ch : channels)
                    if (!dev0->tuneChannel(ch, cf, sr)) return false;
                return true;
            };
        }
        auto scan_done = [this](const std::string& tid, bool ok) {
            deactivateTask(tid, ok ? TaskState::COMPLETED : TaskState::FAILED,
                           ok ? "Scan complete" : "Scan error");
        };
        rt.scan_exec = std::make_unique<ScanExecutor>(
            task_id, *rec.scan_params, std::move(retune_fn), ptrs, scan_done);
        rt.scan_exec->start();
    }

    if (rec.trigger_params && !rt.streamers.empty() && !rt.soapy_streams.empty() && dev0) {
        auto trig_done = [this](const std::string& tid, bool ok) {
            deactivateTask(tid, ok ? TaskState::COMPLETED : TaskState::FAILED,
                           ok ? "Trigger done" : "Trigger error");
        };
        rt.trig_mon = std::make_unique<TriggerMonitor>(
            task_id, *rec.trigger_params, dev0->soapyDevice(),
            rt.soapy_streams[0].second, rt.streamers[0].get(),
            alloc0.sample_rate_sps, trig_done);
        rt.trig_mon->start();
    }

    {
        std::lock_guard lock(rt_mu_);
        runtimes_[task_id] = std::move(rt);
    }
    {
        std::lock_guard lock(reg_mu_);
        if (registry_.count(task_id))
            registry_[task_id].state = TaskState::RUNNING;
    }

    std::string dev_list;
    for (auto& a : rec.allocations) dev_list += a.device_id + " ";
    spdlog::info("activateTask [{}] RUNNING on [{}]", task_id, dev_list);
    notifyStateChange(task_id);
}

// ── Deactivate ───────────────────────────────────────────────────────────
void ResourceManager::deactivateTask(const std::string& task_id,
                                      TaskState terminal_state,
                                      const std::string& reason) {
    spdlog::info("deactivateTask [{}] → {} reason={}",
                 task_id, taskStateToString(terminal_state), reason);

    // Stop runtime objects
    {
        std::lock_guard lock(rt_mu_);
        auto it = runtimes_.find(task_id);
        if (it != runtimes_.end()) {
            auto& rt = it->second;
            if (rt.scan_exec) rt.scan_exec->stop();
            if (rt.trig_mon)  rt.trig_mon->stop();
            for (auto& s : rt.streamers) s->stop();
            for (auto& [d, s] : rt.soapy_streams) {
                if (d && s) { d->deactivateStream(s); d->closeStream(s); }
            }
            runtimes_.erase(it);
        }
    }

    // Release timeline slots and ports
    {
        std::lock_guard lock(reg_mu_);
        auto it = registry_.find(task_id);
        if (it != registry_.end()) {
            auto& rec = it->second;
            for (auto& alloc : rec.allocations) {
                if (timelines_.count(alloc.device_id))
                    timelines_[alloc.device_id]->remove(task_id);
                port_pool_->releaseAll(alloc.udp_ports);
            }
            rec.state           = terminal_state;
            rec.terminal_reason = reason;
        }
    }
    notifyStateChange(task_id);
}

// ── Scheduler / Watchdog ticks ───────────────────────────────────────────
void ResourceManager::schedulerTick() {
    int64_t now = nowMs();
    std::vector<std::string> to_activate;
    {
        std::lock_guard lock(reg_mu_);
        for (auto& [id, rec] : registry_)
            if (rec.state == TaskState::SCHEDULED && rec.start_time_ms <= now)
                to_activate.push_back(id);
    }
    for (auto& id : to_activate) {
        spdlog::info("schedulerTick: activating {}", id);
        activateTask(id);
    }
}

void ResourceManager::watchdogTick() {
    int64_t now = nowMs();
    std::vector<std::string> to_expire;
    {
        std::lock_guard lock(reg_mu_);
        for (auto& [id, rec] : registry_)
            if (rec.state == TaskState::RUNNING &&
                rec.stop_time_ms != TIME_INFINITE &&
                rec.stop_time_ms <= now)
                to_expire.push_back(id);
    }
    for (auto& id : to_expire) {
        spdlog::info("watchdogTick: expiring {}", id);
        deactivateTask(id, TaskState::COMPLETED, "stop_time reached");
    }
}

// ── Queries ──────────────────────────────────────────────────────────────
std::vector<TaskRecord> ResourceManager::getActiveTasks() const {
    std::lock_guard lock(reg_mu_);
    std::vector<TaskRecord> out;
    for (auto& [id,rec] : registry_)
        if (!isTerminalState(rec.state)) out.push_back(rec);
    return out;
}

int ResourceManager::countByState(TaskState s) const {
    std::lock_guard lock(reg_mu_);
    int n=0;
    for (auto& [id,rec] : registry_) if (rec.state==s) ++n;
    return n;
}

std::vector<std::string> ResourceManager::deviceIds() const {
    std::vector<std::string> ids;
    for (auto& [id,_] : devices_) ids.push_back(id);
    return ids;
}

RadioDevice* ResourceManager::getDevice(const std::string& id) {
    auto it = devices_.find(id);
    return it!=devices_.end() ? it->second.get() : nullptr;
}

std::vector<ResourceManager::DeviceSummary> ResourceManager::deviceSummaries() const {
    std::vector<DeviceSummary> out;
    int64_t now = nowMs();
    for (auto& [id, dev] : devices_) {
        DeviceSummary s;
        auto st = dev->getStatus();
        s.device_id       = id;
        s.driver          = st.driver;
        s.uri             = st.uri;
        s.coherency_group = st.coherency_group;
        s.online          = st.online;
        s.cf_hz           = st.center_freq_hz;
        s.rate_sps        = st.sample_rate;
        s.temp_c          = st.temperature_c;
        if (timelines_.count(id)) {
            auto& tl = timelines_.at(id);
            s.alloc_bw_hz = tl->allocatedBw(now);
            double usable = st.sample_rate * cfg_.policy.usable_bw_fraction;
            s.free_bw_hz  = std::max(0.0, usable - s.alloc_bw_hz);
            s.active_tasks= tl->usedRx(now);
        }
        out.push_back(s);
    }
    return out;
}

// ── Helpers ──────────────────────────────────────────────────────────────
void ResourceManager::notifyStateChange(const std::string& task_id) {
    if (!on_state_change_) return;
    std::lock_guard lock(reg_mu_);
    auto it = registry_.find(task_id);
    if (it != registry_.end()) on_state_change_(it->second);
}

std::string ResourceManager::makeStreamId(const std::string& task_id,
                                           const std::string& type,
                                           const std::string& dev_id,
                                           int ch_idx) const {
    return task_id + "-" + type + "-" + dev_id + "-" + std::to_string(ch_idx);
}

} // namespace sdr
