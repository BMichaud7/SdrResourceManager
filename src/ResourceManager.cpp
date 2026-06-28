/*
========================================================================
Project: OpenRFStack
Author:  Brendan Michaud
Year:    2026
Part of OpenRFStack (https://github.com/OpenRFStack)

Licensed under the Personal Use License.
Do not use for commercial, organizational, or military purposes.
Contact author for permission: https://github.com/OpenRFStack
========================================================================
*/
#include "ResourceManager.hpp"
#include "sdr/MessageCodec.hpp"
#include <spdlog/spdlog.h>
#include <uuid/uuid.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <cmath>
#include <thread>

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

    // A detached activateTask() thread may have already passed its
    // terminal-state check (above) and be mid-flight opening hardware /
    // starting an IQStreamer when this call raced ahead of it. Wait for any
    // such in-flight activations to finish before touching the devices —
    // otherwise dev->close() (and the eventual device destruction) can run
    // concurrently with that thread's SoapySDR calls and IQStreamer worker.
    //
    // While waiting, keep re-running deactivation for any task that currently
    // has a live runtime: an in-flight activateTask() may itself be blocked on
    // hw_activation_mu_, held by another task's orphaned runtime (one whose
    // activateTask() raced past the terminal-state checks and inserted a
    // runtime, with hw_lock still held, AFTER our pass(es) above already found
    // it terminal with no runtime to clean up). deactivateTask's "orphaned
    // runtime" path releases that runtime's hw_lock, unblocking the in-flight
    // activation so activations_in_flight_ can reach zero.
    //
    // We must scan runtimes_ directly rather than the active_ids snapshot
    // above: a task can already be terminal (and absent from active_ids) at
    // snapshot time yet still have its activateTask() thread in flight,
    // inserting an orphaned runtime later. Without this, the wait below can
    // deadlock.
    auto deactivateOrphanedRuntimes = [this]() {
        std::vector<std::string> ids;
        {
            std::lock_guard lock(rt_mu_);
            for (auto& [id, rt] : runtimes_) ids.push_back(id);
        }
        for (auto& id : ids)
            deactivateTask(id, TaskState::CANCELLED, "controller shutdown");
    };

    while (activations_in_flight_.load(std::memory_order_relaxed) > 0) {
        deactivateOrphanedRuntimes();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Final pass for any runtime inserted just as the last in-flight
    // activation finished.
    deactivateOrphanedRuntimes();

    for (auto& [id, dev] : devices_) dev->close();
}

// ── Device selection ─────────────────────────────────────────────────────
std::optional<ResourceManager::DeviceCandidate>
ResourceManager::findBestDevice(double cf, double bw, double sr,
                                 int rx, int tx,
                                 int64_t t_start, int64_t t_stop,
                                 const std::string& preferred,
                                 int preferred_channel) const
{
    // Sort devices: preferred first, then by free BW descending
    std::vector<std::string> ordered;
    if (!preferred.empty() && devices_.count(preferred))
        ordered.push_back(preferred);
    for (auto& [id, _] : devices_)
        if (id != preferred) ordered.push_back(id);

    // Sort non-preferred by free BW (preferred, if present, stays first).
    // Snapshot each device's allocated bandwidth once up front: reading it
    // live inside the comparator would let a concurrent insert()/remove()
    // on another thread change the value mid-sort, violating stable_sort's
    // comparator-consistency precondition (and causing UB in libstdc++'s
    // unguarded insertion sort).
    if (ordered.size() > 1) {
        const int64_t now = nowMs();
        std::unordered_map<std::string, double> alloc_bw;
        alloc_bw.reserve(ordered.size());
        for (auto& id : ordered)
            alloc_bw[id] = timelines_.at(id)->allocatedBw(now);

        std::stable_sort(ordered.begin(), ordered.end(),
                         [&](const std::string& a, const std::string& b) {
                             if (a == preferred || b == preferred)
                                 return a == preferred; // preferred sorts first
                             return alloc_bw.at(a) < alloc_bw.at(b);   // less allocated = more free
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

        // Below the device's hardware floor (e.g. RTL-SDR rejects <225,001 Hz)?
        // Acquire at the smallest integer multiple of the requested rate that
        // clears the floor — activateTask then applies Ddc decimation so the
        // client still gets exactly the rate it asked for. Skip this device
        // if no such multiple fits under sample_rate_max_sps.
        double dev_sr = sr;
        if (caps.sample_rate_min_sps > 0.0 && sr > 0.0 && sr < caps.sample_rate_min_sps) {
            double decim_f = std::ceil(caps.sample_rate_min_sps / sr);
            if (decim_f > 1e6) continue;  // unreasonably large decimation ratio — skip
            int decim = std::max(1, (int)decim_f);
            dev_sr = sr * decim;
            if (caps.sample_rate_max_sps > 0.0 && dev_sr > caps.sample_rate_max_sps) continue;
        }

        auto fit = timelines_.at(id)->canFit(
            t_start, t_stop, cf, bw, dev_sr, rx, tx,
            caps.rx_channels, caps.tx_channels,
            cfg_.policy.guard_band_hz,
            dev->config().shared_lo,
            preferred_channel);

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
        rec.rank           = req.rank;
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
        // Run activateTask in a background thread so the proton AMQP thread
        // is not blocked by SoapySDR hardware I/O (openRxStream can take seconds).
        if (activate_now) {
            activations_in_flight_.fetch_add(1, std::memory_order_relaxed);
            std::thread([this, task_id]() {
                activateTask(task_id);
                activations_in_flight_.fetch_sub(1, std::memory_order_relaxed);
            }).detach();
        }
        notifyStateChange(task_id);
        return resp;
    }

    // ── Single-device path ───────────────────────────────────────────────────
    const int pref_ch = req.rf.preferred_channel; // -1 = any; ≥0 = specific
    auto candidate = findBestDevice(
        req.rf.center_freq_hz, req.rf.bandwidth_hz, req.rf.sample_rate_sps,
        req.rf.rx_count, req.rf.tx_count, t_start, t_stop,
        req.rf.preferred_device, pref_ch);

    if (!candidate && req.rank > 0 && tryPreemptConflicting(req, t_start, t_stop)) {
        candidate = findBestDevice(
            req.rf.center_freq_hz, req.rf.bandwidth_hz, req.rf.sample_rate_sps,
            req.rf.rx_count, req.rf.tx_count, t_start, t_stop,
            req.rf.preferred_device, pref_ch);
    }

    // Try expanding an existing shared-LO window to fit both tasks.
    // When preferred_channel is set, also runs for same-CF subscriptions (the
    // same-CF guard is bypassed inside tryRetuneCombined for this case).
    if (!candidate)
        candidate = tryRetuneCombined(req, t_start, t_stop);

    if (!candidate) {
        resp.reject_code   = RejectCode::NO_DEVICE_AVAILABLE;
        resp.reject_reason = "No device satisfies constraints";
        return resp;
    }

    // Port allocation — honour client-provided ports (dest_ports[]) when present.
    // A client that pre-binds its socket includes the bound port in dest_ports[]
    // so the controller can stream to it immediately (eliminates the AMQP race).
    int n_ports = req.rf.rx_count + req.rf.tx_count;
    std::vector<int> ports;
    if (!req.streaming.dest_ports.empty()) {
        ports = req.streaming.dest_ports;
        // Pad with pool ports for any channels the client didn't cover.
        while ((int)ports.size() < n_ports) {
            auto extra = port_pool_->allocateN(1);
            if (extra.empty()) {
                resp.reject_code   = RejectCode::PORT_POOL_EXHAUSTED;
                resp.reject_reason = "UDP port pool exhausted"; return resp;
            }
            ports.push_back(extra[0]);
        }
    } else {
        ports = port_pool_->allocateN(n_ports);
        if ((int)ports.size() < n_ports) {
            resp.reject_code   = RejectCode::PORT_POOL_EXHAUSTED;
            resp.reject_reason = "UDP port pool exhausted"; return resp;
        }
    }

    std::string task_id = genUuid();
    resp.task_id        = task_id;
    resp.actual_start_ms= t_start;
    resp.actual_stop_ms = t_stop;
    resp.accepted       = true;

    // Build allocation record
    TaskRecord::DeviceAllocation alloc;
    alloc.device_id              = candidate->device->id();
    alloc.rx_channels            = candidate->fit.avail_rx;
    alloc.tx_channels            = candidate->fit.avail_tx;
    alloc.center_freq_hz         = candidate->fit.device_cf;
    alloc.sample_rate_sps        = candidate->fit.device_rate;
    // Store the task's requested SR; activateTask uses it to decide whether DDC applies.
    // Non-zero only when the device runs at a wider rate (combined-window path).
    alloc.output_sample_rate_sps = (std::abs(candidate->fit.device_rate - req.rf.sample_rate_sps) > 1.0)
                                   ? req.rf.sample_rate_sps : 0.0;
    alloc.slice_lo_hz            = candidate->fit.placed_lo;
    alloc.slice_hi_hz            = candidate->fit.placed_hi;

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
    // Run activateTask in a background thread so the proton AMQP thread
    // is not blocked by SoapySDR hardware I/O (openRxStream can take seconds).
    if (activate_now) {
        activations_in_flight_.fetch_add(1, std::memory_order_relaxed);
        std::thread([this, task_id]() {
            activateTask(task_id);
            activations_in_flight_.fetch_sub(1, std::memory_order_relaxed);
        }).detach();
    }
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
                e.sample_rate_sps <= 0 || e.dwell_ms < 5) {
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

    // Serialize tune+open+activate against concurrent activateTask() threads on
    // the same device.  hw_activation_mu_ is released before the blocking
    // readStream loop and re-acquired for teardown.
    SoapySDR::Stream* stream = nullptr;
    {
        std::unique_lock<std::mutex> hw_lock(hw_activation_mu_);
        bool tune_ok = dev->config().shared_lo
                       ? dev->tune(sp.center_freq_hz, sp.sample_rate_sps)
                       : dev->tuneChannel(snap_ch, sp.center_freq_hz, sp.sample_rate_sps);
        if (!tune_ok) {
            resp.reject_code = RejectCode::INTERNAL_ERROR;
            resp.reject_reason = "Tune failed"; return resp;
        }
        std::vector<int> chans = {snap_ch};
        stream = dev->openRxStream(chans);
        if (!stream) {
            resp.reject_code = RejectCode::INTERNAL_ERROR;
            resp.reject_reason = "Stream open failed"; return resp;
        }
        if (!dev->activateStream(stream)) {
            dev->closeStream(stream);
            resp.reject_code = RejectCode::INTERNAL_ERROR;
            resp.reject_reason = "Stream activate failed"; return resp;
        }
    } // release hw_lock — readStream must not hold it (blocks for up to 1s)

    // Validate before multiplying — client-supplied values can overflow int32.
    if (sp.fft_size <= 0 || sp.n_averages <= 0 ||
        (int64_t)sp.fft_size * sp.n_averages > 1 << 24) {
        resp.reject_code   = RejectCode::INVALID_REQUEST;
        resp.reject_reason = "snapshot fft_size/n_averages out of range";
        dev->deactivateStream(stream);
        dev->closeStream(stream);
        return resp;
    }
    int total = sp.fft_size * sp.n_averages;
    std::vector<float> buf((size_t)total * 2);
    void* bufs[1] = {buf.data()};
    int flags=0; long long hw_ts=0;
    int collected = 0;
    int err_count = 0;
    while (collected < total) {
        bufs[0] = buf.data() + (size_t)collected * 2;
        int rem = total - collected;
        int ret = dev->readStream(stream, bufs, (size_t)rem, flags, hw_ts, 1'000'000LL);
        if (ret > 0) {
            collected += ret;
            err_count = 0;
        } else if (ret != SOAPY_SDR_TIMEOUT) {
            if (++err_count >= 5) break;
        }
    }

    {
        std::unique_lock<std::mutex> hw_lock(hw_activation_mu_);
        dev->deactivateStream(stream);
        dev->closeStream(stream);
    }

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
    rec.priority     = req.priority;
    rec.rank         = req.rank;
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

    // Run activateTask in a background thread so the proton AMQP thread
    // is not blocked by SoapySDR hardware I/O (openRxStream can take seconds).
    activations_in_flight_.fetch_add(1, std::memory_order_relaxed);
    std::thread([this, task_id]() {
        activateTask(task_id);
        activations_in_flight_.fetch_sub(1, std::memory_order_relaxed);
    }).detach();
    notifyStateChange(task_id);
    return resp;
}

// ── Rank-based preemption ────────────────────────────────────────────────
bool ResourceManager::tryPreemptConflicting(const TaskRequest& req,
                                             int64_t t_start, int64_t t_stop)
{
    const std::string reason = std::string(PREEMPT_TERMINAL_REASON) +
                               " rank=" + std::to_string(req.rank) +
                               " request=" + req.request_id;

    for (auto& [dev_id, tl] : timelines_) {
        auto& dev = devices_.at(dev_id);
        if (!dev->isOnline()) continue;

        const auto& caps = dev->config().caps;
        if (req.rf.center_freq_hz < caps.freq_min_hz ||
            req.rf.center_freq_hz > caps.freq_max_hz) continue;
        if (req.rf.bandwidth_hz    > caps.bandwidth_max_hz)    continue;
        if (req.rf.sample_rate_sps > caps.sample_rate_max_sps) continue;
        if (req.rf.rx_count        > caps.rx_channels)         continue;
        if (req.rf.tx_count        > caps.tx_channels)         continue;

        auto overlapping = tl->slotsOverlapping(t_start, t_stop);
        if (overlapping.empty()) continue;

        std::vector<std::string> to_preempt;
        bool can_preempt = true;
        {
            std::lock_guard lock(reg_mu_);
            for (auto& slot : overlapping) {
                auto it = registry_.find(slot.task_id);
                if (it == registry_.end()) continue;
                if (isTerminalState(it->second.state)) continue;
                if (it->second.rank >= req.rank) { can_preempt = false; break; }
                to_preempt.push_back(slot.task_id);
            }
        }

        if (can_preempt && !to_preempt.empty()) {
            for (auto& tid : to_preempt) {
                spdlog::info("ResourceManager: preempting {} for rank={} request={}",
                             tid, req.rank, req.request_id);
                deactivateTask(tid, TaskState::CANCELLED, reason);
            }
            return true;
        }
    }
    return false;
}

// ── Combined-window retune ───────────────────────────────────────────────
// When a new task at a different CF can't fit the current device window,
// try to expand the window to cover both the existing and new slices.
// The device is physically retuned, all running streamers are paused for
// PLL settle, and existing timeline slots are updated to the new CF/SR.
std::optional<ResourceManager::DeviceCandidate>
ResourceManager::tryRetuneCombined(const TaskRequest& req,
                                    int64_t t_start, int64_t t_stop)
{
    const double guard  = cfg_.policy.guard_band_hz;
    // Settle window: drain 2 packets' worth of samples after retune
    const int    settle = cfg_.policy.iq_packet_samples * 2;

    for (auto& [dev_id, tl] : timelines_) {
        auto& dev = devices_.at(dev_id);
        if (!dev->isOnline()) continue;
        if (!dev->config().shared_lo) continue;

        const auto& caps = dev->config().caps;
        if (req.rf.center_freq_hz < caps.freq_min_hz ||
            req.rf.center_freq_hz > caps.freq_max_hz) continue;
        if (req.rf.bandwidth_hz > caps.bandwidth_max_hz) continue;
        if (req.rf.rx_count     > caps.rx_channels)     continue;

        // Determine whether the new CF matches the existing device window.
        const int pref_ch = req.rf.preferred_channel;
        auto slots_cf = tl->slotsOverlapping(t_start, t_stop);
        bool same_cf = !slots_cf.empty() &&
            std::all_of(slots_cf.begin(), slots_cf.end(),
                [&](const TimeFreqSlot& s){
                    return std::abs(s.center_freq_hz - req.rf.center_freq_hz) < 1.0;
                });

        // Normally skip when same CF — canFit already handles slice packing.
        // Exception: preferred_channel is set AND the existing tasks use that
        // channel at the same CF/SR.  In that case we take a direct "fan-out
        // subscribe" path: the new task joins the existing stream without any
        // hardware retune.
        if (same_cf) {
            if (pref_ch < 0) continue; // normal guard

            // Same-CF fan-out fast path.
            // Verify the requested channel exists in the overlapping slots.
            bool ch_found = false;
            for (auto& slot : slots_cf) {
                if (std::find(slot.rx_channels.begin(), slot.rx_channels.end(), pref_ch)
                        != slot.rx_channels.end()) {
                    ch_found = true;
                    break;
                }
            }
            if (!ch_found) continue;

            // Verify the new slice fits inside the existing hardware window.
            double dev_cf = slots_cf[0].center_freq_hz;
            double dev_sr = slots_cf[0].sample_rate_sps;
            double win_lo = dev_cf - dev_sr / 2.0;
            double win_hi = dev_cf + dev_sr / 2.0;
            double req_lo = req.rf.center_freq_hz - req.rf.bandwidth_hz / 2.0;
            double req_hi = req.rf.center_freq_hz + req.rf.bandwidth_hz / 2.0;
            if (req_lo < win_lo || req_hi > win_hi) continue;

            // Rank guard
            {
                bool rank_ok = true;
                std::lock_guard lock(reg_mu_);
                for (auto& slot : slots_cf) {
                    auto it = registry_.find(slot.task_id);
                    if (it == registry_.end()) continue;
                    if (!isTerminalState(it->second.state) && req.rank < it->second.rank)
                        { rank_ok = false; break; }
                }
                if (!rank_ok) continue;
            }

            spdlog::info("ResourceManager: same-CF subscribe on {} ch={} cf={:.3f}MHz "
                         "sr={:.3f}MSPS (no hardware change)",
                         dev_id, pref_ch, dev_cf / 1e6, dev_sr / 1e6);

            FitResult fit;
            fit.ok          = true;
            fit.device_cf   = dev_cf;
            fit.device_rate = dev_sr;
            fit.placed_lo   = req_lo;
            fit.placed_hi   = req_hi;
            fit.avail_rx    = {pref_ch};
            return DeviceCandidate{dev.get(), std::move(fit)};
        }

        // Different-CF path: use canCombine to compute the widened window.
        auto cr = tl->canCombine(t_start, t_stop,
                                  req.rf.center_freq_hz, req.rf.bandwidth_hz,
                                  req.rf.rx_count, caps.rx_channels,
                                  guard, caps.sample_rate_max_sps,
                                  true /* reuse_channels — multicast from existing channel */);
        if (!cr.ok) {
            spdlog::debug("tryRetuneCombined: {} not combinable: {}", dev_id, cr.reject_reason);
            continue;
        }

        // If a specific channel is required, verify the existing tasks use it and
        // filter avail_rx to that channel only.
        if (pref_ch >= 0) {
            bool found = std::find(cr.avail_rx.begin(), cr.avail_rx.end(), pref_ch)
                         != cr.avail_rx.end();
            if (!found) {
                spdlog::debug("tryRetuneCombined: {} has no preferred_channel={}", dev_id, pref_ch);
                continue;
            }
            cr.avail_rx = {pref_ch};
        }

        // Rank guard: don't let a lower-rank task subscribe to a higher-rank stream
        {
            auto slots_chk = tl->slotsOverlapping(t_start, t_stop);
            bool rank_ok = true;
            std::lock_guard lock(reg_mu_);
            for (auto& slot : slots_chk) {
                auto it = registry_.find(slot.task_id);
                if (it == registry_.end()) continue;
                if (!isTerminalState(it->second.state) && req.rank < it->second.rank) {
                    rank_ok = false;
                    break;
                }
            }
            if (!rank_ok) continue;
        }

        // Check whether the hardware actually needs to change.
        // When canCombine returns the same CF/SR as the current device (edge case
        // where the combined window happens to match), we skip the retune and settle
        // drain to avoid an unnecessary glitch on existing streams.
        auto slots = tl->slotsOverlapping(t_start, t_stop);
        const bool hw_changed = !slots.empty() &&
            (std::abs(cr.combined_cf - slots[0].center_freq_hz) > 1.0 ||
             std::abs(cr.combined_sr - slots[0].sample_rate_sps) > 1.0);

        if (hw_changed) {
            // Serialize the tune + timeline update against concurrent activateTask()
            // threads that may be calling openRxStream on the same device.
            // Released before the rt_mu_ block to preserve lock ordering:
            //   hw_activation_mu_ must never be acquired while rt_mu_ is held.
            {
                std::unique_lock<std::mutex> hw_lock(hw_activation_mu_);
                if (!dev->tune(cr.combined_cf, cr.combined_sr)) {
                    spdlog::warn("tryRetuneCombined: tune failed on {}", dev_id);
                    continue;
                }
                // Update timeline slots to reflect new device CF/SR
                tl->updateDeviceTune(cr.combined_cf, cr.combined_sr);
            } // release hw_lock before acquiring rt_mu_

            // Pause all running streamers on this device for PLL settle,
            // then push the new CF/SR into the packet headers
            {
                std::lock_guard lock(rt_mu_);
                for (auto& slot : slots) {
                    auto it = runtimes_.find(slot.task_id);
                    if (it == runtimes_.end()) continue;
                    for (auto& streamer : it->second.streamers) {
                        streamer->pauseForRetune(settle);
                        streamer->updateCenterFreq(cr.combined_cf);
                        streamer->updateSampleRate(cr.combined_sr);
                    }
                }
            }
        }

        // Upgrade existing raw fan-out consumers to DDC sub-bands.
        // When the hardware widens to cover a new task, any task previously receiving
        // the unfiltered stream must be converted so it continues to see only its own
        // requested band — not the now-wider wideband capture.
        //
        // Step A: collect raw consumers (under rt_mu_)
        struct UpgradeCand {
            std::string                task_id;
            std::shared_ptr<IQStreamer> streamer;
        };
        std::vector<UpgradeCand> upgrade_cands;
        {
            std::lock_guard lk_rt(rt_mu_);
            for (auto& slot : slots) {
                auto rt_it = runtimes_.find(slot.task_id);
                if (rt_it == runtimes_.end()) continue;
                auto& rt = rt_it->second;
                if (rt.is_subband_consumer) continue; // already DDC — skip
                if (rt.streamers.empty()) continue;
                upgrade_cands.push_back({slot.task_id, rt.streamers[0]});
            }
        }

        // Step B: build upgrade plans — read alloc data BEFORE the registry update
        // below overwrites sample_rate_sps with cr.combined_sr.
        struct UpgradePlan {
            std::string                task_id;
            std::string                stream_id;
            std::string                dest_ip;
            int                        dest_port;
            int                        channel_index;
            double                     task_cf;
            double                     output_sr;
            std::shared_ptr<IQStreamer> streamer;
        };
        std::vector<UpgradePlan> upgrade_plans;
        {
            std::lock_guard lk_reg(reg_mu_);
            for (auto& cand : upgrade_cands) {
                auto it = registry_.find(cand.task_id);
                if (it == registry_.end()) continue;
                auto& rec = it->second;
                if (isTerminalState(rec.state)) continue;
                if (rec.allocations.empty()) continue;
                auto& alloc = rec.allocations[0];
                if (alloc.device_id != dev_id) continue;
                if (alloc.rx_channels.empty() || alloc.udp_ports.empty()) continue;

                // Task's original requested SR (before the update below clobbers it)
                double output_sr = alloc.output_sample_rate_sps > 0.0
                                   ? alloc.output_sample_rate_sps : alloc.sample_rate_sps;
                double task_cf   = (alloc.slice_lo_hz + alloc.slice_hi_hz) / 2.0;

                double decim_f = cr.combined_sr / output_sr;
                int    decim   = (int)std::round(decim_f);
                if (decim < 2 || std::abs(decim_f - decim) / decim >= 1e-3) continue;

                UpgradePlan plan;
                plan.task_id      = cand.task_id;
                plan.stream_id    = makeStreamId(cand.task_id, "RX",
                                                 alloc.device_id, alloc.rx_channels[0]);
                plan.dest_ip      = rec.streaming.dest_ip;
                plan.dest_port    = alloc.udp_ports[0];
                plan.channel_index= alloc.rx_channels[0];
                plan.task_cf      = task_cf;
                plan.output_sr    = output_sr;
                plan.streamer     = cand.streamer;

                // Record the output SR so deactivateTask and any future retune can find it
                alloc.output_sample_rate_sps = output_sr;

                upgrade_plans.push_back(std::move(plan));
            }
        }

        // Step C: perform conversion — streamer methods are internally lock-safe
        for (auto& plan : upgrade_plans) {
            plan.streamer->removeDest(plan.task_id);
            plan.streamer->addSubBand(
                plan.task_id, plan.stream_id, plan.channel_index,
                plan.dest_ip, plan.dest_port,
                plan.task_cf, plan.output_sr, cr.combined_sr);
            spdlog::info("tryRetuneCombined: upgraded task {} → DDC sub-band "
                         "cf={:.3f}MHz sr={:.3f}MSPS",
                         plan.task_id, plan.task_cf / 1e6, plan.output_sr / 1e6);
        }

        // Step D: update is_subband_consumer flags (under rt_mu_)
        if (!upgrade_plans.empty()) {
            std::lock_guard lk_rt(rt_mu_);
            for (auto& plan : upgrade_plans) {
                auto rt_it = runtimes_.find(plan.task_id);
                if (rt_it != runtimes_.end())
                    rt_it->second.is_subband_consumer = true;
            }
        }

        // Update registry allocations and notify affected tasks
        std::vector<std::string> to_notify;
        {
            std::lock_guard lock(reg_mu_);
            for (auto& slot : slots) {
                auto it = registry_.find(slot.task_id);
                if (it == registry_.end()) continue;
                for (auto& alloc : it->second.allocations) {
                    if (alloc.device_id == dev_id) {
                        alloc.center_freq_hz  = cr.combined_cf;
                        alloc.sample_rate_sps = cr.combined_sr;
                    }
                }
                to_notify.push_back(slot.task_id);
            }
        }
        for (auto& tid : to_notify)
            notifyStateChange(tid);

        spdlog::info("ResourceManager: combined {} on {} → cf={:.3f}MHz sr={:.3f}MSPS "
                     "(covering {} existing task(s))",
                     hw_changed ? "retune" : "subscribe",
                     dev_id, cr.combined_cf / 1e6, cr.combined_sr / 1e6, (int)slots.size());

        FitResult fit;
        fit.ok          = true;
        fit.device_cf   = cr.combined_cf;
        fit.device_rate = cr.combined_sr;
        fit.placed_lo   = cr.new_slice_lo;
        fit.placed_hi   = cr.new_slice_hi;
        fit.avail_rx    = cr.avail_rx;
        return DeviceCandidate{dev.get(), std::move(fit)};
    }
    return std::nullopt;
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

    // Serialize hardware open calls (tune/openRxStream/activateStream) across
    // background threads — only one task at a time may set up a SoapySDR
    // stream. Released below once per-allocation hardware setup completes;
    // NOT held for the task's RUNNING lifetime (that would serialize
    // independent-LO channels on the same device against each other).
    std::unique_lock<std::mutex> hw_lock(hw_activation_mu_);

    // Check state again after acquiring hw_lock: task may have been cancelled while waiting.
    {
        std::lock_guard lock(reg_mu_);
        auto it = registry_.find(task_id);
        if (it == registry_.end() || isTerminalState(it->second.state)) return;
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
            hw_lock.unlock();
            deactivateTask(task_id, TaskState::FAILED,
                           "Device not found: " + alloc.device_id); return;
        }
        RadioDevice* dev = dev_it->second.get();
        if (!rt.device) rt.device = dev; // primary device (scan/trigger uses first)

        const bool shared_lo = dev->config().shared_lo;

        if (shared_lo) {
            // Check whether all requested channels are already live (combined-window
            // multicast). If so, subscribe to the existing IQStreamer(s) instead of
            // opening new hardware.
            std::vector<std::shared_ptr<IQStreamer>> borrowed;
            std::vector<std::pair<RadioDevice*, SoapySDR::Stream*>> borrowed_hw;
            {
                std::lock_guard lk(rt_mu_);
                for (int ch : alloc.rx_channels) {
                    std::string key = alloc.device_id + ":" + std::to_string(ch);
                    auto it = channel_states_.find(key);
                    if (it != channel_states_.end()) {
                        borrowed.push_back(it->second.streamer);
                        borrowed_hw.push_back({it->second.device, it->second.soapy_stream});
                    }
                }
            }

            if (borrowed.size() == alloc.rx_channels.size()) {
                // All channels shared — subscribe to the live streamer.
                // If the task requested a narrower band (DDC path), apply DDC; otherwise
                // fan-out the raw wideband stream via addDest.
                // Read wideband SR directly from the IQStreamer's atomic — avoids
                // adding cf/sr to ChannelState which would change struct size and timing.
                double wideband_sr = borrowed.empty() ? 0.0 : borrowed[0]->currentSR();
                double output_sr = alloc.output_sample_rate_sps > 0.0
                                   ? alloc.output_sample_rate_sps : alloc.sample_rate_sps;
                bool needs_ddc = false;
                if (wideband_sr > 0.0 && std::abs(output_sr - wideband_sr) > 1.0) {
                    double decim_f = wideband_sr / output_sr;
                    int    decim   = (int)std::round(decim_f);
                    // Only apply DDC when decimation ratio is an integer (within 0.1%).
                    // Non-integer ratios fall back to raw wideband fan-out (addDest).
                    needs_ddc = (decim >= 2 && std::abs(decim_f - decim) / decim < 1e-3);
                }

                if (needs_ddc && alloc.rx_channels.size() == 1) {
                    // Sub-band DDC: mix + filter + decimate to task's requested band
                    double task_cf = (alloc.slice_lo_hz + alloc.slice_hi_hz) / 2.0;
                    borrowed[0]->addSubBand(
                        task_id,
                        makeStreamId(task_id, "RX", alloc.device_id, alloc.rx_channels[0]),
                        alloc.rx_channels[0],
                        streaming.dest_ip, alloc.udp_ports[0],
                        task_cf, output_sr, wideband_sr);
                    rt.streamers.push_back(borrowed[0]);
                    rt.soapy_streams.push_back(borrowed_hw[0]);
                    rt.is_subband_consumer = true;
                    spdlog::info("activateTask [{}] DDC sub-band on {} cf={:.3f}MHz sr={:.3f}MSPS"
                                 " (wideband {:.3f}MSPS)",
                                 task_id, alloc.device_id,
                                 task_cf / 1e6, output_sr / 1e6, wideband_sr / 1e6);
                } else {
                    // Raw fan-out: same CF/SR, or multi-channel (no DDC)
                    for (int i = 0; i < (int)alloc.rx_channels.size(); ++i) {
                        borrowed[i]->addDest(
                            task_id,
                            makeStreamId(task_id, "RX", alloc.device_id, alloc.rx_channels[i]),
                            streaming.dest_ip,
                            alloc.udp_ports[static_cast<size_t>(i)]);
                        rt.streamers.push_back(borrowed[i]);
                        rt.soapy_streams.push_back(borrowed_hw[i]);
                    }
                    spdlog::info("activateTask [{}] subscribed to shared channel(s) on {}",
                                 task_id, alloc.device_id);
                }
            } else {
                // New channel(s) — tune device, open stream, create IQStreamer(s)
                if (!dev->tune(alloc.center_freq_hz, alloc.sample_rate_sps)) {
                    hw_lock.unlock();
                    deactivateTask(task_id, TaskState::FAILED,
                                   "Tune failed on " + alloc.device_id); return;
                }
                for (int ch : alloc.rx_channels)
                    dev->setRxGain(ch, dev->config().rx_gain_db, dev->config().rx_agc);

                SoapySDR::Stream* s = dev->openRxStream(alloc.rx_channels);
                if (!s) {
                    hw_lock.unlock();
                    deactivateTask(task_id, TaskState::FAILED,
                                   "openRxStream failed on " + alloc.device_id); return;
                }
                if (!dev->activateStream(s)) {
                    dev->closeStream(s);
                    hw_lock.unlock();
                    deactivateTask(task_id, TaskState::FAILED,
                                   "activateStream failed on " + alloc.device_id); return;
                }
                rt.soapy_streams.push_back({dev, s});

                // ONE IQStreamer owns the SoapySDR stream and reads all channels.
                // Extra channels are demuxed inside workerLoop — avoids concurrent
                // readStream calls from multiple threads on the same stream object.
                IQStreamer::Config sc;
                sc.task_id       = task_id;
                sc.stream_id     = makeStreamId(task_id,"RX",alloc.device_id,alloc.rx_channels[0]);
                sc.channel_index = alloc.rx_channels[0];
                sc.dest_ip       = streaming.dest_ip;
                sc.dest_port     = alloc.udp_ports[0];
                sc.packet_samples= cfg_.policy.iq_packet_samples;
                sc.task_start_ms = rec.start_time_ms;
                sc.sample_rate   = alloc.sample_rate_sps;
                for (int i=1; i<(int)alloc.rx_channels.size(); ++i) {
                    IQStreamer::ChannelDest ec;
                    ec.channel_index = alloc.rx_channels[i];
                    ec.stream_id     = makeStreamId(task_id,"RX",alloc.device_id,alloc.rx_channels[i]);
                    ec.dest_ip       = streaming.dest_ip;
                    ec.dest_port     = alloc.udp_ports[static_cast<size_t>(i)];
                    sc.extra_channels.push_back(ec);
                }
                auto streamer = std::make_shared<IQStreamer>(sc, dev->soapyDevice(), s, task_err_cb);
                streamer->updateCenterFreq(alloc.center_freq_hz);
                streamer->start();
                rt.streamers.push_back(streamer);
                for (int ch : alloc.rx_channels) {
                    std::string key = alloc.device_id + ":" + std::to_string(ch);
                    std::lock_guard lk(rt_mu_);
                    channel_states_[key] = {dev, s, streamer};
                }
            }
        } else {
            // Each channel has its own LO and its own SoapySDR stream.
            // Device acquires at alloc.sample_rate_sps, which findBestDevice
            // clamps up to the device's hardware floor (sample_rate_min_sps)
            // when the task asked for less. When that differs from what was
            // actually requested, stream via DDC instead of raw fan-out so
            // narrowband/digital-protocol tasks below the floor (e.g. FM_NB
            // or P25/DMR demod on RTL-SDR) still get decimated down to the
            // requested rate — transparent to the client.
            bool needs_ddc = alloc.output_sample_rate_sps > 0.0 &&
                std::abs(alloc.output_sample_rate_sps - alloc.sample_rate_sps) > 1.0;
            double task_cf = (alloc.slice_lo_hz + alloc.slice_hi_hz) / 2.0;

            for (int i=0; i<(int)alloc.rx_channels.size(); ++i) {
                int ch = alloc.rx_channels[i];
                if (!dev->tuneChannel(ch, alloc.center_freq_hz, alloc.sample_rate_sps)) {
                    hw_lock.unlock();
                    deactivateTask(task_id, TaskState::FAILED,
                                   "tuneChannel failed ch="+std::to_string(ch)); return;
                }
                dev->setRxGain(ch, dev->config().rx_gain_db, dev->config().rx_agc);

                SoapySDR::Stream* s = dev->openRxStream({ch});
                if (!s) {
                    hw_lock.unlock();
                    deactivateTask(task_id, TaskState::FAILED,
                                   "openRxStream failed ch="+std::to_string(ch)); return;
                }
                if (!dev->activateStream(s)) {
                    dev->closeStream(s);
                    hw_lock.unlock();
                    deactivateTask(task_id, TaskState::FAILED,
                                   "activateStream failed ch="+std::to_string(ch)); return;
                }
                rt.soapy_streams.push_back({dev, s});

                IQStreamer::Config sc;
                sc.task_id       = task_id;
                sc.stream_id     = makeStreamId(task_id,"RX",alloc.device_id,ch);
                sc.channel_index = ch;
                // DDC case: the sub-band consumer (added below) is the real
                // destination — leave dest_ip/port unset so IQStreamer::start()
                // doesn't also register the task's UDP port as a second,
                // un-decimated raw dest.
                if (!needs_ddc) {
                    sc.dest_ip   = streaming.dest_ip;
                    sc.dest_port = alloc.udp_ports[static_cast<size_t>(i)];
                }
                sc.packet_samples= cfg_.policy.iq_packet_samples;
                sc.task_start_ms = rec.start_time_ms;
                sc.sample_rate   = alloc.sample_rate_sps;
                auto streamer = std::make_shared<IQStreamer>(sc, dev->soapyDevice(), s, task_err_cb);
                streamer->updateCenterFreq(alloc.center_freq_hz);
                streamer->start();
                rt.streamers.push_back(streamer);

                if (needs_ddc) {
                    streamer->addSubBand(
                        task_id, makeStreamId(task_id,"RX",alloc.device_id,ch), ch,
                        streaming.dest_ip, alloc.udp_ports[static_cast<size_t>(i)],
                        task_cf, alloc.output_sample_rate_sps, alloc.sample_rate_sps);
                    rt.is_subband_consumer = true;
                    spdlog::info("activateTask [{}] DDC sub-band ch={} on {} cf={:.3f}MHz "
                                 "sr={:.3f}MSPS (acquired {:.3f}MSPS)",
                                 task_id, ch, alloc.device_id, task_cf / 1e6,
                                 alloc.output_sample_rate_sps / 1e6, alloc.sample_rate_sps / 1e6);
                }
            }
        }
    }

    // Hardware setup for all allocations is complete — release hw_activation_mu_
    // so other tasks (e.g. independent-LO channels on the same device) can
    // proceed with their own setup while this task runs.
    hw_lock.unlock();

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

    // Final state transition: PENDING → RUNNING.
    {
        std::lock_guard lock(reg_mu_);
        if (registry_.count(task_id)) {
            auto& r = registry_[task_id];
            if (!isTerminalState(r.state)) {
                // For IMMEDIATE tasks the stop_time was computed at acceptance time.
                // If hw activation was delayed (waiting on hw_activation_mu_) the
                // deadline may already be past.  Reset it so the task gets its full
                // intended duration from the moment hardware is actually running.
                if (r.schedule_mode == ScheduleMode::IMMEDIATE &&
                    r.stop_time_ms  != TIME_INFINITE) {
                    int64_t duration_ms = r.stop_time_ms - r.start_time_ms;
                    int64_t now2        = nowMs();
                    r.start_time_ms = now2;
                    r.stop_time_ms  = now2 + duration_ms;
                }
                r.state = TaskState::RUNNING;
            }
            // If state is already terminal (task cancelled during hw setup),
            // do NOT overwrite it. The IQStreamer's done-callback will call
            // deactivateTask() which now also cleans up orphaned runtimes.
        }
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
    // Atomically claim deactivation rights under reg_mu_.
    // If two threads race (e.g. stopTask() vs a background IQStreamer done-callback),
    // only the first one sets the state.  A second concurrent call finds state
    // already terminal but may still need to clean up an orphaned runtime (if
    // the first call ran before activateTask() inserted into runtimes_).
    bool already_terminal = false;
    {
        std::lock_guard lock(reg_mu_);
        auto it = registry_.find(task_id);
        if (it == registry_.end()) return;
        if (isTerminalState(it->second.state)) {
            already_terminal = true;  // state set by a prior deactivateTask call
        } else {
            it->second.state           = terminal_state;
            it->second.terminal_reason = reason;
        }
    }
    // If already terminal, only proceed if there is an orphaned runtime to clean.
    if (already_terminal) {
        std::lock_guard lock(rt_mu_);
        if (!runtimes_.count(task_id)) return;  // nothing orphaned
        // Fall through — there's an orphaned runtime inserted by activateTask
        // AFTER our previous deactivateTask cleaned up (found empty runtimes_).
    }

    spdlog::info("deactivateTask [{}] → {} reason={}",
                 task_id, taskStateToString(terminal_state), reason);

    // Stop runtime objects.
    // IMPORTANT: scan_exec/trig_mon are moved out of the runtime and stopped
    // AFTER releasing rt_mu_.  Their done-callbacks call deactivateTask which
    // also acquires rt_mu_; stopping them while holding rt_mu_ would deadlock.
    std::unique_ptr<ScanExecutor>    scan_exec_to_stop;
    std::unique_ptr<TriggerMonitor>  trig_mon_to_stop;
    {
        // Acquire hw_activation_mu_ before rt_mu_ (matching activateTask's lock
        // ordering) — deactivateStream/closeStream below must be serialized
        // against concurrent hardware setup in activateTask.
        std::lock_guard hwlock(hw_activation_mu_);
        std::lock_guard lock(rt_mu_);
        auto it = runtimes_.find(task_id);
        if (it != runtimes_.end()) {
            auto& rt = it->second;
            // Steal the executor/monitor so we can stop them without the lock.
            scan_exec_to_stop = std::move(rt.scan_exec);
            trig_mon_to_stop  = std::move(rt.trig_mon);

            // Remove this task's subscription from each streamer.
            // Sub-band consumers registered via addSubBand; raw fan-out via addDest.
            for (auto& streamer : rt.streamers) {
                if (rt.is_subband_consumer)
                    streamer->removeSubBand(task_id);
                else
                    streamer->removeDest(task_id);
            }

            // Close the SoapySDR stream only when the last consumer (dest or sub-band) is gone.
            // Shared-LO streams are tracked in channel_states_ — close when no consumers remain.
            // Independent-LO streams are NOT in channel_states_ (each task owns its stream
            // exclusively) — always close them directly to avoid leaking the SoapySDR stream.
            for (auto& [d, s] : rt.soapy_streams) {
                bool should_close = false;
                bool found_in_channel_states = false;
                for (auto cs = channel_states_.begin(); cs != channel_states_.end(); ) {
                    if (cs->second.soapy_stream == s) {
                        found_in_channel_states = true;
                        if (cs->second.streamer->totalConsumers() == 0) {
                            cs->second.streamer->stop();
                            should_close = true;
                            cs = channel_states_.erase(cs);
                        } else {
                            ++cs;
                        }
                    } else {
                        ++cs;
                    }
                }
                if (!found_in_channel_states) {
                    // Independent-LO stream exclusively owned by this task — stop all
                    // its IQStreamers (idempotent) and close the stream unconditionally.
                    for (auto& streamer : rt.streamers)
                        streamer->stop();
                    should_close = true;
                }
                if (should_close && d && s) {
                    d->deactivateStream(s);
                    d->closeStream(s);
                }
            }
            runtimes_.erase(it);
        }
    }
    // Stop executors outside rt_mu_ — their done-callbacks call deactivateTask
    // which needs rt_mu_, so joining them inside would deadlock.
    if (scan_exec_to_stop) scan_exec_to_stop->stop();
    if (trig_mon_to_stop)  trig_mon_to_stop->stop();

    // Release timeline slots and ports — only on first deactivation.
    // When already_terminal==true we are cleaning an orphaned runtime left by
    // an activateTask() that ran after a prior deactivateTask() already released
    // the ports. Releasing again would corrupt the pool.
    if (!already_terminal) {
        std::lock_guard lock(reg_mu_);
        auto it = registry_.find(task_id);
        if (it != registry_.end()) {
            for (auto& alloc : it->second.allocations) {
                if (timelines_.count(alloc.device_id))
                    timelines_[alloc.device_id]->remove(task_id);
                port_pool_->releaseAll(alloc.udp_ports);
            }
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
        activations_in_flight_.fetch_add(1, std::memory_order_relaxed);
        std::thread([this, id]() {
            activateTask(id);
            activations_in_flight_.fetch_sub(1, std::memory_order_relaxed);
        }).detach();
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

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
