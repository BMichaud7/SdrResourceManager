#include "AmqpClient.hpp"
#include <proton/container.hpp>
#include <proton/message.hpp>
#include <proton/messaging_handler.hpp>
#include <proton/connection_options.hpp>
#include <proton/reconnect_options.hpp>
#include <proton/transport.hpp>
#include <proton/sender.hpp>
#include <proton/receiver.hpp>
#include <proton/delivery.hpp>
#include <proton/work_queue.hpp>
#include <proton/source_options.hpp>
#include <proton/target_options.hpp>
#include <proton/value.hpp>
#include <spdlog/spdlog.h>
#include <mutex>
#include <deque>
#include <chrono>
#include <stdexcept>

namespace sdr {

// ─── Handler (proton internal class) ────────────────────────────────────
class AmqpClient::Handler : public proton::messaging_handler {
public:
    Handler(AmqpClient& parent, const BrokerConfig& cfg)
        : parent_(parent), cfg_(cfg) {}

    void on_container_start(proton::container& c) override {
        proton::connection_options copts;
        if (!cfg_.username.empty()) {
            copts.sasl_allowed_mechs("PLAIN");
            copts.sasl_allow_insecure_mechs(true);
            copts.user(cfg_.username).password(cfg_.password);
        } else {
            copts.sasl_allowed_mechs("ANONYMOUS");
        }

        proton::reconnect_options ropts;
        ropts.delay(proton::duration(cfg_.reconnect_interval_sec * 1000));
        ropts.max_delay(proton::duration(cfg_.max_reconnect_interval_sec * 1000));
        ropts.max_attempts(0);  // 0 = retry forever
        copts.reconnect(ropts);

        c.connect(cfg_.url, copts);
    }

    void on_connection_open(proton::connection& c) override {
        spdlog::info("AmqpClient: connected to {}", cfg_.url);
        // Open receiver on request queue
        c.open_receiver(cfg_.request_queue);
        // Open senders
        response_sender_ = c.open_sender(cfg_.response_queue);
        status_sender_   = c.open_sender(cfg_.status_topic);
        health_sender_   = c.open_sender(cfg_.health_topic);
        parent_.onConnected();
    }

    void on_message(proton::delivery&, proton::message& msg) override {
        try {
            std::string body = proton::get<std::string>(msg.body());
            parent_.on_message_(body);
        } catch (const std::exception& ex) {
            spdlog::error("AmqpClient: on_message error: {}", ex.what());
        }
    }

    void on_connection_close(proton::connection&) override {
        spdlog::warn("AmqpClient: connection closed");
        parent_.onDisconnected();
    }

    void on_transport_error(proton::transport& t) override {
        spdlog::error("AmqpClient: transport error: {}", t.error().what());
        parent_.onDisconnected();
    }

    void on_error(const proton::error_condition& e) override {
        spdlog::error("AmqpClient: error: {}", e.what());
    }

    void sendOn(proton::sender& s, const std::string& body) {
        if (!s || !s.credit()) return;
        try {
            proton::message m;
            m.body(body);
            m.content_type("application/json");
            m.durable(false);
            s.send(m);
        } catch (const std::exception& ex) {
            spdlog::error("AmqpClient: send error: {}", ex.what());
        }
    }

    proton::sender response_sender_;
    proton::sender status_sender_;
    proton::sender health_sender_;

private:
    AmqpClient&   parent_;
    BrokerConfig  cfg_;
};

// ─────────────────────────────────────────────────────────────────────────
AmqpClient::AmqpClient(const BrokerConfig& cfg, MessageHandler on_message)
    : cfg_(cfg), on_message_(std::move(on_message))
{
    handler_   = std::make_unique<Handler>(*this, cfg_);
    container_ = std::make_unique<proton::container>(*handler_);
}

AmqpClient::~AmqpClient() { stop(); }

void AmqpClient::start() {
    running_.store(true);
    container_thread_ = std::thread([this]() {
        try { container_->run(); }
        catch (const std::exception& ex) {
            spdlog::error("AmqpClient: container run exception: {}", ex.what());
        }
    });
    spdlog::info("AmqpClient: started, connecting to {}", cfg_.url);
}

void AmqpClient::stop() {
    if (!running_.exchange(false)) return;
    try { container_->stop(); } catch(...) {}
    if (container_thread_.joinable()) container_thread_.join();
    spdlog::info("AmqpClient: stopped");
}

void AmqpClient::sendResponse(const std::string& body) {
    if (!connected_) { spdlog::warn("AmqpClient: sendResponse while disconnected"); return; }
    container_->schedule(proton::duration(0), [this, body]() {
        handler_->sendOn(handler_->response_sender_, body);
    });
}

void AmqpClient::sendStatus(const std::string& body) {
    if (!connected_) return;
    container_->schedule(proton::duration(0), [this, body]() {
        handler_->sendOn(handler_->status_sender_, body);
    });
}

void AmqpClient::sendHealth(const std::string& body) {
    if (!connected_) return;
    container_->schedule(proton::duration(0), [this, body]() {
        handler_->sendOn(handler_->health_sender_, body);
    });
}

void AmqpClient::onConnected()    { connected_.store(true);  }
void AmqpClient::onDisconnected() { connected_.store(false); }

} // namespace sdr
