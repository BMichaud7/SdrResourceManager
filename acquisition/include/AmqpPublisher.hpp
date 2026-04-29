#pragma once
#include "Types.hpp"
#include <proton/container.hpp>
#include <proton/message.hpp>
#include <proton/messaging_handler.hpp>
#include <proton/connection.hpp>
#include <proton/connection_options.hpp>
#include <proton/sender.hpp>
#include <proton/transport.hpp>
#include <proton/work_queue.hpp>
#include <atomic>
#include <thread>
#include <string>

namespace acq {

// Publishes one AMQP message per Detection to a topic.
// Thread-safe: publish() may be called from any thread.
class AmqpPublisher : public proton::messaging_handler {
public:
    AmqpPublisher(std::string url, std::string topic, std::string scanner_id);
    ~AmqpPublisher();

    AmqpPublisher(const AmqpPublisher&)            = delete;
    AmqpPublisher& operator=(const AmqpPublisher&) = delete;

    void start();
    void stop();
    void publish(const Detection& d);

    // proton messaging_handler overrides
    void on_container_start(proton::container&) override;
    void on_sender_open(proton::sender&) override;
    void on_transport_error(proton::transport&) override;
    void on_connection_error(proton::connection&) override;

private:
    std::string         url_;
    std::string         topic_;
    std::string         scanner_id_;
    proton::sender      sender_;
    proton::work_queue* work_queue_{nullptr};
    proton::container*  container_{nullptr};
    std::thread         thread_;
    std::atomic<bool>   stopping_{false};

    proton::message makeMessage(const Detection& d) const;
};

} // namespace acq
