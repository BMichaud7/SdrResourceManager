#include "AmqpPublisher.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>

namespace acq {

using json = nlohmann::json;
using namespace std::chrono;

AmqpPublisher::AmqpPublisher(std::string url, std::string topic, std::string scanner_id)
    : url_(std::move(url)), topic_(std::move(topic)), scanner_id_(std::move(scanner_id))
{}

AmqpPublisher::~AmqpPublisher() { stop(); }

void AmqpPublisher::start() {
    container_ = new proton::container(*this);
    thread_ = std::thread([this]{ container_->run(); });
}

void AmqpPublisher::stop() {
    stopping_ = true;
    if (container_) {
        if (work_queue_)
            work_queue_->add([this]{ sender_.connection().close(); });
        if (thread_.joinable()) thread_.join();
        delete container_;
        container_ = nullptr;
    }
}

void AmqpPublisher::publish(const Detection& d) {
    if (!work_queue_ || stopping_) return;
    proton::message msg = makeMessage(d);
    work_queue_->add([this, msg]() mutable {
        if (sender_ && sender_.credit() > 0)
            sender_.send(msg);
    });
}

void AmqpPublisher::on_container_start(proton::container& c) {
    proton::connection_options opts;
    if (!url_.empty()) {
        // Extract credentials from config and set via options if needed
    }
    auto conn = c.connect(url_);
    conn.open_sender(topic_);
}

void AmqpPublisher::on_sender_open(proton::sender& s) {
    sender_     = s;
    work_queue_ = &s.work_queue();
    spdlog::info("[AmqpPublisher] connected → {}", topic_);
}

void AmqpPublisher::on_transport_error(proton::transport& t) {
    spdlog::warn("[AmqpPublisher] transport error: {}", t.error().what());
}

void AmqpPublisher::on_connection_error(proton::connection& c) {
    spdlog::warn("[AmqpPublisher] connection error: {}", c.error().what());
}

proton::message AmqpPublisher::makeMessage(const Detection& d) const {
    auto ms = duration_cast<milliseconds>(
        d.timestamp.time_since_epoch()).count();

    json body = {
        {"msg_type",        "RF_DETECTION"},
        {"schema_version",  SCHEMA_VERSION},
        {"timestamp_ms",    ms},
        {"scanner_id",      d.scanner_id},
        {"center_freq_hz",  d.center_freq_hz},
        {"bandwidth_hz",    d.bandwidth_hz},
        {"power_db",        d.power_db},
        {"channel",         d.channel}
    };

    proton::message msg;
    msg.body(body.dump());
    msg.content_type("application/json");
    return msg;
}

} // namespace acq
