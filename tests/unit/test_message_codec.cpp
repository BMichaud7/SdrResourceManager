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
#include <gtest/gtest.h>
#include "sdr/MessageCodec.hpp"
#include "sdr/Types.hpp"
#include <nlohmann/json.hpp>
#include <chrono>

using namespace sdr;
using json = nlohmann::json;

static int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// ── Decode tests ──────────────────────────────────────────────────────────────

TEST(MessageCodec, DecodeHealthQuery) {
    std::string body = json{
        {"msg_type",       "HEALTH_QUERY"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   nowMs()},
        {"request_id",     "req-health-001"}
    }.dump();

    auto req = MessageCodec::decode(body);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->msg_type,   "HEALTH_QUERY");
    EXPECT_EQ(req->request_id, "req-health-001");
}

TEST(MessageCodec, DecodeTaskRequestScheduled) {
    int64_t start = nowMs() + 10'000;
    int64_t stop  = start  + 60'000;
    std::string body = json{
        {"msg_type",       "TASK_REQUEST_SCHEDULED"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   nowMs()},
        {"request_id",     "req-sched-001"},
        {"correlation_id", "corr-abc"},
        {"task_type",      "DF"},
        {"priority",       7},
        {"rank",           2},
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
            {"rx_gain_db",      {30.0, 32.0}},
            {"rx_agc",          {false, true}}
        }},
        {"streaming", {
            {"dest_ip",    "10.0.1.10"},
            {"dest_ports", {5000, 5001}}
        }},
        {"task_params", {
            {"df", {{"algorithm","MUSIC"},{"num_sources",1},
                    {"snapshot_count",512},{"angular_res_deg",0.5}}}
        }}
    }.dump();

    auto req = MessageCodec::decode(body);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->msg_type,       "TASK_REQUEST_SCHEDULED");
    EXPECT_EQ(req->correlation_id, "corr-abc");
    EXPECT_EQ(req->priority,       7);
    EXPECT_EQ(req->schedule_mode,  ScheduleMode::SCHEDULED);
    EXPECT_EQ(req->start_time_ms,  start);
    EXPECT_EQ(req->end_time_ms,    stop);
    EXPECT_EQ(req->task_type,      TaskType::DF);
    EXPECT_DOUBLE_EQ(req->rf.center_freq_hz, 915e6);
    EXPECT_DOUBLE_EQ(req->rf.bandwidth_hz,   10e6);
    EXPECT_EQ(req->rf.rx_count, 2);
    EXPECT_EQ(req->rf.rx_gain_db, (std::vector<double>{30.0, 32.0}));
    EXPECT_EQ(req->streaming.dest_ip, "10.0.1.10");
    EXPECT_EQ(req->streaming.dest_ports, (std::vector<int>{5000, 5001}));
    ASSERT_TRUE(req->df_params.has_value());
    EXPECT_EQ(req->df_params->algorithm,      "MUSIC");
    EXPECT_EQ(req->df_params->snapshot_count, 512);
}

TEST(MessageCodec, DecodeTaskRequestContinuous) {
    std::string body = json{
        {"msg_type",       "TASK_REQUEST_CONTINUOUS"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   nowMs()},
        {"request_id",     "req-cont-001"},
        {"task_type",      "NARROWBAND"},
        {"rank",           0},
        {"schedule",       {{"mode","CONTINUOUS"}}},
        {"rf", {
            {"center_freq_hz",  162400000.0},
            {"bandwidth_hz",    200000.0},
            {"sample_rate_sps", 250000.0},
            {"rx_count",        1},
            {"tx_count",        0}
        }},
        {"streaming", {{"dest_ip","10.0.1.11"},{"dest_ports",{5100}}}},
        {"task_params", {
            {"narrowband", {{"demod","FM"},{"squelch_dbfs",-70.0}}}
        }}
    }.dump();

    auto req = MessageCodec::decode(body);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->schedule_mode, ScheduleMode::CONTINUOUS);
    EXPECT_EQ(req->task_type,     TaskType::NARROWBAND);
    EXPECT_DOUBLE_EQ(req->rf.center_freq_hz, 162.4e6);
    ASSERT_TRUE(req->nb_params.has_value());
    EXPECT_EQ(req->nb_params->demod, "FM");
    EXPECT_DOUBLE_EQ(req->nb_params->squelch_dbfs, -70.0);
}

TEST(MessageCodec, DecodeTaskRequestScan) {
    std::string body = json{
        {"msg_type",       "TASK_REQUEST_SCAN"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   nowMs()},
        {"request_id",     "req-scan-001"},
        {"task_type",      "WIDEBAND"},
        {"rank",           1},
        {"schedule",       {{"mode","CONTINUOUS"}}},
        {"scan", {
            {"repeat",   true},
            {"rx_count", 1},
            {"tx_count", 0},
            {"entries", {
                {{"step",1},{"center_freq_hz",915e6},{"bandwidth_hz",5e6},
                 {"sample_rate_sps",5e6},{"dwell_ms",1000}},
                {{"step",2},{"center_freq_hz",433.92e6},{"bandwidth_hz",2e6},
                 {"sample_rate_sps",2e6},{"dwell_ms",500}}
            }}
        }},
        {"streaming", {{"dest_ip","10.0.1.12"},{"dest_ports",{5200}}}}
    }.dump();

    auto req = MessageCodec::decode(body);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->task_type, TaskType::SCAN);
    ASSERT_TRUE(req->scan_params.has_value());
    ASSERT_EQ(req->scan_params->entries.size(), 2u);
    EXPECT_TRUE(req->scan_params->repeat);
    EXPECT_DOUBLE_EQ(req->scan_params->entries[0].center_freq_hz, 915e6);
    EXPECT_EQ(req->scan_params->entries[1].dwell_ms, 500);
}

TEST(MessageCodec, DecodeTaskRequestSnapshot) {
    std::string body = json{
        {"msg_type",       "TASK_REQUEST_SNAPSHOT"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   nowMs()},
        {"request_id",     "req-snap-001"},
        {"rank",           0},
        {"snapshot", {
            {"center_freq_hz",  2400e6},
            {"bandwidth_hz",    20e6},
            {"sample_rate_sps", 20e6},
            {"fft_size",        4096},
            {"n_averages",      16},
            {"preferred_device","fake-0"}
        }}
    }.dump();

    auto req = MessageCodec::decode(body);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->msg_type,  "TASK_REQUEST_SNAPSHOT");
    EXPECT_EQ(req->task_type, TaskType::SNAPSHOT);
    ASSERT_TRUE(req->snapshot_params.has_value());
    EXPECT_DOUBLE_EQ(req->snapshot_params->center_freq_hz, 2400e6);
    EXPECT_EQ(req->snapshot_params->fft_size, 4096);
    EXPECT_EQ(req->snapshot_params->preferred_device, "fake-0");
}

TEST(MessageCodec, DecodeTaskStop) {
    std::string body = json{
        {"msg_type",     "TASK_STOP"},
        {"schema_version","2.0"},
        {"timestamp_ms", nowMs()},
        {"request_id",   "req-stop-001"},
        {"task_id",      "some-task-uuid"},
        {"reason",       "test done"}
    }.dump();

    auto req = MessageCodec::decode(body);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->msg_type, "TASK_STOP");
    EXPECT_EQ(req->task_id,  "some-task-uuid");
    EXPECT_EQ(req->reason,   "test done");
}

TEST(MessageCodec, MissingRequestIdReturnsNullopt) {
    std::string body = json{
        {"msg_type",       "HEALTH_QUERY"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   nowMs()}
    }.dump();

    auto req = MessageCodec::decode(body);
    EXPECT_FALSE(req.has_value());
}

TEST(MessageCodec, MalformedJsonReturnsNullopt) {
    auto req = MessageCodec::decode("{not valid json");
    EXPECT_FALSE(req.has_value());
}

TEST(MessageCodec, PeekMsgType) {
    std::string body = json{{"msg_type","TASK_STOP"},{"request_id","x"}}.dump();
    EXPECT_EQ(MessageCodec::peekMsgType(body), "TASK_STOP");
    EXPECT_EQ(MessageCodec::peekMsgType("{bad"), "");
}

// ── Encode tests ──────────────────────────────────────────────────────────────

TEST(MessageCodec, EncodeTaskResponseAccepted) {
    TaskResponse resp;
    resp.request_id     = "req-001";
    resp.correlation_id = "corr-001";
    resp.accepted       = true;
    resp.task_id        = "task-uuid-001";
    resp.schedule_mode  = "CONTINUOUS";
    resp.actual_start_ms= 1000;
    resp.actual_stop_ms = TIME_INFINITE;

    AssignedStream s;
    s.stream_id       = "task-uuid-001-RX-fake-0-0";
    s.device_id       = "fake-0";
    s.channel_type    = "RX";
    s.channel_index   = 0;
    s.udp_ip          = "10.0.0.10";
    s.udp_port        = 30000;
    s.center_freq_hz  = 915e6;
    s.sample_rate_sps = 10e6;
    s.format          = "CF32";
    resp.streams.push_back(s);

    auto j = json::parse(MessageCodec::encodeTaskResponse(resp));
    EXPECT_EQ(j["msg_type"], "TASK_RESPONSE");
    EXPECT_EQ(j["status"],   "ACCEPTED");
    EXPECT_EQ(j["task_id"],  "task-uuid-001");
    ASSERT_TRUE(j.contains("streams"));
    ASSERT_EQ(j["streams"].size(), 1u);
    EXPECT_EQ(j["streams"][0]["udp_port"],  30000);
    EXPECT_EQ(j["streams"][0]["device_id"], "fake-0");
    EXPECT_EQ(j["streams"][0]["format"],    "CF32");
}

TEST(MessageCodec, EncodeTaskResponseRejected) {
    TaskResponse resp;
    resp.request_id    = "req-002";
    resp.accepted      = false;
    resp.reject_code   = RejectCode::SPECTRUM_CONFLICT;
    resp.reject_reason = "No slice available";

    auto j = json::parse(MessageCodec::encodeTaskResponse(resp));
    EXPECT_EQ(j["status"],        "REJECTED");
    EXPECT_EQ(j["reject_code"],   "SPECTRUM_CONFLICT");
    EXPECT_EQ(j["reject_reason"], "No slice available");
    EXPECT_FALSE(j.contains("streams"));
}

TEST(MessageCodec, EncodeSnapshotResult) {
    SnapshotResult r;
    r.device_id          = "fake-0";
    r.center_freq_hz     = 2400e6;
    r.fft_size           = 4096;
    r.n_averages         = 16;
    r.freq_resolution_hz = 4882.8;
    r.power_bins         = {-80.0, -81.0, -79.0};
    r.success            = true;

    auto j = json::parse(MessageCodec::encodeSnapshotResult("req-snap", r));
    EXPECT_EQ(j["msg_type"],  "SNAPSHOT_RESULT");
    EXPECT_EQ(j["status"],    "COMPLETED");
    EXPECT_EQ(j["device_id"], "fake-0");
    EXPECT_EQ(j["fft_size"],  4096);
    ASSERT_EQ(j["power_bins"].size(), 3u);
}

// ── rank field ────────────────────────────────────────────────────────────────

TEST(MessageCodec, DecodeRankPropagates) {
    std::string body = json{
        {"msg_type",       "TASK_REQUEST_SCHEDULED"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   nowMs()},
        {"request_id",     "req-rank-rm"},
        {"task_type",      "DF"},
        {"priority",       8},
        {"rank",           2},
        {"schedule",       {{"mode","SCHEDULED"},
                            {"start_time_epoch_ms", nowMs()+5000},
                            {"end_time_epoch_ms",   nowMs()+65000}}},
        {"rf", {{"center_freq_hz",915e6},{"bandwidth_hz",10e6},
                {"sample_rate_sps",10e6},{"rx_count",1},{"tx_count",0}}},
        {"streaming", {{"dest_ip","10.0.0.1"},{"dest_ports",{5000}}}}
    }.dump();

    auto req = MessageCodec::decode(body);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->priority, 8);
    EXPECT_EQ(req->rank,     2);
}

TEST(MessageCodec, DecodeRankMissingDefaultsToZero) {
    // rank is optional — omitting it defaults to 0 (lowest priority)
    std::string body = json{
        {"msg_type",       "TASK_REQUEST_SCHEDULED"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   nowMs()},
        {"request_id",     "req-rank-missing"},
        {"task_type",      "DF"},
        {"schedule",       {{"mode","IMMEDIATE"}}},
        {"rf", {{"center_freq_hz",915e6},{"bandwidth_hz",10e6},
                {"sample_rate_sps",10e6},{"rx_count",1},{"tx_count",0}}},
        {"streaming", {{"dest_ip","10.0.0.1"},{"dest_ports",{5000}}}}
    }.dump();

    auto req = MessageCodec::decode(body);
    ASSERT_TRUE(req.has_value()) << "Missing rank must be accepted (defaults to 0)";
    EXPECT_EQ(req->rank, 0);
}

TEST(MessageCodec, EncodeTaskStatusHasRank) {
    TaskRecord rec;
    rec.task_id      = "12345678";
    rec.task_type    = TaskType::DF;
    rec.schedule_mode= ScheduleMode::SCHEDULED;
    rec.state        = TaskState::RUNNING;
    rec.priority     = 8;
    rec.rank         = 2;
    rec.start_time_ms= nowMs();
    rec.stop_time_ms = TIME_INFINITE;

    auto j = json::parse(MessageCodec::encodeTaskStatus(rec, {}));
    EXPECT_EQ(j["priority"], 8);
    EXPECT_EQ(j["rank"],     2);
}

// ── scan_params key (canonical) ───────────────────────────────────────────────

// ── Additional edge cases ─────────────────────────────────────────────────────

TEST(MessageCodec, DecodeEmptyBodyReturnsNullopt) {
    EXPECT_FALSE(MessageCodec::decode("").has_value());
}

TEST(MessageCodec, DecodeUnknownMsgTypeIsDecodable) {
    std::string body = json{
        {"msg_type",       "FUTURE_MSG"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   nowMs()},
        {"request_id",     "req-future"},
        {"rank",           0}
    }.dump();
    auto req = MessageCodec::decode(body);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->msg_type, "FUTURE_MSG");
    EXPECT_EQ(req->request_id, "req-future");
}

TEST(MessageCodec, EncodeTaskStatusTerminalReason) {
    TaskRecord rec;
    rec.task_id         = "task-failed-001";
    rec.task_type       = TaskType::NARROWBAND;
    rec.schedule_mode   = ScheduleMode::CONTINUOUS;
    rec.state           = TaskState::FAILED;
    rec.priority        = 5;
    rec.rank            = 1;
    rec.start_time_ms   = nowMs();
    rec.stop_time_ms    = nowMs();
    rec.terminal_reason = "device error";

    auto j = json::parse(MessageCodec::encodeTaskStatus(rec, {}));
    EXPECT_EQ(j["state"],           "FAILED");
    EXPECT_EQ(j["terminal_reason"], "device error");
}

TEST(MessageCodec, EncodeTaskStatusCompletedState) {
    TaskRecord rec;
    rec.task_id       = "task-done-001";
    rec.task_type     = TaskType::WIDEBAND;
    rec.schedule_mode = ScheduleMode::SCHEDULED;
    rec.state         = TaskState::COMPLETED;
    rec.priority      = 3;
    rec.rank          = 0;
    rec.start_time_ms = nowMs();
    rec.stop_time_ms  = nowMs();

    auto j = json::parse(MessageCodec::encodeTaskStatus(rec, {}));
    EXPECT_EQ(j["state"], "COMPLETED");
    EXPECT_EQ(j["task_type"], "WIDEBAND");
}

TEST(MessageCodec, DecodeScanWithScanParamsKey) {
    std::string body = json{
        {"msg_type",       "TASK_REQUEST_SCAN"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   nowMs()},
        {"request_id",     "req-scan-canonical"},
        {"task_type",      "SCAN"},
        {"rank",           0},
        {"schedule",       {{"mode","CONTINUOUS"}}},
        {"rf", {{"center_freq_hz",433.92e6},{"bandwidth_hz",2e6},
                {"sample_rate_sps",2e6},{"rx_count",1},{"tx_count",0},
                {"rx_gain_db",{30.0}}}},
        {"streaming", {{"dest_ip","127.0.0.1"},{"dest_ports",{5100}}}},
        {"scan_params", {
            {"repeat", true},
            {"entries", {
                {{"step",0},{"center_freq_hz",433.92e6},{"bandwidth_hz",2e6},
                 {"sample_rate_sps",2e6},{"dwell_ms",500}}
            }}
        }}
    }.dump();

    auto req = MessageCodec::decode(body);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->task_type, TaskType::SCAN);
    ASSERT_TRUE(req->scan_params.has_value());
    ASSERT_EQ(req->scan_params->entries.size(), 1u);
    EXPECT_DOUBLE_EQ(req->scan_params->entries[0].center_freq_hz, 433.92e6);
}

// ── DEVICE_TEMP_QUERY / DEVICE_TEMP_RESPONSE tests ───────────────────────────

TEST(MessageCodec, DecodeTempQuery) {
    std::string body = json{
        {"msg_type",    "DEVICE_TEMP_QUERY"},
        {"request_id",  "req-temp-1"}
    }.dump();
    auto req = MessageCodec::decode(body);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->msg_type,    "DEVICE_TEMP_QUERY");
    EXPECT_EQ(req->request_id,  "req-temp-1");
}

TEST(MessageCodec, EncodeTempResponse_AllValid) {
    std::vector<MessageCodec::TempEntry> devs;
    MessageCodec::TempEntry e;
    e.device_id = "pluto-0";
    e.online    = true;
    e.sensors.push_back({"temp0", 43.25, true});
    devs.push_back(std::move(e));

    auto body = MessageCodec::encodeTempResponse("req-temp-1", devs);
    auto j    = json::parse(body);

    EXPECT_EQ(j["msg_type"],    "DEVICE_TEMP_RESPONSE");
    EXPECT_EQ(j["request_id"],  "req-temp-1");
    ASSERT_EQ(j["devices"].size(), 1u);
    auto& dev = j["devices"][0];
    EXPECT_EQ(dev["device_id"], "pluto-0");
    EXPECT_TRUE(dev["online"].get<bool>());
    ASSERT_EQ(dev["sensors"].size(), 1u);
    EXPECT_EQ(dev["sensors"][0]["name"],    "temp0");
    EXPECT_NEAR(dev["sensors"][0]["value_c"].get<double>(), 43.25, 0.01);
}

TEST(MessageCodec, EncodeTempResponse_OfflineDevice) {
    std::vector<MessageCodec::TempEntry> devs;
    MessageCodec::TempEntry e;
    e.device_id = "pluto-1";
    e.online    = false;
    // No sensors for offline device
    devs.push_back(std::move(e));

    auto body = MessageCodec::encodeTempResponse("req-temp-2", devs);
    auto j    = json::parse(body);

    ASSERT_EQ(j["devices"].size(), 1u);
    EXPECT_FALSE(j["devices"][0]["online"].get<bool>());
    EXPECT_TRUE(j["devices"][0]["sensors"].empty());
}

TEST(MessageCodec, EncodeTempResponse_FailedSensor_NullValue) {
    std::vector<MessageCodec::TempEntry> devs;
    MessageCodec::TempEntry e;
    e.device_id = "pluto-0";
    e.online    = true;
    e.sensors.push_back({"temp0", 0.0, false}); // valid=false → null in JSON
    devs.push_back(std::move(e));

    auto body = MessageCodec::encodeTempResponse("req-temp-3", devs);
    auto j    = json::parse(body);

    auto& sensor = j["devices"][0]["sensors"][0];
    EXPECT_EQ(sensor["name"], "temp0");
    EXPECT_TRUE(sensor["value_c"].is_null()) << "Failed sensor must encode as null";
}

TEST(MessageCodec, EncodeTempResponse_MultipleDevicesAndSensors) {
    std::vector<MessageCodec::TempEntry> devs;
    {
        MessageCodec::TempEntry e;
        e.device_id = "dev-0"; e.online = true;
        e.sensors.push_back({"temp0", 38.0, true});
        e.sensors.push_back({"temp1", 52.1, true});
        devs.push_back(std::move(e));
    }
    {
        MessageCodec::TempEntry e;
        e.device_id = "dev-1"; e.online = true;
        e.sensors.push_back({"temp0", 41.7, true});
        devs.push_back(std::move(e));
    }

    auto j = json::parse(MessageCodec::encodeTempResponse("req-multi", devs));
    ASSERT_EQ(j["devices"].size(), 2u);
    EXPECT_EQ(j["devices"][0]["sensors"].size(), 2u);
    EXPECT_EQ(j["devices"][1]["sensors"].size(), 1u);
    EXPECT_NEAR(j["devices"][0]["sensors"][1]["value_c"].get<double>(), 52.1, 0.01);
}
