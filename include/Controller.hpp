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
 * @file Controller.hpp
 * @brief Top-level orchestrator for the SDR Resource Manager.
 *
 * Controller owns the ResourceManager and AmqpClient.  It:
 * - Starts the AMQP subscription loop (receives task requests).
 * - Dispatches each message to the appropriate handler.
 * - Runs the scheduler tick (starts SCHEDULED tasks on time) and watchdog
 *   (expires tasks whose end time has passed) in background threads.
 * - Publishes TASK_STATUS heartbeats on state changes.
 *
 * ## Lifecycle
 * ```
 * Controller ctrl(cfg);
 * ctrl.run();   // blocks until stop() is called (e.g. SIGTERM handler)
 * ```
 */
#include "ConfigParser.hpp"
#include "ResourceManager.hpp"
#include "AmqpClient.hpp"
#include "sdr/MessageCodec.hpp"
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>

namespace sdr {

/**
 * @brief Top-level controller: AMQP dispatcher + scheduler + watchdog.
 *
 * Non-copyable.  Exactly one instance per process.
 */
class Controller {
public:
    /**
     * @brief Construct the controller.
     * @param cfg Fully populated application configuration from ConfigParser.
     */
    explicit Controller(const AppConfig& cfg);
    ~Controller();
    Controller(const Controller&)=delete;
    Controller& operator=(const Controller&)=delete;

    /**
     * @brief Open all devices, connect to AMQP, and run until stop().
     *
     * Blocks the calling thread.  Call stop() from a signal handler or
     * another thread to exit.
     */
    void run();

    /// @brief Signal run() to return.  Thread-safe.
    void stop();

private:
    AppConfig                      cfg_;
    std::unique_ptr<ResourceManager> rm_;
    std::unique_ptr<AmqpClient>      amqp_;

    std::atomic<bool>  running_{false};
    std::thread        scheduler_thread_;  ///< Calls rm_->schedulerTick() periodically.
    std::thread        watchdog_thread_;   ///< Calls rm_->watchdogTick() periodically.
    std::thread        heartbeat_thread_;  ///< Publishes TASK_STATUS heartbeats.

    std::chrono::steady_clock::time_point start_time_;

    void onMessage(const std::string& body, const std::string& reply_to);
    void onTaskStateChanged(const TaskRecord& rec);
    void schedulerLoop();
    void watchdogLoop();
    void heartbeatLoop();

    void handleTaskRequest(const TaskRequest& req, const std::string& reply_to);
    void handleTaskStop(const TaskRequest& req,    const std::string& reply_to);
    void handleTaskCancel(const TaskRequest& req,  const std::string& reply_to);
    void handleHealthQuery(const TaskRequest& req, const std::string& reply_to);
    void handleSnapshotRequest(const TaskRequest& req, const std::string& reply_to);
    void handleTempQuery(const TaskRequest& req,   const std::string& reply_to);

    void sendReject(const std::string& req_id, const std::string& corr_id,
                    RejectCode code, const std::string& reason);
};

} // namespace sdr

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
