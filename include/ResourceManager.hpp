#pragma once
// ════════════════════════════════════════════════════════════════════════
//  ResourceManager.hpp
//  The scheduling brain. Owns all SpectrumTimelines, RadioDevices,
//  UdpPortPool, and the TaskRegistry.
// ════════════════════════════════════════════════════════════════════════
#include "Types.hpp"
#include "ConfigParser.hpp"
#include "SpectrumTimeline.hpp"
#include "UdpPortPool.hpp"
#include "RadioDevice.hpp"
#include "IQStreamer.hpp"
#include "ScanExecutor.hpp"
#include "TriggerMonitor.hpp"
#include "FftEngine.hpp"
#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <functional>
#include <chrono>

namespace sdr {

class ResourceManager {
public:
    explicit ResourceManager(const AppConfig& cfg,
                             TaskStateChangedCb on_state_change,
                             TaskErrorCb        on_error);
    ~ResourceManager();

    ResourceManager(const ResourceManager&)=delete;
    ResourceManager& operator=(const ResourceManager&)=delete;

    // Open all devices from config. Returns count opened successfully.
    int  openDevices();
    void closeDevices();

    // ── Scheduling API ─────────────────────────────────────────────────
    TaskResponse tryAccept(const TaskRequest& req);
    TaskResponse stopTask(const std::string& task_id, const std::string& request_id,
                          const std::string& reason);
    TaskResponse cancelTask(const std::string& task_id, const std::string& request_id,
                            const std::string& reason);

    // ── Called by Controller threads ───────────────────────────────────
    void schedulerTick();   // starts SCHEDULED tasks whose start_time <= now
    void watchdogTick();    // expires tasks whose stop_time <= now

    // ── Queries ────────────────────────────────────────────────────────
    std::vector<TaskRecord>   getActiveTasks() const;
    int  countByState(TaskState s) const;
    std::vector<std::string>  deviceIds() const;
    RadioDevice*              getDevice(const std::string& id);

    // For health reporting
    struct DeviceSummary {
        std::string device_id, driver, uri, coherency_group;
        bool   online=false; int active_tasks=0;
        double cf_hz=0, rate_sps=0, alloc_bw_hz=0, free_bw_hz=0, temp_c=0;
    };
    std::vector<DeviceSummary> deviceSummaries() const;

    int udpPortsUsed() const { return port_pool_->usedCount(); }
    int udpPortsFree() const { return port_pool_->freeCount(); }

private:
    AppConfig         cfg_;
    TaskStateChangedCb on_state_change_;
    TaskErrorCb        on_error_;

    // Devices (indexed by id)
    std::unordered_map<std::string, std::unique_ptr<RadioDevice>>     devices_;
    std::unordered_map<std::string, std::unique_ptr<SpectrumTimeline>> timelines_;

    std::unique_ptr<UdpPortPool> port_pool_;
    std::unique_ptr<FftEngine>   fft_engine_;

    // Task registry
    mutable std::mutex                             reg_mu_;
    std::unordered_map<std::string, TaskRecord>    registry_;

    // Live streaming objects (per task)
    struct TaskRuntime {
        // Each entry is {owning RadioDevice, SoapySDR stream}.
        // Stored as pairs so deactivation can close each stream on its own device,
        // which matters for multi-device coherent tasks.
        std::vector<std::pair<RadioDevice*, SoapySDR::Stream*>> soapy_streams;
        RadioDevice*                              device = nullptr; // primary (scan/trigger)
        std::vector<std::unique_ptr<IQStreamer>>  streamers;
        std::unique_ptr<ScanExecutor>             scan_exec;
        std::unique_ptr<TriggerMonitor>           trig_mon;
    };
    mutable std::mutex                              rt_mu_;
    std::unordered_map<std::string, TaskRuntime>    runtimes_;

    std::chrono::steady_clock::time_point start_time_;

    // ── Internal helpers ───────────────────────────────────────────────
    TaskResponse doAcceptStandard(const TaskRequest& req);
    TaskResponse doAcceptScan(const TaskRequest& req);
    TaskResponse doAcceptSnapshot(const TaskRequest& req);
    TaskResponse doAcceptCalibration(const TaskRequest& req);

    struct DeviceCandidate {
        RadioDevice*    device;
        FitResult       fit;
    };
    std::optional<DeviceCandidate>
        findBestDevice(double cf, double bw, double sr,
                       int rx_count, int tx_count,
                       int64_t t_start, int64_t t_stop,
                       const std::string& preferred) const;

    void activateTask(const std::string& task_id);
    void deactivateTask(const std::string& task_id, TaskState terminal_state,
                        const std::string& reason);
    void notifyStateChange(const std::string& task_id);

    std::string makeStreamId(const std::string& task_id, const std::string& type,
                             const std::string& dev_id, int ch_idx) const;

    static int64_t nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
};

} // namespace sdr
