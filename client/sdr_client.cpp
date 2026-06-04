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
// ════════════════════════════════════════════════════════════════════════
//  sdr_client.cpp — SDR Radio Resource Task Manager Example Client
//
//  Demonstrates all request types defined in the ICD:
//    1. Scheduled task (with start/end time)
//    2. Continuous task
//    3. Scan / dwell plan task
//    4. Snapshot (FFT survey)
//    5. Triggered capture
//    6. Health query
//    7. Task stop / cancel
//
//  Build: see client/CMakeLists.txt
//  Run:   ./sdr_client [broker_url] [dest_ip]
// ════════════════════════════════════════════════════════════════════════
#include <proton/container.hpp>
#include <proton/message.hpp>
#include <proton/messaging_handler.hpp>
#include <proton/connection_options.hpp>
#include <proton/sender.hpp>
#include <proton/receiver.hpp>
#include <proton/delivery.hpp>
#include <proton/transport.hpp>
#include <proton/work_queue.hpp>
#include <nlohmann/json.hpp>
#include <uuid/uuid.h>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <map>
#include <functional>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;
using namespace std::chrono;

// ─── Utilities ───────────────────────────────────────────────────────────
static std::string genUuid() {
    uuid_t uu; uuid_generate_random(uu);
    char buf[37]; uuid_unparse_lower(uu, buf);
    return buf;
}
static int64_t epochMs() {
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
static std::string envelope(const std::string& msg_type,
                              const std::string& req_id = "") {
    return ""; // placeholder — fields added inline below
}

// ─── Message builders ────────────────────────────────────────────────────
static std::string buildScheduledTask(const std::string& dest_ip) {
    std::string req_id = genUuid();
    int64_t now   = epochMs();
    int64_t start = now + 10000;   // 10s from now
    int64_t stop  = now + 70000;   // 60s window

    return json{
        {"msg_type",       "TASK_REQUEST_SCHEDULED"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   now},
        {"request_id",     req_id},
        {"correlation_id", "df-scheduled-001"},
        {"task_type",      "DF"},
        {"priority",       7},
        {"schedule", {
            {"mode",                "SCHEDULED"},
            {"start_time_epoch_ms", start},
            {"end_time_epoch_ms",   stop}
        }},
        {"rf", {
            {"center_freq_hz",  915000000.0},
            {"bandwidth_hz",    10000000.0},
            {"sample_rate_sps", 10000000.0},
            {"rx_count",        2},
            {"tx_count",        0},
            {"rx_gain_db",      {30.0, 30.0}},
            {"rx_agc",          {false, false}}
        }},
        {"streaming", {
            {"dest_ip",    dest_ip},
            {"dest_ports", {5000, 5001}}
        }},
        {"task_params", {
            {"df", {
                {"algorithm",       "MUSIC"},
                {"num_sources",     1},
                {"snapshot_count",  1024},
                {"angular_res_deg", 1.0}
            }}
        }}
    }.dump(2);
}

static std::string buildContinuousTask(const std::string& dest_ip) {
    return json{
        {"msg_type",       "TASK_REQUEST_CONTINUOUS"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   epochMs()},
        {"request_id",     genUuid()},
        {"correlation_id", "nb-continuous-001"},
        {"task_type",      "NARROWBAND"},
        {"priority",       3},
        {"schedule", {{"mode","CONTINUOUS"}}},
        {"rf", {
            {"center_freq_hz",  162400000.0},
            {"bandwidth_hz",    200000.0},
            {"sample_rate_sps", 250000.0},
            {"rx_count",        1},
            {"tx_count",        0},
            {"rx_gain_db",      {40.0}}
        }},
        {"streaming", {
            {"dest_ip",    dest_ip},
            {"dest_ports", {5100}}
        }},
        {"task_params", {
            {"narrowband", {
                {"demod",        "FM"},
                {"squelch_dbfs", -75.0}
            }}
        }}
    }.dump(2);
}

static std::string buildScanTask(const std::string& dest_ip) {
    return json{
        {"msg_type",       "TASK_REQUEST_SCAN"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   epochMs()},
        {"request_id",     genUuid()},
        {"correlation_id", "scan-survey-001"},
        {"task_type",      "WIDEBAND"},
        {"priority",       2},
        {"schedule", {{"mode","CONTINUOUS"}}},
        {"scan", {
            {"repeat",   true},
            {"rx_count", 1},
            {"tx_count", 0},
            {"rx_gain_db", {30.0}},
            {"entries", {
                {{"step",1},{"center_freq_hz",915000000.0},
                 {"bandwidth_hz",5000000.0},{"sample_rate_sps",5000000.0},
                 {"dwell_ms",1000}},
                {{"step",2},{"center_freq_hz",2400000000.0},
                 {"bandwidth_hz",10000000.0},{"sample_rate_sps",10000000.0},
                 {"dwell_ms",2000}},
                {{"step",3},{"center_freq_hz",433920000.0},
                 {"bandwidth_hz",2000000.0},{"sample_rate_sps",2000000.0},
                 {"dwell_ms",500}}
            }}
        }},
        {"streaming", {
            {"dest_ip",    dest_ip},
            {"dest_ports", {5200}}
        }}
    }.dump(2);
}

static std::string buildSnapshot() {
    return json{
        {"msg_type",       "TASK_REQUEST_SNAPSHOT"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   epochMs()},
        {"request_id",     genUuid()},
        {"correlation_id", "snapshot-001"},
        {"snapshot", {
            {"center_freq_hz",   2400000000.0},
            {"bandwidth_hz",     20000000.0},
            {"sample_rate_sps",  20000000.0},
            {"fft_size",         4096},
            {"n_averages",       16},
            {"preferred_device", ""}
        }}
    }.dump(2);
}

static std::string buildTriggered(const std::string& dest_ip) {
    return json{
        {"msg_type",       "TASK_REQUEST_TRIGGERED"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   epochMs()},
        {"request_id",     genUuid()},
        {"correlation_id", "trig-001"},
        {"task_type",      "NARROWBAND"},
        {"schedule", {{"mode","CONTINUOUS"}}},
        {"rf", {
            {"center_freq_hz",  433920000.0},
            {"bandwidth_hz",    2000000.0},
            {"sample_rate_sps", 2000000.0},
            {"rx_count",        1},
            {"tx_count",        0},
            {"rx_gain_db",      {50.0}}
        }},
        {"trigger", {
            {"type",            "POWER_THRESHOLD"},
            {"threshold_dbfs",  -60.0},
            {"pre_trigger_ms",  50},
            {"post_trigger_ms", 200},
            {"max_captures",    0}
        }},
        {"streaming", {
            {"dest_ip",    dest_ip},
            {"dest_ports", {5300}}
        }}
    }.dump(2);
}

static std::string buildStop(const std::string& task_id) {
    return json{
        {"msg_type",       "TASK_STOP"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   epochMs()},
        {"request_id",     genUuid()},
        {"task_id",        task_id},
        {"reason",         "Client stop request"}
    }.dump(2);
}

static std::string buildCancel(const std::string& task_id) {
    return json{
        {"msg_type",       "TASK_CANCEL"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   epochMs()},
        {"request_id",     genUuid()},
        {"task_id",        task_id},
        {"reason",         "Client cancel"}
    }.dump(2);
}

static std::string buildHealthQuery() {
    return json{
        {"msg_type",       "HEALTH_QUERY"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   epochMs()},
        {"request_id",     genUuid()}
    }.dump(2);
}

static std::string buildTempQuery() {
    return json{
        {"msg_type",       "DEVICE_TEMP_QUERY"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   epochMs()},
        {"request_id",     genUuid()}
    }.dump(2);
}

// ─── Client messaging handler ────────────────────────────────────────────
class ClientHandler : public proton::messaging_handler {
public:
    ClientHandler(const std::string& url,
                  const std::string& username,
                  const std::string& password,
                  const std::string& dest_ip,
                  const std::string& req_q,
                  const std::string& resp_q)
        : url_(url), username_(username), password_(password),
          dest_ip_(dest_ip), req_q_(req_q), resp_q_(resp_q) {}

    ~ClientHandler() {
        if (demo_thread_.joinable()) demo_thread_.join();
    }

    void on_container_start(proton::container& c) override {
        container_ = &c;
        proton::connection_options opts;
        if (!username_.empty()) {
            opts.sasl_allowed_mechs("PLAIN");
            opts.sasl_allow_insecure_mechs(true);
            opts.user(username_).password(password_);
        } else {
            opts.sasl_allowed_mechs("ANONYMOUS");
        }
        c.connect(url_, opts);
    }

    void on_connection_open(proton::connection& c) override {
        std::cout << "[CLIENT] Connected to " << url_ << std::endl;
        sender_   = c.open_sender(req_q_);
        receiver_ = c.open_receiver(resp_q_);
        // Run demo sequence on a background thread
        demo_thread_ = std::thread([this]() {
            try { runDemo(); }
            catch (const std::exception& ex) {
                std::cerr << "[CLIENT] demo error: " << ex.what() << std::endl;
            }
            if (container_) container_->stop();
        });
    }

    void on_message(proton::delivery&, proton::message& msg) override {
        try {
            std::string body = proton::get<std::string>(msg.body());
            auto j = json::parse(body);
            std::string mt = j.value("msg_type","");
            std::cout << "\n[RESPONSE] msg_type=" << mt << std::endl;

            if (mt == "TASK_RESPONSE") {
                std::string status = j.value("status","");
                std::string tid    = j.value("task_id","");
                std::cout << "  status=" << status << " task_id=" << tid << std::endl;
                if (status == "ACCEPTED" && j.contains("streams")) {
                    for (auto& s : j["streams"]) {
                        std::cout << "  STREAM: " << s["stream_id"].get<std::string>()
                                  << " → " << s["udp_ip"].get<std::string>()
                                  << ":" << s["udp_port"].get<int>()
                                  << " ch=" << s["channel_index"].get<int>()
                                  << " cf=" << s["center_freq_hz"].get<double>()/1e6 << "MHz"
                                  << std::endl;
                    }
                    // Store last accepted task_id for STOP
                    last_task_id_ = tid;
                } else if (status == "REJECTED") {
                    std::cout << "  REJECT: " << j.value("reject_code","")
                              << " — " << j.value("reject_reason","") << std::endl;
                }
            } else if (mt == "HEALTH_QUERY_RESPONSE") {
                std::cout << "  HEALTH: " << body.substr(0,200) << "..." << std::endl;
            } else if (mt == "DEVICE_TEMP_RESPONSE") {
                std::cout << "  TEMPERATURES:" << std::endl;
                for (auto& dev : j.value("devices", json::array())) {
                    std::string did = dev.value("device_id","?");
                    bool online     = dev.value("online", false);
                    if (!online) { std::cout << "    " << did << ": offline" << std::endl; continue; }
                    for (auto& s : dev.value("sensors", json::array())) {
                        if (s["value_c"].is_null())
                            std::cout << "    " << did << " / " << s["name"] << ": (read error)" << std::endl;
                        else
                            std::cout << "    " << did << " / " << s["name"]
                                      << ": " << s["value_c"].get<double>() << " °C" << std::endl;
                    }
                }
            } else {
                std::cout << "  " << body.substr(0, 300) << std::endl;
            }
        } catch (const std::exception& ex) {
            std::cerr << "[CLIENT] on_message error: " << ex.what() << std::endl;
        }
    }

    void on_transport_error(proton::transport& t) override {
        std::cerr << "[CLIENT] transport error: " << t.error().what() << std::endl;
        done_.store(true);
        if (container_) container_->stop();
    }

private:
    void send(const std::string& body) {
        sender_.work_queue().add([this, body]() {
            if (!sender_ || !sender_.credit()) return;
            proton::message m;
            m.body(body);
            m.content_type("application/json");
            sender_.send(m);
        });
    }

    void runDemo() {
        // Wait for sender credit
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::cout << "\n════ SDR Client Demo ════\n" << std::endl;

        // 1. Health query
        std::cout << "[SEND] HEALTH_QUERY" << std::endl;
        send(buildHealthQuery());
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // 2. Temperature query
        std::cout << "[SEND] DEVICE_TEMP_QUERY" << std::endl;
        send(buildTempQuery());
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // 3. Scheduled DF task
        std::cout << "\n[SEND] TASK_REQUEST_SCHEDULED (DF, 2 RX, 60s window)" << std::endl;
        send(buildScheduledTask(dest_ip_));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // 3. Continuous narrowband
        std::cout << "\n[SEND] TASK_REQUEST_CONTINUOUS (NB FM)" << std::endl;
        send(buildContinuousTask(dest_ip_));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        // 4. Stop the continuous task
        if (!last_task_id_.empty()) {
            std::cout << "\n[SEND] TASK_STOP for " << last_task_id_ << std::endl;
            send(buildStop(last_task_id_));
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }

        // 5. Scan plan
        std::cout << "\n[SEND] TASK_REQUEST_SCAN (3-step dwell plan)" << std::endl;
        send(buildScanTask(dest_ip_));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        if (!last_task_id_.empty()) {
            std::cout << "\n[SEND] TASK_STOP scan" << std::endl;
            send(buildStop(last_task_id_));
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // 6. Snapshot
        std::cout << "\n[SEND] TASK_REQUEST_SNAPSHOT (FFT survey, 2.4 GHz)" << std::endl;
        send(buildSnapshot());
        std::this_thread::sleep_for(std::chrono::seconds(3));

        // 7. Triggered capture
        std::cout << "\n[SEND] TASK_REQUEST_TRIGGERED (433 MHz, power threshold)" << std::endl;
        send(buildTriggered(dest_ip_));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        if (!last_task_id_.empty()) {
            std::cout << "\n[SEND] TASK_CANCEL triggered task" << std::endl;
            send(buildCancel(last_task_id_));
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::cout << "\n════ Demo complete ════\n" << std::endl;
        done_.store(true);
    }

    std::string   url_, username_, password_, dest_ip_, req_q_, resp_q_;
    proton::container* container_ = nullptr;
    proton::sender   sender_;
    proton::receiver receiver_;
    std::thread      demo_thread_;
    std::atomic<bool> done_{false};
    std::string      last_task_id_;
};

// ─── main ─────────────────────────────────────────────────────────────────
// Usage: sdr_client [url] [username] [password] [dest_ip]
int main(int argc, char* argv[]) {
    std::string broker_url = "amqp://localhost:5672";
    std::string username   = "sdr_ctrl";
    std::string password   = "sdr_test_pw";
    std::string dest_ip    = "127.0.0.1";

    if (argc >= 2) broker_url = argv[1];
    if (argc >= 3) username   = argv[2];
    if (argc >= 4) password   = argv[3];
    if (argc >= 5) dest_ip    = argv[4];

    std::cout << "SDR Client — broker=" << broker_url
              << " user=" << username
              << " dest_ip=" << dest_ip << std::endl;

    try {
        ClientHandler handler(broker_url, username, password, dest_ip,
                              "sdr.task.request",
                              "sdr.task.response");
        proton::container c(handler);
        c.run();
    } catch (const std::exception& ex) {
        std::cerr << "Fatal: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
