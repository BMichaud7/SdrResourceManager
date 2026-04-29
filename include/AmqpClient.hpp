#pragma once
// ════════════════════════════════════════════════════════════════════════
//  AmqpClient.hpp  —  qpid-proton AMQP 1.0 client with auto-reconnect
// ════════════════════════════════════════════════════════════════════════
#include "ConfigParser.hpp"
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <deque>
#include <atomic>
#include <thread>

namespace proton {
class container;
class messaging_handler;
class sender;
class work_queue;
}

namespace sdr {

using MessageHandler = std::function<void(const std::string& body)>;

class AmqpClient {
public:
    explicit AmqpClient(const BrokerConfig& cfg, MessageHandler on_message);
    ~AmqpClient();

    AmqpClient(const AmqpClient&)=delete;
    AmqpClient& operator=(const AmqpClient&)=delete;

    void start();
    void stop();

    void sendResponse(const std::string& json_body);
    void sendStatus(const std::string& json_body);
    void sendHealth(const std::string& json_body);

    bool isConnected() const { return connected_.load(); }

private:
    BrokerConfig   cfg_;
    MessageHandler on_message_;

    class Handler;
    std::unique_ptr<Handler> handler_;
    std::unique_ptr<proton::container> container_;
    std::thread container_thread_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_  {false};

    friend class Handler;
    void onConnected();
    void onDisconnected();
};

} // namespace sdr
