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
#pragma once
/**
 * @file ResourceManager.hpp
 * @brief Scheduling brain — owns SpectrumTimelines, RadioDevices, and the task registry.
 *
 * ResourceManager is the core scheduler.  It:
 * - Maintains a SpectrumTimeline per device for time/frequency conflict detection.
 * - Selects the best device for each incoming TaskRequest (findBestDevice).
 * - Handles preemption: higher-rank tasks cancel lower-rank tasks holding needed resources.
 * - Handles combined-window retuning: two tasks near each other on a shared_lo device
 *   are combined onto a single physical channel (tryRetuneCombined).
 * - Activates accepted tasks in background threads (openRxStream, IQStreamer).
 * - Deactivates tasks on completion, failure, or cancellation.
 *
 * ## Thread safety
 * All public methods are safe to call from multiple threads.  Two mutexes are used:
 * - `reg_mu_` — guards the task registry and SpectrumTimelines.
 * - `rt_mu_` — guards live runtime objects (IQStreamers, ScanExecutors, etc.).
 * - `hw_activation_mu_` — serialises SoapySDR open/close across background activation threads.
 */
#include "sdr/Types.hpp"
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

/**
 * @brief Schedules and activates SDR tasks across a pool of RadioDevices.
 *
 * Non-copyable.  Created by Controller; all methods are thread-safe.
 */
class ResourceManager {
public:
    /**
     * @brief Construct the resource manager.
     * @param cfg              Application configuration (devices, port pool, etc.).
     * @param on_state_change  Callback fired on every task state transition.
     * @param on_error         Callback fired on unrecoverable task errors.
     */
    explicit ResourceManager(const AppConfig& cfg,
                             TaskStateChangedCb on_state_change,
                             TaskErrorCb        on_error);
    ~ResourceManager();

    ResourceManager(const ResourceManager&)=delete;
    ResourceManager& operator=(const ResourceManager&)=delete;

    /**
     * @brief Open all devices listed in the configuration.
     * @return Number of devices that opened successfully.
     */
    int  openDevices();
    /// @brief Close all open devices (called at shutdown).
    void closeDevices();

    // ── Scheduling API ─────────────────────────────────────────────────────

    /**
     * @brief Evaluate and accept (or reject) an incoming task request.
     *
     * Performs conflict detection, preemption, and device selection.
     * On acceptance, the task is inserted into the registry and queued
     * for activation (PENDING → RUNNING) in a background thread.
     *
     * @param req Decoded task request from the AMQP message.
     * @return TaskResponse with accepted=true and assigned streams, or
     *         accepted=false with a reject_code and reject_reason.
     */
    TaskResponse tryAccept(const TaskRequest& req);

    /**
     * @brief Stop a running or scheduled task gracefully.
     * @param task_id    Task to stop.
     * @param request_id Request UUID (echoed in the response).
     * @param reason     Human-readable stop reason.
     * @return TaskResponse indicating success or failure (e.g. TASK_NOT_FOUND).
     */
    TaskResponse stopTask(const std::string& task_id, const std::string& request_id,
                          const std::string& reason);

    /**
     * @brief Cancel a task immediately (including SCHEDULED tasks).
     * @param task_id    Task to cancel.
     * @param request_id Request UUID.
     * @param reason     Human-readable cancel reason.
     * @return TaskResponse indicating success or failure.
     */
    TaskResponse cancelTask(const std::string& task_id, const std::string& request_id,
                            const std::string& reason);

    // ── Controller thread helpers ───────────────────────────────────────────

    /// @brief Start any SCHEDULED tasks whose start_time_ms ≤ now.  Called by Controller.
    void schedulerTick();
    /// @brief Expire any RUNNING tasks whose stop_time_ms ≤ now.  Called by Controller.
    void watchdogTick();

    // ── Queries ────────────────────────────────────────────────────────────

    /// @brief Return all tasks in non-terminal states.
    std::vector<TaskRecord>   getActiveTasks() const;
    /// @brief Count tasks in state @p s.
    int  countByState(TaskState s) const;
    /// @brief Return all configured device IDs.
    std::vector<std::string>  deviceIds() const;
    /// @brief Pointer to a device by ID; nullptr if not found.
    RadioDevice*              getDevice(const std::string& id);

    /// @brief Summary of one device for health responses.
    struct DeviceSummary {
        std::string device_id, driver, uri, coherency_group;
        bool   online      = false;
        int    active_tasks = 0;
        double cf_hz       = 0;  ///< Current LO frequency (Hz).
        double rate_sps    = 0;  ///< Current sample rate (samples/s).
        double alloc_bw_hz = 0;  ///< Allocated bandwidth (Hz).
        double free_bw_hz  = 0;  ///< Free bandwidth (Hz).
        double temp_c      = 0;  ///< Last temperature reading (°C).
    };
    /// @brief Snapshot of all device summaries for a HEALTH_QUERY_RESPONSE.
    std::vector<DeviceSummary> deviceSummaries() const;

    int udpPortsUsed() const { return port_pool_->usedCount(); }
    int udpPortsFree() const { return port_pool_->freeCount(); }

private:
    AppConfig         cfg_;
    TaskStateChangedCb on_state_change_;
    TaskErrorCb        on_error_;

    std::unordered_map<std::string, std::unique_ptr<RadioDevice>>     devices_;
    std::unordered_map<std::string, std::unique_ptr<SpectrumTimeline>> timelines_;

    std::unique_ptr<UdpPortPool> port_pool_;
    std::unique_ptr<FftEngine>   fft_engine_;

    mutable std::mutex                             reg_mu_;
    std::unordered_map<std::string, TaskRecord>    registry_;

    /// Per-channel hardware state shared by all tasks multicast from the same channel.
    struct ChannelState {
        RadioDevice*               device       = nullptr;
        SoapySDR::Stream*          soapy_stream = nullptr;
        std::shared_ptr<IQStreamer> streamer;
    };
    std::unordered_map<std::string, ChannelState> channel_states_; // key="dev_id:ch"

    /// Live runtime objects for one active task.
    struct TaskRuntime {
        std::vector<std::pair<RadioDevice*, SoapySDR::Stream*>> soapy_streams;
        RadioDevice*                             device = nullptr;
        std::vector<std::shared_ptr<IQStreamer>> streamers;
        std::unique_ptr<ScanExecutor>            scan_exec;
        std::unique_ptr<TriggerMonitor>          trig_mon;
        /// Held during hardware open; released in deactivateTask after stream close.
        std::unique_lock<std::mutex>             hw_lock;
        bool                                     is_subband_consumer = false;
    };
    mutable std::mutex                              rt_mu_;
    std::unordered_map<std::string, TaskRuntime>    runtimes_;

    /// Serialises SoapySDR open/close across background activation threads.
    std::mutex hw_activation_mu_;

    std::chrono::steady_clock::time_point start_time_;

    TaskResponse doAcceptStandard(const TaskRequest& req);
    TaskResponse doAcceptScan(const TaskRequest& req);
    TaskResponse doAcceptSnapshot(const TaskRequest& req);
    TaskResponse doAcceptCalibration(const TaskRequest& req);

    struct DeviceCandidate { RadioDevice* device; FitResult fit; };
    std::optional<DeviceCandidate>
        findBestDevice(double cf, double bw, double sr,
                       int rx_count, int tx_count,
                       int64_t t_start, int64_t t_stop,
                       const std::string& preferred,
                       int preferred_channel = -1) const;

    void activateTask(const std::string& task_id);
    void deactivateTask(const std::string& task_id, TaskState terminal_state,
                        const std::string& reason);
    void notifyStateChange(const std::string& task_id);

    bool tryPreemptConflicting(const TaskRequest& req, int64_t t_start, int64_t t_stop);
    std::optional<DeviceCandidate> tryRetuneCombined(const TaskRequest& req, int64_t t_start, int64_t t_stop);

    std::string makeStreamId(const std::string& task_id, const std::string& type,
                             const std::string& dev_id, int ch_idx) const;

    static int64_t nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
};

} // namespace sdr
