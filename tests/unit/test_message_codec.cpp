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

TEST(MessageCodec, DecodeRankDefaultsToZero) {
    std::string body = json{
        {"msg_type",       "TASK_REQUEST_SCHEDULED"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   nowMs()},
        {"request_id",     "req-rank-default"},
        {"task_type",      "DF"},
        {"schedule",       {{"mode","IMMEDIATE"}}},
        {"rf", {{"center_freq_hz",915e6},{"bandwidth_hz",10e6},
                {"sample_rate_sps",10e6},{"rx_count",1},{"tx_count",0}}},
        {"streaming", {{"dest_ip","10.0.0.1"},{"dest_ports",{5000}}}}
    }.dump();

    auto req = MessageCodec::decode(body);
    ASSERT_TRUE(req.has_value());
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

TEST(MessageCodec, DecodeScanWithScanParamsKey) {
    std::string body = json{
        {"msg_type",       "TASK_REQUEST_SCAN"},
        {"schema_version", "2.0"},
        {"timestamp_ms",   nowMs()},
        {"request_id",     "req-scan-canonical"},
        {"task_type",      "SCAN"},
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
