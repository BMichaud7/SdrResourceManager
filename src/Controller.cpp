#include "Controller.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <cmath>
#include <thread>

namespace sdr {
using namespace std::chrono;

Controller::Controller(const AppConfig& cfg)
    : cfg_(cfg)
    , start_time_(steady_clock::now())
{
    rm_ = std::make_unique<ResourceManager>(
        cfg_,
        [this](const TaskRecord& rec) { onTaskStateChanged(rec); },
        [](const std::string& tid, const std::string& err) {
            spdlog::error("Task error {}: {}", tid, err);
        });

    amqp_ = std::make_unique<AmqpClient>(
        cfg_.broker,
        [this](const std::string& body, const std::string& reply_to) { onMessage(body, reply_to); });
}

Controller::~Controller() { stop(); }

void Controller::run() {
    spdlog::info("Controller: starting, version={}", APP_VERSION);

    int opened = rm_->openDevices();
    spdlog::info("Controller: {}/{} devices opened",
                 opened, (int)cfg_.devices.size());

    amqp_->start();
    running_.store(true);

    scheduler_thread_ = std::thread(&Controller::schedulerLoop, this);
    watchdog_thread_  = std::thread(&Controller::watchdogLoop,  this);
    heartbeat_thread_ = std::thread(&Controller::heartbeatLoop, this);

    spdlog::info("Controller: running");
    while (running_.load()) {
        std::this_thread::sleep_for(milliseconds(100));
    }
    spdlog::info("Controller: stopped");
}

void Controller::stop() {
    running_.store(false);
    if (scheduler_thread_.joinable()) scheduler_thread_.join();
    if (watchdog_thread_.joinable())  watchdog_thread_.join();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    amqp_->stop();
    rm_->closeDevices();
}

// ── Message routing ──────────────────────────────────────────────────────
void Controller::onMessage(const std::string& body, const std::string& reply_to) {
    auto req = MessageCodec::decode(body);
    if (!req) {
        spdlog::warn("Controller: failed to decode message");
        return;
    }
    spdlog::debug("Controller: received msg_type={} req_id={}",
                  req->msg_type, req->request_id);

    if (req->msg_type=="TASK_STOP")                  return handleTaskStop(*req, reply_to);
    if (req->msg_type=="TASK_CANCEL")                return handleTaskCancel(*req, reply_to);
    if (req->msg_type=="HEALTH_QUERY")               return handleHealthQuery(*req, reply_to);
    if (req->msg_type=="DEVICE_TEMP_QUERY")          return handleTempQuery(*req, reply_to);
    if (req->msg_type=="TASK_REQUEST_SNAPSHOT")      return handleSnapshotRequest(*req, reply_to);
    return handleTaskRequest(*req, reply_to);
}

void Controller::handleTaskRequest(const TaskRequest& req, const std::string& reply_to) {
    if (req.task_type == TaskType::UNKNOWN) {
        sendReject(req.request_id, req.correlation_id,
                   RejectCode::INVALID_TASK_TYPE, "Unknown task_type"); return;
    }
    auto resp = rm_->tryAccept(req);
    amqp_->sendResponse(MessageCodec::encodeTaskResponse(resp), reply_to);
}

void Controller::handleTaskStop(const TaskRequest& req, const std::string& reply_to) {
    auto resp = rm_->stopTask(req.task_id, req.request_id, req.reason);
    amqp_->sendResponse(MessageCodec::encodeTaskResponse(resp), reply_to);
}

void Controller::handleTaskCancel(const TaskRequest& req, const std::string& reply_to) {
    auto resp = rm_->cancelTask(req.task_id, req.request_id, req.reason);
    amqp_->sendResponse(MessageCodec::encodeTaskResponse(resp), reply_to);
}

void Controller::handleHealthQuery(const TaskRequest& req, const std::string& reply_to) {
    auto devs = rm_->deviceSummaries();
    std::vector<MessageCodec::DevHealthEntry> entries;
    for (auto& d : devs) {
        MessageCodec::DevHealthEntry e;
        e.device_id=d.device_id; e.driver=d.driver; e.uri=d.uri;
        e.coherency_group=d.coherency_group; e.online=d.online;
        e.active_tasks=d.active_tasks; e.cf_hz=d.cf_hz;
        e.rate_sps=d.rate_sps; e.alloc_bw_hz=d.alloc_bw_hz;
        e.free_bw_hz=d.free_bw_hz; e.temp_c=d.temp_c;
        entries.push_back(e);
    }
    int64_t uptime = duration_cast<seconds>(steady_clock::now()-start_time_).count();
    auto body = MessageCodec::encodeHealthQueryResponse(
        req.request_id, entries,
        (int)cfg_.devices.size(),
        (int)std::count_if(entries.begin(),entries.end(),[](auto& e){return e.online;}),
        rm_->countByState(TaskState::SCHEDULED),
        rm_->countByState(TaskState::PENDING),
        rm_->countByState(TaskState::RUNNING),
        rm_->countByState(TaskState::SCHEDULED)+
        rm_->countByState(TaskState::PENDING)+
        rm_->countByState(TaskState::RUNNING),
        rm_->udpPortsUsed(), rm_->udpPortsFree(), uptime);
    amqp_->sendResponse(body, reply_to);
}

void Controller::handleSnapshotRequest(const TaskRequest& req, const std::string& reply_to) {
    auto resp = rm_->tryAccept(req);
    amqp_->sendResponse(MessageCodec::encodeTaskResponse(resp), reply_to);
}

void Controller::onTaskStateChanged(const TaskRecord& rec) {
    std::vector<StreamMetrics> m = rec.stream_metrics;
    auto body = MessageCodec::encodeTaskStatus(rec, m);
    amqp_->sendStatus(body);
    spdlog::info("TaskStatus [{}] → {}", rec.task_id, taskStateToString(rec.state));
}

// ── Background threads ───────────────────────────────────────────────────
void Controller::schedulerLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(milliseconds(cfg_.policy.scheduler_tick_ms));
        if (running_.load()) rm_->schedulerTick();
    }
}

void Controller::watchdogLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(milliseconds(cfg_.policy.watchdog_tick_ms));
        if (running_.load()) rm_->watchdogTick();
    }
}

void Controller::heartbeatLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(milliseconds(cfg_.policy.heartbeat_interval_ms));
        if (!running_.load()) break;

        auto devs = rm_->deviceSummaries();
        std::vector<MessageCodec::DevHealthEntry> entries;
        for (auto& d : devs) {
            MessageCodec::DevHealthEntry e;
            e.device_id=d.device_id; e.driver=d.driver; e.uri=d.uri;
            e.coherency_group=d.coherency_group; e.online=d.online;
            e.active_tasks=d.active_tasks; e.cf_hz=d.cf_hz;
            e.rate_sps=d.rate_sps; e.alloc_bw_hz=d.alloc_bw_hz;
            e.free_bw_hz=d.free_bw_hz; e.temp_c=d.temp_c;
            entries.push_back(e);
        }
        amqp_->sendHealth(MessageCodec::encodeDeviceHealth("hb-auto", entries));

        int64_t uptime = duration_cast<seconds>(steady_clock::now()-start_time_).count();
        amqp_->sendHealth(MessageCodec::encodeControllerHealth(
            "hb-auto",
            (int)cfg_.devices.size(),
            (int)std::count_if(entries.begin(),entries.end(),[](auto& e){return e.online;}),
            rm_->countByState(TaskState::SCHEDULED),
            rm_->countByState(TaskState::PENDING),
            rm_->countByState(TaskState::RUNNING),
            rm_->countByState(TaskState::SCHEDULED)+
            rm_->countByState(TaskState::PENDING)+
            rm_->countByState(TaskState::RUNNING),
            rm_->udpPortsUsed(), rm_->udpPortsFree(), uptime));
    }
}

void Controller::handleTempQuery(const TaskRequest& req, const std::string& reply_to) {
    std::vector<MessageCodec::TempEntry> entries;
    for (auto& dev_id : rm_->deviceIds()) {
        MessageCodec::TempEntry e;
        e.device_id = dev_id;
        auto* dev = rm_->getDevice(dev_id);
        if (!dev) continue;
        e.online = dev->isOnline();
        for (auto& s : dev->listTemperatures()) {
            bool ok = !std::isnan(s.value_c);
            e.sensors.push_back({s.name, s.value_c, ok});
        }
        entries.push_back(std::move(e));
    }
    amqp_->sendResponse(MessageCodec::encodeTempResponse(req.request_id, entries), reply_to);
}

void Controller::sendReject(const std::string& req_id,
                              const std::string& corr_id,
                              RejectCode code,
                              const std::string& reason) {
    TaskResponse resp;
    resp.request_id    = req_id;
    resp.correlation_id= corr_id;
    resp.reject_code   = code;
    resp.reject_reason = reason;
    amqp_->sendResponse(MessageCodec::encodeTaskResponse(resp));
}

} // namespace sdr
