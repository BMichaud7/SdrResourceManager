#include <gtest/gtest.h>
#include "ResourceManager.hpp"
#include "sdr/Types.hpp"
#include "FakeSoapyControl.hpp"
#include <chrono>
#include <thread>

using namespace sdr;

// ── Helpers ───────────────────────────────────────────────────────────────────

static int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static AppConfig makeTestConfig(int max_tasks = 10, int num_devices = 1) {
    AppConfig cfg;
    cfg.policy.max_concurrent_tasks    = max_tasks;
    cfg.policy.guard_band_hz           = 200e3;
    cfg.policy.usable_bw_fraction      = 0.80;
    cfg.policy.default_task_timeout_ms = 60'000;
    cfg.policy.scheduler_tick_ms       = 200;
    cfg.policy.watchdog_tick_ms        = 500;
    cfg.policy.udp_port_pool_start     = 45000;
    cfg.policy.udp_port_pool_end       = 45099;
    cfg.policy.iq_packet_samples       = 128;
    cfg.policy.retune_conflict_policy  = RetuneConflictPolicy::REJECT_NEW;
    cfg.policy.heartbeat_interval_ms   = 10'000;

    for (int i = 0; i < num_devices; ++i) {
        DeviceConfig dc;
        dc.id                  = "fake-" + std::to_string(i);
        dc.driver              = "fake";
        dc.uri                 = "fake";
        dc.label               = "Fake SDR " + std::to_string(i);
        dc.streaming_source_ip = "127.0.0.1";
        dc.coherency_group     = "test-group";
        dc.caps.rx_channels         = 2;
        dc.caps.tx_channels         = 2;
        dc.caps.freq_min_hz         = 70e6;
        dc.caps.freq_max_hz         = 6e9;
        dc.caps.bandwidth_max_hz    = 56e6;
        dc.caps.sample_rate_max_sps = 61.44e6;
        dc.caps.rx_gain_min_db      = -3;
        dc.caps.rx_gain_max_db      = 71;
        dc.caps.tx_atten_min_db     = 0;
        dc.caps.tx_atten_max_db     = 89;
        cfg.devices.push_back(dc);
    }
    return cfg;
}

static TaskRequest makeScheduled(const std::string& req_id,
                                  double cf, double bw, double sr, int rx,
                                  int64_t start_offset_ms = 60'000,
                                  int64_t duration_ms     = 60'000) {
    TaskRequest r;
    r.msg_type           = "TASK_REQUEST_SCHEDULED";
    r.request_id         = req_id;
    r.schema_version     = "2.0";
    r.timestamp_ms       = nowMs();
    r.task_type          = TaskType::DF;
    r.schedule_mode      = ScheduleMode::SCHEDULED;
    r.start_time_ms      = nowMs() + start_offset_ms;
    r.end_time_ms        = r.start_time_ms + duration_ms;
    r.rf.center_freq_hz  = cf;
    r.rf.bandwidth_hz    = bw;
    r.rf.sample_rate_sps = sr;
    r.rf.rx_count        = rx;
    r.rf.tx_count        = 0;
    r.streaming.dest_ip  = "127.0.0.1";
    r.streaming.dest_ports = {5000};
    return r;
}

static TaskRequest makeContinuous(const std::string& req_id,
                                   double cf, double bw, double sr, int rx) {
    TaskRequest r;
    r.msg_type           = "TASK_REQUEST_CONTINUOUS";
    r.request_id         = req_id;
    r.schema_version     = "2.0";
    r.timestamp_ms       = nowMs();
    r.task_type          = TaskType::NARROWBAND;
    r.schedule_mode      = ScheduleMode::CONTINUOUS;
    r.start_time_ms      = nowMs();
    r.end_time_ms        = TIME_INFINITE;
    r.rf.center_freq_hz  = cf;
    r.rf.bandwidth_hz    = bw;
    r.rf.sample_rate_sps = sr;
    r.rf.rx_count        = rx;
    r.rf.tx_count        = 0;
    r.streaming.dest_ip  = "127.0.0.1";
    r.streaming.dest_ports = {5000};
    return r;
}

// ── Basic ResourceManager tests ───────────────────────────────────────────────

TEST(ResourceManager, OpenDevicesSucceedsWithFakeDriver) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    EXPECT_EQ(rm.openDevices(), 1);
}

TEST(ResourceManager, TaskRejectedWhenDeviceNotOpened) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});

    auto resp = rm.tryAccept(makeScheduled("req-1", 915e6, 10e6, 10e6, 1));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::NO_DEVICE_AVAILABLE);
}

TEST(ResourceManager, ScheduledTaskAcceptedOnFreeDevice) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 915e6, 5e6, 10e6, 1));
    ASSERT_TRUE(resp.accepted);
    EXPECT_FALSE(resp.task_id.empty());
    ASSERT_EQ(resp.streams.size(), 1u);
    EXPECT_EQ(resp.streams[0].device_id,    "fake-0");
    EXPECT_EQ(resp.streams[0].channel_type, "RX");
    EXPECT_EQ(resp.streams[0].format,       "CF32");
    EXPECT_GE(resp.streams[0].udp_port, 45000);
    EXPECT_LE(resp.streams[0].udp_port, 45099);
    EXPECT_EQ(rm.udpPortsUsed(), 1);
}

TEST(ResourceManager, RejectedWithFreqOutOfRange) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 10e6, 5e6, 5e6, 1));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::FREQ_OUT_OF_RANGE);
}

TEST(ResourceManager, RejectedWithBwExceeded) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 915e6, 60e6, 60e6, 1));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::BW_EXCEEDED);
}

TEST(ResourceManager, RejectedWithSampleRateExceeded) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 915e6, 10e6, 70e6, 1)); // 70 MSPS > 61.44
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::SAMPLE_RATE_EXCEEDED);
}

TEST(ResourceManager, TwoScheduledTasksSameCfShareSpectrum) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1", 915e6, 5e6, 10e6, 1, 60'000, 60'000));
    auto r2 = rm.tryAccept(makeScheduled("req-2", 915e6, 3e6, 10e6, 1, 60'000, 60'000));
    ASSERT_TRUE(r1.accepted);
    ASSERT_TRUE(r2.accepted);
    EXPECT_NE(r2.streams[0].channel_index, r1.streams[0].channel_index);
    EXPECT_EQ(rm.udpPortsUsed(), 2);
}

TEST(ResourceManager, RetuneConflictForDifferentCfOnSameDevice) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1", 915e6,  5e6, 10e6, 1, 60'000, 60'000));
    ASSERT_TRUE(r1.accepted);

    auto r2 = rm.tryAccept(makeScheduled("req-2", 2400e6, 5e6, 20e6, 1, 60'000, 60'000));
    ASSERT_FALSE(r2.accepted);
    EXPECT_EQ(r2.reject_code, RejectCode::NO_DEVICE_AVAILABLE);
}

TEST(ResourceManager, ChannelCountExceededWhenAllRxBusy) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1", 915e6, 3e6, 10e6, 2, 60'000, 60'000));
    ASSERT_TRUE(r1.accepted);

    auto r2 = rm.tryAccept(makeScheduled("req-2", 915e6, 2e6, 10e6, 1, 60'000, 60'000));
    ASSERT_FALSE(r2.accepted);
    EXPECT_EQ(r2.reject_code, RejectCode::NO_DEVICE_AVAILABLE);
}

TEST(ResourceManager, TaskLimitReached) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(2), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1", 915e6, 3e6, 10e6, 1,  60'000, 30'000));
    auto r2 = rm.tryAccept(makeScheduled("req-2", 915e6, 3e6, 10e6, 1, 120'000, 30'000));
    ASSERT_TRUE(r1.accepted);
    ASSERT_TRUE(r2.accepted);
    EXPECT_EQ(rm.countByState(TaskState::SCHEDULED), 2);

    auto r3 = rm.tryAccept(makeScheduled("req-3", 915e6, 3e6, 10e6, 1, 200'000, 30'000));
    ASSERT_FALSE(r3.accepted);
    EXPECT_EQ(r3.reject_code, RejectCode::TASK_LIMIT_REACHED);
}

TEST(ResourceManager, ContinuousTaskAcceptRunStopPortsFreed) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeContinuous("req-1", 915e6, 5e6, 10e6, 1));
    ASSERT_TRUE(resp.accepted);
    EXPECT_EQ(rm.countByState(TaskState::RUNNING), 1);
    EXPECT_EQ(rm.udpPortsUsed(), 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto stop = rm.stopTask(resp.task_id, "req-stop", "test done");
    ASSERT_TRUE(stop.accepted);
    EXPECT_EQ(rm.countByState(TaskState::RUNNING),   0);
    EXPECT_EQ(rm.countByState(TaskState::CANCELLED), 1);
    EXPECT_EQ(rm.udpPortsUsed(), 0);
}

TEST(ResourceManager, CancelTaskOnScheduledTask) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 915e6, 5e6, 10e6, 1));
    ASSERT_TRUE(resp.accepted);
    EXPECT_EQ(rm.countByState(TaskState::SCHEDULED), 1);

    auto cancel = rm.cancelTask(resp.task_id, "req-cancel", "changed mind");
    ASSERT_TRUE(cancel.accepted);
    EXPECT_EQ(rm.countByState(TaskState::SCHEDULED), 0);
    EXPECT_EQ(rm.countByState(TaskState::CANCELLED), 1);
    EXPECT_EQ(rm.udpPortsUsed(), 0);
}

TEST(ResourceManager, TwoDevicesTasksGoToLeastLoaded) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(20, 2), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1", 915e6, 15e6, 20e6, 1, 60'000, 60'000));
    ASSERT_TRUE(r1.accepted);
    std::string dev1 = r1.streams[0].device_id;

    auto r2 = rm.tryAccept(makeScheduled("req-2", 915e6, 15e6, 20e6, 1, 60'000, 60'000));
    ASSERT_TRUE(r2.accepted);
    EXPECT_NE(r2.streams[0].device_id, dev1);
}

TEST(ResourceManager, StopTaskOnUnknownIdIsRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.stopTask("no-such-task", "req", "reason");
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::TASK_NOT_FOUND);
}

// ── Config helpers for device-agnostic tests ─────────────────────────────────

static AppConfig makeIndepConfig(int num_devices = 1) {
    AppConfig cfg = makeTestConfig(10, num_devices);
    for (auto& dc : cfg.devices) {
        dc.shared_lo        = false;
        dc.caps.rx_channels = 2;
        dc.caps.tx_channels = 0;
    }
    return cfg;
}

static AppConfig makeHighSrPoolConfig() {
    AppConfig cfg = makeTestConfig(10, 2);
    cfg.devices[1].caps.sample_rate_max_sps = 200e6;
    return cfg;
}

static AppConfig makeMixedRangePoolConfig() {
    AppConfig cfg = makeTestConfig(10, 2);
    auto& b             = cfg.devices[1];
    b.shared_lo         = false;
    b.caps.rx_channels  = 1;
    b.caps.tx_channels  = 0;
    b.caps.freq_min_hz  = 24e6;
    b.caps.freq_max_hz  = 1766e6;
    b.caps.bandwidth_max_hz    = 8e6;
    b.caps.sample_rate_max_sps = 3.2e6;
    return cfg;
}

// ── Independent-LO (shared_lo=false) tests ───────────────────────────────────

TEST(ResourceManager, IndepLo_ConcurrentTasksAtDifferentCfsBothAccepted) {
    FakeSoapy::reset();
    ResourceManager rm(makeIndepConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1",  915e6, 5e6, 10e6, 1, 60'000, 60'000));
    auto r2 = rm.tryAccept(makeScheduled("req-2", 2400e6, 5e6, 10e6, 1, 60'000, 60'000));
    ASSERT_TRUE(r1.accepted);
    ASSERT_TRUE(r2.accepted);
    EXPECT_EQ(r1.streams[0].device_id, r2.streams[0].device_id);
    EXPECT_NE(r1.streams[0].channel_index, r2.streams[0].channel_index);
}

TEST(ResourceManager, IndepLo_ChannelExhaustionStillRejects) {
    FakeSoapy::reset();
    ResourceManager rm(makeIndepConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1",  433e6, 1e6, 2e6, 1, 60'000, 60'000));
    auto r2 = rm.tryAccept(makeScheduled("req-2", 2400e6, 1e6, 2e6, 1, 60'000, 60'000));
    ASSERT_TRUE(r1.accepted);
    ASSERT_TRUE(r2.accepted);

    auto r3 = rm.tryAccept(makeScheduled("req-3", 915e6, 1e6, 2e6, 1, 60'000, 60'000));
    ASSERT_FALSE(r3.accepted);
    EXPECT_EQ(r3.reject_code, RejectCode::NO_DEVICE_AVAILABLE);
}

TEST(ResourceManager, IndepLo_ContinuousTasksAtDifferentCfsRunSimultaneously) {
    FakeSoapy::reset();
    ResourceManager rm(makeIndepConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeContinuous("req-1",  915e6, 5e6, 10e6, 1));
    auto r2 = rm.tryAccept(makeContinuous("req-2", 2400e6, 5e6, 10e6, 1));
    ASSERT_TRUE(r1.accepted);
    ASSERT_TRUE(r2.accepted);
    EXPECT_EQ(rm.countByState(TaskState::RUNNING), 2);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rm.stopTask(r1.task_id, "s1", "done");
    rm.stopTask(r2.task_id, "s2", "done");
    EXPECT_EQ(rm.countByState(TaskState::RUNNING), 0);
}

// ── Aggregate capability / mixed-pool tests ───────────────────────────────────

TEST(ResourceManager, Caps_HighSrTaskRoutedToCapableDevice) {
    FakeSoapy::reset();
    ResourceManager rm(makeHighSrPoolConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 915e6, 10e6, 100e6, 1));
    ASSERT_TRUE(resp.accepted);
    EXPECT_EQ(resp.streams[0].device_id, "fake-1");
}

TEST(ResourceManager, Caps_SrBeyondAllDevicesRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeHighSrPoolConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 915e6, 10e6, 300e6, 1));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::SAMPLE_RATE_EXCEEDED);
}

TEST(ResourceManager, Caps_LowCfTaskRoutedToNarrowRangeDevice) {
    FakeSoapy::reset();
    ResourceManager rm(makeMixedRangePoolConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 50e6, 1e6, 2e6, 1));
    ASSERT_TRUE(resp.accepted);
    EXPECT_EQ(resp.streams[0].device_id, "fake-1");
}

TEST(ResourceManager, Caps_CfBelowAllDevicesRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeMixedRangePoolConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 10e6, 1e6, 2e6, 1));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::FREQ_OUT_OF_RANGE);
}

TEST(ResourceManager, Caps_BwBeyondAllDevicesRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeMixedRangePoolConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 915e6, 70e6, 10e6, 1));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::BW_EXCEEDED);
}

// ── Cross-board coherent DF tests ─────────────────────────────────────────────

static AppConfig makeCoherentPoolConfig() {
    AppConfig cfg = makeTestConfig(10, 2);
    for (auto& dc : cfg.devices)
        dc.coherency_group = "refclk-group-0";
    return cfg;
}

static TaskRequest makeCoherentDf(const std::string& req_id,
                                   double cf, double bw, double sr,
                                   int rx_count,
                                   const std::string& group,
                                   int64_t start_offset_ms = 60'000,
                                   int64_t duration_ms     = 60'000) {
    TaskRequest r = makeScheduled(req_id, cf, bw, sr, rx_count,
                                  start_offset_ms, duration_ms);
    r.rf.coherency_group = group;
    return r;
}

TEST(ResourceManager, Coherent_4ChannelsAllocatedAcross2Boards) {
    FakeSoapy::reset();
    ResourceManager rm(makeCoherentPoolConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeCoherentDf("req-1", 915e6, 5e6, 10e6, 4, "refclk-group-0"));
    ASSERT_TRUE(resp.accepted);
    ASSERT_EQ(resp.streams.size(), 4u);

    int fake0 = 0, fake1 = 0;
    for (auto& s : resp.streams) {
        if (s.device_id == "fake-0") ++fake0;
        if (s.device_id == "fake-1") ++fake1;
    }
    EXPECT_EQ(fake0, 2);
    EXPECT_EQ(fake1, 2);

    for (auto& s : resp.streams)
        EXPECT_DOUBLE_EQ(s.center_freq_hz, 915e6);

    EXPECT_EQ(rm.udpPortsUsed(), 4);
}

TEST(ResourceManager, Coherent_2ChannelRequestOnCoherentGroupUsesOneBoard) {
    FakeSoapy::reset();
    ResourceManager rm(makeCoherentPoolConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeCoherentDf("req-1", 915e6, 5e6, 10e6, 2, "refclk-group-0"));
    ASSERT_TRUE(resp.accepted);
    EXPECT_EQ(resp.streams.size(), 2u);
}

TEST(ResourceManager, Coherent_UnknownGroupRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeCoherentPoolConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeCoherentDf("req-1", 915e6, 5e6, 10e6, 4, "no-such-group"));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::COHERENCY_UNAVAILABLE);
}

TEST(ResourceManager, Coherent_ExceedingGroupCapacityRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeCoherentPoolConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeCoherentDf("req-1", 915e6, 5e6, 10e6, 5, "refclk-group-0"));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::COHERENCY_UNAVAILABLE);
}

TEST(ResourceManager, Coherent_Continuous4ChannelTaskStreamsFromBothBoards) {
    FakeSoapy::reset();
    ResourceManager rm(makeCoherentPoolConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    TaskRequest r = makeContinuous("req-1", 915e6, 5e6, 10e6, 4);
    r.rf.coherency_group   = "refclk-group-0";
    r.rf.rx_count          = 4;
    r.streaming.dest_ports = {5000, 5001, 5002, 5003};

    auto resp = rm.tryAccept(r);
    ASSERT_TRUE(resp.accepted);
    EXPECT_EQ(resp.streams.size(), 4u);
    EXPECT_EQ(rm.countByState(TaskState::RUNNING), 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto stop = rm.stopTask(resp.task_id, "s1", "done");
    ASSERT_TRUE(stop.accepted);
    EXPECT_EQ(rm.countByState(TaskState::RUNNING), 0);
    EXPECT_EQ(rm.udpPortsUsed(), 0);
}

TEST(ResourceManager, Coherent_ScheduledFutureTaskAcceptedInScheduledState) {
    FakeSoapy::reset();
    ResourceManager rm(makeCoherentPoolConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeCoherentDf("req-1", 915e6, 5e6, 10e6, 4, "refclk-group-0"));
    ASSERT_TRUE(resp.accepted);
    EXPECT_EQ(rm.countByState(TaskState::SCHEDULED), 1);
    EXPECT_EQ(rm.countByState(TaskState::RUNNING),   0);
    EXPECT_EQ(rm.udpPortsUsed(), 4);
}

TEST(ResourceManager, Coherent_RejectedWhenOneBoardHasConflictingTask) {
    FakeSoapy::reset();
    ResourceManager rm(makeCoherentPoolConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1", 915e6, 5e6, 10e6, 2, 60'000, 60'000));
    ASSERT_TRUE(r1.accepted);

    auto r2 = rm.tryAccept(makeCoherentDf("req-2", 915e6, 5e6, 10e6, 4, "refclk-group-0"));
    ASSERT_FALSE(r2.accepted);
    EXPECT_EQ(r2.reject_code, RejectCode::COHERENCY_UNAVAILABLE);
}

TEST(ResourceManager, Coherent_TwoGroupsRoutedToTheirRespectiveBoards) {
    FakeSoapy::reset();
    AppConfig cfg = makeTestConfig(10, 4);
    cfg.devices[0].coherency_group = "alpha";
    cfg.devices[1].coherency_group = "alpha";
    cfg.devices[2].coherency_group = "beta";
    cfg.devices[3].coherency_group = "beta";

    ResourceManager rm(cfg, [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto ra = rm.tryAccept(makeCoherentDf("req-a", 915e6,  5e6, 10e6, 4, "alpha"));
    auto rb = rm.tryAccept(makeCoherentDf("req-b", 2400e6, 5e6, 20e6, 4, "beta"));
    ASSERT_TRUE(ra.accepted);
    ASSERT_TRUE(rb.accepted);
    ASSERT_EQ(ra.streams.size(), 4u);
    ASSERT_EQ(rb.streams.size(), 4u);

    for (auto& s : ra.streams)
        EXPECT_TRUE(s.device_id == "fake-0" || s.device_id == "fake-1");
    for (auto& s : rb.streams)
        EXPECT_TRUE(s.device_id == "fake-2" || s.device_id == "fake-3");
}

// ── Hardware profile config helpers ──────────────────────────────────────────

static PolicyConfig hwTestPolicy() {
    PolicyConfig p;
    p.max_concurrent_tasks    = 10;
    p.guard_band_hz           = 200e3;
    p.usable_bw_fraction      = 0.80;
    p.default_task_timeout_ms = 60'000;
    p.scheduler_tick_ms       = 200;
    p.watchdog_tick_ms        = 500;
    p.udp_port_pool_start     = 45000;
    p.udp_port_pool_end       = 45099;
    p.iq_packet_samples       = 128;
    p.retune_conflict_policy  = RetuneConflictPolicy::REJECT_NEW;
    p.heartbeat_interval_ms   = 10'000;
    return p;
}

static AppConfig makeRtlSdrConfig() {
    AppConfig cfg;
    cfg.policy = hwTestPolicy();
    DeviceConfig dc;
    dc.id = "rtlsdr-0"; dc.driver = "fake"; dc.uri = "fake";
    dc.label = "RTL-SDR R820T2"; dc.streaming_source_ip = "127.0.0.1";
    dc.coherency_group = ""; dc.shared_lo = false;
    dc.caps.rx_channels = 1;   dc.caps.tx_channels = 0;
    dc.caps.freq_min_hz = 24e6;  dc.caps.freq_max_hz = 1766e6;
    dc.caps.bandwidth_max_hz = 8e6; dc.caps.sample_rate_max_sps = 3.2e6;
    dc.caps.rx_gain_min_db = 0; dc.caps.rx_gain_max_db = 49;
    dc.caps.tx_atten_min_db = 0; dc.caps.tx_atten_max_db = 0;
    cfg.devices.push_back(dc);
    return cfg;
}

static AppConfig makeHackRfConfig() {
    AppConfig cfg;
    cfg.policy = hwTestPolicy();
    DeviceConfig dc;
    dc.id = "hackrf-0"; dc.driver = "fake"; dc.uri = "fake";
    dc.label = "HackRF One"; dc.streaming_source_ip = "127.0.0.1";
    dc.coherency_group = ""; dc.shared_lo = false;
    dc.caps.rx_channels = 1;  dc.caps.tx_channels = 1;
    dc.caps.freq_min_hz = 1e6;  dc.caps.freq_max_hz = 6e9;
    dc.caps.bandwidth_max_hz = 20e6; dc.caps.sample_rate_max_sps = 20e6;
    dc.caps.rx_gain_min_db = 0; dc.caps.rx_gain_max_db = 47;
    dc.caps.tx_atten_min_db = 0; dc.caps.tx_atten_max_db = 47;
    cfg.devices.push_back(dc);
    return cfg;
}

static AppConfig makeLimeSdrConfig() {
    AppConfig cfg;
    cfg.policy = hwTestPolicy();
    DeviceConfig dc;
    dc.id = "limesdr-0"; dc.driver = "fake"; dc.uri = "fake";
    dc.label = "LimeSDR-USB"; dc.streaming_source_ip = "127.0.0.1";
    dc.coherency_group = "lime-group"; dc.shared_lo = true;
    dc.caps.rx_channels = 2;  dc.caps.tx_channels = 2;
    dc.caps.freq_min_hz = 100e3; dc.caps.freq_max_hz = 3.8e9;
    dc.caps.bandwidth_max_hz = 130e6; dc.caps.sample_rate_max_sps = 200e6;
    dc.caps.rx_gain_min_db = -12; dc.caps.rx_gain_max_db = 61;
    dc.caps.tx_atten_min_db = 0;  dc.caps.tx_atten_max_db = 60;
    cfg.devices.push_back(dc);
    return cfg;
}

static AppConfig makeUsrpB210Config() {
    AppConfig cfg;
    cfg.policy = hwTestPolicy();
    DeviceConfig dc;
    dc.id = "b210-0"; dc.driver = "fake"; dc.uri = "fake";
    dc.label = "USRP B210"; dc.streaming_source_ip = "127.0.0.1";
    dc.coherency_group = "uhd-group"; dc.shared_lo = false;
    dc.caps.rx_channels = 2;  dc.caps.tx_channels = 2;
    dc.caps.freq_min_hz = 70e6;  dc.caps.freq_max_hz = 6e9;
    dc.caps.bandwidth_max_hz = 56e6; dc.caps.sample_rate_max_sps = 61.44e6;
    dc.caps.rx_gain_min_db = 0;  dc.caps.rx_gain_max_db = 76;
    dc.caps.tx_atten_min_db = 0; dc.caps.tx_atten_max_db = 89;
    cfg.devices.push_back(dc);
    return cfg;
}

// ── RTL-SDR profile tests ─────────────────────────────────────────────────────

TEST(RtlSdr, TaskAcceptedWithinDeviceCapabilities) {
    FakeSoapy::reset();
    ResourceManager rm(makeRtlSdrConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 100e6, 2e6, 2e6, 1));
    ASSERT_TRUE(resp.accepted);
    EXPECT_EQ(resp.streams[0].device_id, "rtlsdr-0");
    EXPECT_DOUBLE_EQ(resp.streams[0].sample_rate_sps, 2e6);
}

TEST(RtlSdr, FreqBelow24MHzMinimumRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeRtlSdrConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 10e6, 1e6, 1e6, 1));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::FREQ_OUT_OF_RANGE);
}

TEST(RtlSdr, FreqAbove1766MHzMaximumRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeRtlSdrConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 2400e6, 1e6, 1e6, 1));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::FREQ_OUT_OF_RANGE);
}

TEST(RtlSdr, SampleRateAbove32MspsRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeRtlSdrConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 100e6, 1e6, 5e6, 1));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::SAMPLE_RATE_EXCEEDED);
}

TEST(RtlSdr, BandwidthAbove8MHzRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeRtlSdrConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 100e6, 10e6, 2e6, 1));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::BW_EXCEEDED);
}

TEST(RtlSdr, RequestFor2ChannelsRejectedDeviceHas1) {
    FakeSoapy::reset();
    ResourceManager rm(makeRtlSdrConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 100e6, 2e6, 2e6, 2));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::NO_DEVICE_AVAILABLE);
}

TEST(RtlSdr, SecondTaskRejectedWhenSingleChannelBusy) {
    FakeSoapy::reset();
    ResourceManager rm(makeRtlSdrConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1", 100e6, 2e6, 2e6, 1, 60'000, 60'000));
    ASSERT_TRUE(r1.accepted);

    auto r2 = rm.tryAccept(makeScheduled("req-2", 200e6, 2e6, 2e6, 1, 60'000, 60'000));
    ASSERT_FALSE(r2.accepted);
    EXPECT_EQ(r2.reject_code, RejectCode::NO_DEVICE_AVAILABLE);
}

TEST(RtlSdr, ContinuousStreamRunsAndStopsCleanly) {
    FakeSoapy::reset();
    ResourceManager rm(makeRtlSdrConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeContinuous("req-1", 433e6, 2e6, 2e6, 1));
    ASSERT_TRUE(resp.accepted);
    EXPECT_EQ(rm.countByState(TaskState::RUNNING), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rm.stopTask(resp.task_id, "s1", "done");
    EXPECT_EQ(rm.udpPortsUsed(), 0);
}

// ── HackRF profile tests ──────────────────────────────────────────────────────

TEST(HackRf, TaskAcceptedAtVeryLowFrequency) {
    FakeSoapy::reset();
    ResourceManager rm(makeHackRfConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 5e6, 1e6, 2e6, 1));
    ASSERT_TRUE(resp.accepted);
    EXPECT_DOUBLE_EQ(resp.streams[0].center_freq_hz, 5e6);
}

TEST(HackRf, TaskAcceptedNearTopOfRange) {
    FakeSoapy::reset();
    ResourceManager rm(makeHackRfConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 5800e6, 5e6, 10e6, 1));
    ASSERT_TRUE(resp.accepted);
}

TEST(HackRf, RequestFor2RxChannelsRejectedDeviceHas1) {
    FakeSoapy::reset();
    ResourceManager rm(makeHackRfConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 915e6, 5e6, 10e6, 2));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::NO_DEVICE_AVAILABLE);
}

TEST(HackRf, SampleRateAbove20MspsRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeHackRfConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 915e6, 5e6, 25e6, 1));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::SAMPLE_RATE_EXCEEDED);
}

TEST(HackRf, BandwidthAbove20MHzRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeHackRfConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 915e6, 25e6, 10e6, 1));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::BW_EXCEEDED);
}

TEST(HackRf, ContinuousStreamAtSubSeventy_MHzFrequency) {
    FakeSoapy::reset();
    ResourceManager rm(makeHackRfConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeContinuous("req-1", 27e6, 2e6, 5e6, 1));
    ASSERT_TRUE(resp.accepted);
    EXPECT_EQ(rm.countByState(TaskState::RUNNING), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rm.stopTask(resp.task_id, "s1", "done");
    EXPECT_EQ(rm.udpPortsUsed(), 0);
}

// ── LimeSDR profile tests ─────────────────────────────────────────────────────

TEST(LimeSdr, HighSampleRate150MspsAccepted) {
    FakeSoapy::reset();
    ResourceManager rm(makeLimeSdrConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 915e6, 10e6, 150e6, 1));
    ASSERT_TRUE(resp.accepted);
    EXPECT_DOUBLE_EQ(resp.streams[0].sample_rate_sps, 150e6);
}

TEST(LimeSdr, SampleRateAbove200MspsRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeLimeSdrConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeScheduled("req-1", 915e6, 10e6, 250e6, 1));
    ASSERT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::SAMPLE_RATE_EXCEEDED);
}

TEST(LimeSdr, SharedLoTwoTasksAtSameCfShareSpectrum) {
    FakeSoapy::reset();
    ResourceManager rm(makeLimeSdrConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1", 915e6, 5e6, 20e6, 1, 60'000, 60'000));
    auto r2 = rm.tryAccept(makeScheduled("req-2", 915e6, 3e6, 20e6, 1, 60'000, 60'000));
    ASSERT_TRUE(r1.accepted);
    ASSERT_TRUE(r2.accepted);
    EXPECT_NE(r1.streams[0].channel_index, r2.streams[0].channel_index);
}

TEST(LimeSdr, SharedLoRetuneConflictWhenSecondTaskHasDifferentCf) {
    FakeSoapy::reset();
    ResourceManager rm(makeLimeSdrConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1",  915e6, 5e6, 20e6, 1, 60'000, 60'000));
    auto r2 = rm.tryAccept(makeScheduled("req-2", 2400e6, 5e6, 20e6, 1, 60'000, 60'000));
    ASSERT_TRUE(r1.accepted);
    ASSERT_FALSE(r2.accepted);
    EXPECT_EQ(r2.reject_code, RejectCode::NO_DEVICE_AVAILABLE);
}

TEST(LimeSdr, Continuous2ChannelTaskStreamsBothChannels) {
    FakeSoapy::reset();
    ResourceManager rm(makeLimeSdrConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto resp = rm.tryAccept(makeContinuous("req-1", 915e6, 5e6, 20e6, 2));
    ASSERT_TRUE(resp.accepted);
    EXPECT_EQ(resp.streams.size(), 2u);
    EXPECT_EQ(rm.countByState(TaskState::RUNNING), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rm.stopTask(resp.task_id, "s1", "done");
    EXPECT_EQ(rm.udpPortsUsed(), 0);
}

// ── USRP B210 profile tests ───────────────────────────────────────────────────

TEST(UsrpB210, TwoConcurrentTasksAtDifferentCfsBothAccepted) {
    FakeSoapy::reset();
    ResourceManager rm(makeUsrpB210Config(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1",  915e6, 5e6, 10e6, 1, 60'000, 60'000));
    auto r2 = rm.tryAccept(makeScheduled("req-2", 2400e6, 5e6, 10e6, 1, 60'000, 60'000));
    ASSERT_TRUE(r1.accepted);
    ASSERT_TRUE(r2.accepted);
    EXPECT_EQ(r1.streams[0].device_id, r2.streams[0].device_id);
    EXPECT_NE(r1.streams[0].channel_index, r2.streams[0].channel_index);
}

TEST(UsrpB210, ThirdConcurrentTaskRejectedWhenBothChannelsBusy) {
    FakeSoapy::reset();
    ResourceManager rm(makeUsrpB210Config(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1",  915e6, 5e6, 10e6, 1, 60'000, 60'000));
    auto r2 = rm.tryAccept(makeScheduled("req-2", 2400e6, 5e6, 10e6, 1, 60'000, 60'000));
    ASSERT_TRUE(r1.accepted);
    ASSERT_TRUE(r2.accepted);

    auto r3 = rm.tryAccept(makeScheduled("req-3", 433e6, 1e6, 2e6, 1, 60'000, 60'000));
    ASSERT_FALSE(r3.accepted);
    EXPECT_EQ(r3.reject_code, RejectCode::NO_DEVICE_AVAILABLE);
}

TEST(UsrpB210, SameRequestsCauseRetuneConflictOnSharedLoDevice) {
    FakeSoapy::reset();

    // AD9361 (shared_lo=true): second task at different CF fails
    {
        ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
        rm.openDevices();
        auto r1 = rm.tryAccept(makeScheduled("req-1",  915e6, 5e6, 10e6, 1, 60'000, 60'000));
        auto r2 = rm.tryAccept(makeScheduled("req-2", 2400e6, 5e6, 10e6, 1, 60'000, 60'000));
        ASSERT_TRUE(r1.accepted);
        ASSERT_FALSE(r2.accepted);
    }

    FakeSoapy::reset();

    // USRP B210 (shared_lo=false): identical requests both succeed
    {
        ResourceManager rm(makeUsrpB210Config(), [](const TaskRecord&){}, [](const auto&, const auto&){});
        rm.openDevices();
        auto r1 = rm.tryAccept(makeScheduled("req-1",  915e6, 5e6, 10e6, 1, 60'000, 60'000));
        auto r2 = rm.tryAccept(makeScheduled("req-2", 2400e6, 5e6, 10e6, 1, 60'000, 60'000));
        ASSERT_TRUE(r1.accepted);
        ASSERT_TRUE(r2.accepted);
    }
}

TEST(UsrpB210, ContinuousIndependentStreamsAtDifferentCfsRunSimultaneously) {
    FakeSoapy::reset();
    ResourceManager rm(makeUsrpB210Config(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeContinuous("req-1",  915e6, 5e6, 10e6, 1));
    auto r2 = rm.tryAccept(makeContinuous("req-2", 2400e6, 5e6, 10e6, 1));
    ASSERT_TRUE(r1.accepted);
    ASSERT_TRUE(r2.accepted);
    EXPECT_EQ(rm.countByState(TaskState::RUNNING), 2);
    EXPECT_EQ(rm.udpPortsUsed(), 2);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rm.stopTask(r1.task_id, "s1", "done");
    rm.stopTask(r2.task_id, "s2", "done");
    EXPECT_EQ(rm.countByState(TaskState::RUNNING), 0);
    EXPECT_EQ(rm.udpPortsUsed(), 0);
}

// ── preferred_device routing tests ───────────────────────────────────────────

TEST(ResourceManager, Routing_PreferredDeviceHintRoutesToSpecifiedDevice) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(10, 2), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1", 915e6, 5e6, 10e6, 1, 60'000, 60'000));
    ASSERT_TRUE(r1.accepted);
    std::string dev1 = r1.streams[0].device_id;

    TaskRequest r2req = makeScheduled("req-2", 915e6, 3e6, 10e6, 1, 60'000, 60'000);
    r2req.rf.preferred_device = dev1;
    auto r2 = rm.tryAccept(r2req);
    ASSERT_TRUE(r2.accepted);
    EXPECT_EQ(r2.streams[0].device_id, dev1);
}

TEST(ResourceManager, Routing_FallsBackToOtherDeviceWhenPreferredFull) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(10, 2), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto r1 = rm.tryAccept(makeScheduled("req-1", 915e6, 3e6, 10e6, 2, 60'000, 60'000));
    ASSERT_TRUE(r1.accepted);
    std::string dev1 = r1.streams[0].device_id;
    std::string dev2 = (dev1 == "fake-0") ? "fake-1" : "fake-0";

    TaskRequest r2req = makeScheduled("req-2", 915e6, 2e6, 10e6, 1, 60'000, 60'000);
    r2req.rf.preferred_device = dev1;
    auto r2 = rm.tryAccept(r2req);
    ASSERT_TRUE(r2.accepted);
    EXPECT_EQ(r2.streams[0].device_id, dev2);
}

// ── Rank preemption tests ─────────────────────────────────────────────────────

TEST(ResourceManager, Rank_HigherRankTaskPreemptsLowerRankContinuousTask) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    // Occupy both RX channels with a rank=0 task
    auto req_low = makeContinuous("req-low", 915e6, 5e6, 10e6, 2);
    req_low.rank = 0;
    auto r_low = rm.tryAccept(req_low);
    ASSERT_TRUE(r_low.accepted);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(rm.countByState(TaskState::RUNNING), 1);

    // Higher-rank task needs 1 channel at the same freq — preempts the rank=0 task
    auto req_high = makeContinuous("req-high", 915e6, 5e6, 10e6, 1);
    req_high.rank = 2;
    auto r_high = rm.tryAccept(req_high);
    ASSERT_TRUE(r_high.accepted);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    EXPECT_EQ(rm.countByState(TaskState::RUNNING),   1);
    EXPECT_EQ(rm.countByState(TaskState::CANCELLED), 1);

    rm.stopTask(r_high.task_id, "s1", "done");
}

TEST(ResourceManager, Rank_EqualRankDoesNotPreempt) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto req1 = makeContinuous("req-1", 915e6, 5e6, 10e6, 2);
    req1.rank = 1;
    auto r1 = rm.tryAccept(req1);
    ASSERT_TRUE(r1.accepted);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Same rank — must NOT preempt
    auto req2 = makeContinuous("req-2", 915e6, 5e6, 10e6, 1);
    req2.rank = 1;
    auto r2 = rm.tryAccept(req2);
    EXPECT_FALSE(r2.accepted);
    EXPECT_EQ(rm.countByState(TaskState::RUNNING),   1);
    EXPECT_EQ(rm.countByState(TaskState::CANCELLED), 0);

    rm.stopTask(r1.task_id, "s1", "done");
}

TEST(ResourceManager, Rank_ZeroRankNewTaskDoesNotPreempt) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    auto req1 = makeContinuous("req-1", 915e6, 5e6, 10e6, 2);
    req1.rank = 0;
    auto r1 = rm.tryAccept(req1);
    ASSERT_TRUE(r1.accepted);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // rank=0 must never trigger preemption
    auto req2 = makeContinuous("req-2", 915e6, 5e6, 10e6, 1);
    req2.rank = 0;
    auto r2 = rm.tryAccept(req2);
    EXPECT_FALSE(r2.accepted);
    EXPECT_EQ(r2.reject_code, RejectCode::NO_DEVICE_AVAILABLE);
    EXPECT_EQ(rm.countByState(TaskState::RUNNING),   1);
    EXPECT_EQ(rm.countByState(TaskState::CANCELLED), 0);

    rm.stopTask(r1.task_id, "s1", "done");
}

TEST(ResourceManager, Rank_LowerRankCannotPreemptHigherRankTask) {
    FakeSoapy::reset();
    ResourceManager rm(makeTestConfig(), [](const TaskRecord&){}, [](const auto&, const auto&){});
    rm.openDevices();

    // High-rank task occupies both channels
    auto req_high = makeContinuous("req-high", 915e6, 5e6, 10e6, 2);
    req_high.rank = 5;
    auto r_high = rm.tryAccept(req_high);
    ASSERT_TRUE(r_high.accepted);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Lower-rank task (rank=2) must not preempt the rank=5 task
    auto req_low = makeContinuous("req-low", 915e6, 5e6, 10e6, 1);
    req_low.rank = 2;
    auto r_low = rm.tryAccept(req_low);
    EXPECT_FALSE(r_low.accepted);
    EXPECT_EQ(rm.countByState(TaskState::RUNNING),   1);
    EXPECT_EQ(rm.countByState(TaskState::CANCELLED), 0);

    rm.stopTask(r_high.task_id, "s1", "done");
}

TEST(ResourceManager, Rank_PreemptedTaskTerminalReasonContainsPreemptedString) {
    FakeSoapy::reset();
    std::vector<TaskRecord> state_changes;
    std::mutex mu;
    auto on_change = [&](const TaskRecord& r) {
        std::lock_guard lk(mu);
        state_changes.push_back(r);
    };
    ResourceManager rm(makeTestConfig(), on_change, [](const auto&, const auto&){});
    rm.openDevices();

    auto req_low = makeContinuous("req-low", 915e6, 5e6, 10e6, 2);
    req_low.rank = 0;
    auto r_low = rm.tryAccept(req_low);
    ASSERT_TRUE(r_low.accepted);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto req_high = makeContinuous("req-high", 915e6, 5e6, 10e6, 1);
    req_high.rank = 5;
    auto r_high = rm.tryAccept(req_high);
    ASSERT_TRUE(r_high.accepted);

    // Find the CANCELLED state change for the low-rank task
    {
        std::lock_guard lk(mu);
        auto it = std::find_if(state_changes.begin(), state_changes.end(),
            [&](const TaskRecord& r) {
                return r.task_id == r_low.task_id && r.state == TaskState::CANCELLED;
            });
        ASSERT_NE(it, state_changes.end()) << "Low-rank task should be cancelled";
        EXPECT_NE(it->terminal_reason.find(PREEMPT_TERMINAL_REASON), std::string::npos)
            << "terminal_reason must contain PREEMPT_TERMINAL_REASON";
        EXPECT_TRUE(isPreempted(*it));
    }  // release mu before stopTask to avoid deadlock via notifyStateChange callback

    rm.stopTask(r_high.task_id, "s1", "done");
}

TEST(ResourceManager, Rank_TaskRecordCarriesRankFromRequest) {
    FakeSoapy::reset();
    std::vector<TaskRecord> records;
    std::mutex mu;
    auto on_change = [&](const TaskRecord& r) {
        std::lock_guard lk(mu);
        records.push_back(r);
    };
    ResourceManager rm(makeTestConfig(), on_change, [](const auto&, const auto&){});
    rm.openDevices();

    auto req = makeContinuous("req-rank7", 915e6, 5e6, 10e6, 1);
    req.rank = 7;
    auto resp = rm.tryAccept(req);
    ASSERT_TRUE(resp.accepted);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    {
        std::lock_guard lk(mu);
        auto it = std::find_if(records.begin(), records.end(),
            [&](const TaskRecord& r) { return r.task_id == resp.task_id; });
        ASSERT_NE(it, records.end());
        EXPECT_EQ(it->rank, 7);
    }  // release mu before stopTask to avoid deadlock via notifyStateChange callback

    rm.stopTask(resp.task_id, "s1", "done");
}
