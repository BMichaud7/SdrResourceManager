#pragma once
// ════════════════════════════════════════════════════════════════════════
//  Controller.hpp  —  Top-level orchestrator
// ════════════════════════════════════════════════════════════════════════
#include "ConfigParser.hpp"
#include "ResourceManager.hpp"
#include "AmqpClient.hpp"
#include "sdr/MessageCodec.hpp"
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>

namespace sdr {

class Controller {
public:
    explicit Controller(const AppConfig& cfg);
    ~Controller();
    Controller(const Controller&)=delete;
    Controller& operator=(const Controller&)=delete;

    void run();     // blocks until stop() is called
    void stop();

private:
    AppConfig                      cfg_;
    std::unique_ptr<ResourceManager> rm_;
    std::unique_ptr<AmqpClient>      amqp_;

    std::atomic<bool>  running_{false};
    std::thread        scheduler_thread_;
    std::thread        watchdog_thread_;
    std::thread        heartbeat_thread_;

    std::chrono::steady_clock::time_point start_time_;

    void onMessage(const std::string& body);
    void onTaskStateChanged(const TaskRecord& rec);
    void schedulerLoop();
    void watchdogLoop();
    void heartbeatLoop();

    void handleTaskRequest(const TaskRequest& req);
    void handleTaskStop(const TaskRequest& req);
    void handleTaskCancel(const TaskRequest& req);
    void handleHealthQuery(const TaskRequest& req);
    void handleSnapshotRequest(const TaskRequest& req);
    void handleTempQuery(const TaskRequest& req);

    void sendReject(const std::string& req_id, const std::string& corr_id,
                    RejectCode code, const std::string& reason);
};

} // namespace sdr
