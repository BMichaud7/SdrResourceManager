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
/**
 * @file AmqpClient.hpp
 * @brief AMQP 1.0 client for ResourceManager — handles task request/response messaging.
 *
 * Runs a proton::container in a background thread. Subscribes to the task
request queue, publishes TASK_ACCEPTED/REJECTED responses, and notifies the
scheduler of new requests via a callback.
 */
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

// reply_to is the address from the request's reply_to property (may be empty).
using MessageHandler = std::function<void(const std::string& body, const std::string& reply_to)>;

class AmqpClient {
public:
    explicit AmqpClient(const BrokerConfig& cfg, MessageHandler on_message);
    ~AmqpClient();

    AmqpClient(const AmqpClient&)=delete;
    AmqpClient& operator=(const AmqpClient&)=delete;

    void start();
    void stop();

    // If reply_to is non-empty, route to that address; otherwise use the
    // configured response_queue (backwards-compatible fallback).
    void sendResponse(const std::string& json_body,
                      const std::string& reply_to = "");
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
    // Set by the container_thread_ lambda itself right before it returns —
    // lets stop() decide join() vs. detach() without ever calling both on
    // the same std::thread concurrently (see stop()'s comment).
    std::atomic<bool> container_stopped_{false};

    friend class Handler;
    void onConnected();
    void onDisconnected();
};

} // namespace sdr

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
