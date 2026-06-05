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
 * @file test_resource_manager_scale.cpp
 * @brief Multi-device scalability tests for ResourceManager.
 *
 * Tests SdrResourceManager behaviour when many fake SDR devices (10, 100)
 * are registered simultaneously — the same scenario as AcquisitionApp
 * auto-splitting bands across all available devices and submitting one
 * SCAN task per slice.
 *
 * All devices use FakeSoapyDevice (driver="fake") — no physical hardware.
 */
#include <gtest/gtest.h>
#include "ResourceManager.hpp"
#include "sdr/Types.hpp"
#include "sdr/MessageCodec.hpp"
#include "FakeSoapyControl.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace sdr;
using namespace std::chrono;

// ── Helpers (mirrors test_resource_manager.cpp style) ────────────────────────

static int64_t nowMs() {
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

static void waitForState(ResourceManager& rm, TaskState state, int expected,
                         int timeout_ms = 2000) {
    for (int i = 0; i < timeout_ms / 2 && rm.countByState(state) != expected; ++i)
        std::this_thread::sleep_for(milliseconds(2));
}

/// Build an AppConfig with `n` fake devices, each with a wide frequency range
/// so they can accept any test task.
static AppConfig makeScaleConfig(int n, int max_tasks = 200) {
    AppConfig cfg;
    cfg.policy.max_concurrent_tasks    = max_tasks;
    cfg.policy.guard_band_hz           = 200e3;
    cfg.policy.usable_bw_fraction      = 0.80;
    cfg.policy.default_task_timeout_ms = 60'000;
    cfg.policy.scheduler_tick_ms       = 50;
    cfg.policy.watchdog_tick_ms        = 500;
    cfg.policy.udp_port_pool_start     = 46000;
    cfg.policy.udp_port_pool_end       = 46999;  // 1000 ports for up to 100 devices + headroom
    cfg.policy.iq_packet_samples       = 128;
    cfg.policy.retune_conflict_policy  = RetuneConflictPolicy::REJECT_NEW;
    cfg.policy.heartbeat_interval_ms   = 10'000;

    for (int i = 0; i < n; ++i) {
        DeviceConfig dc;
        dc.id                       = "fake-" + std::to_string(i);
        dc.driver                   = "fake";
        dc.uri                      = "fake";
        dc.label                    = "Fake SDR " + std::to_string(i);
        dc.streaming_source_ip      = "127.0.0.1";
        dc.coherency_group          = "scale-group-" + std::to_string(i / 4);
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

/// Build a SCAN task request mimicking AcquisitionApp's buildScanRequest().
/// Each step covers a 10 MHz slice; `freq_slice_hz` is the band start.
static TaskRequest makeScanRequest(const std::string& req_id,
                                    double freq_slice_hz,
                                    double span_hz    = 100e6,
                                    double sr_hz      = 10e6,
                                    int    rank       = 1,
                                    const std::string& preferred = "") {
    TaskRequest r;
    r.msg_type           = "TASK_REQUEST_SCAN";
    r.request_id         = req_id;
    r.schema_version     = "2.0";
    r.timestamp_ms       = nowMs();
    r.task_type          = TaskType::SCAN;
    r.schedule_mode      = ScheduleMode::CONTINUOUS;
    r.start_time_ms      = nowMs();
    r.end_time_ms        = TIME_INFINITE;
    r.rank               = rank;
    r.rf.center_freq_hz  = freq_slice_hz + span_hz / 2.0;
    r.rf.bandwidth_hz    = sr_hz * 0.8;
    r.rf.sample_rate_sps = sr_hz;
    r.rf.rx_count        = 1;
    r.rf.tx_count        = 0;
    r.rf.preferred_device = preferred;
    r.streaming.dest_ip  = "127.0.0.1";

    ScanParams sp;
    sp.repeat = true;
    double step = sr_hz * 0.8;
    for (double pos = freq_slice_hz; pos < freq_slice_hz + span_hz; pos += step) {
        ScanEntry e;
        e.step            = static_cast<int>((pos - freq_slice_hz) / step);
        e.center_freq_hz  = pos + step / 2.0;
        e.sample_rate_sps = sr_hz;
        e.bandwidth_hz    = sr_hz * 0.8;
        e.dwell_ms        = 10;
        sp.entries.push_back(e);
    }
    r.scan_params = sp;
    return r;
}

static TaskRequest makeContinuousRequest(const std::string& req_id,
                                          double cf, double bw, double sr,
                                          int rank = 1,
                                          const std::string& preferred = "") {
    TaskRequest r;
    r.msg_type            = "TASK_REQUEST_CONTINUOUS";
    r.request_id          = req_id;
    r.schema_version      = "2.0";
    r.timestamp_ms        = nowMs();
    r.task_type           = TaskType::NARROWBAND;
    r.schedule_mode       = ScheduleMode::CONTINUOUS;
    r.start_time_ms       = nowMs();
    r.end_time_ms         = TIME_INFINITE;
    r.rank                = rank;
    r.rf.center_freq_hz   = cf;
    r.rf.bandwidth_hz     = bw;
    r.rf.sample_rate_sps  = sr;
    r.rf.rx_count         = 1;
    r.rf.tx_count         = 0;
    r.rf.preferred_device = preferred;
    r.streaming.dest_ip   = "127.0.0.1";
    return r;
}

// ── 10-device tests ───────────────────────────────────────────────────────────

class TenDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        FakeSoapy::reset();
        rm = std::make_unique<ResourceManager>(
            makeScaleConfig(10), [](const TaskRecord&){}, [](const auto&, const auto&){});
        ASSERT_EQ(rm->openDevices(), 10);
    }
    std::unique_ptr<ResourceManager> rm;
};

TEST_F(TenDeviceTest, TenScanTasksAllAccepted) {
    std::vector<std::string> task_ids;
    for (int i = 0; i < 10; ++i) {
        double slice_start = 70e6 + i * 590e6;
        auto resp = rm->tryAccept(
            makeScanRequest("req-" + std::to_string(i), slice_start));
        EXPECT_TRUE(resp.accepted) << "Task " << i << " rejected: " << resp.reject_reason;
        if (resp.accepted) task_ids.push_back(resp.task_id);
    }
    EXPECT_EQ(task_ids.size(), 10u);
    waitForState(*rm, TaskState::RUNNING, 10);
    EXPECT_EQ(rm->countByState(TaskState::RUNNING), 10);
}

TEST_F(TenDeviceTest, EleventhScanTaskPendingOrRejected) {
    // Fill all 10 devices
    for (int i = 0; i < 10; ++i)
        rm->tryAccept(makeScanRequest("req-" + std::to_string(i), 70e6 + i * 590e6));
    waitForState(*rm, TaskState::RUNNING, 10);

    // 11th should have no free device
    auto resp = rm->tryAccept(makeScanRequest("req-11", 100e6));
    EXPECT_FALSE(resp.accepted);
}

TEST_F(TenDeviceTest, EachTaskGetsUniqueDevice) {
    std::set<std::string> assigned_devices;
    for (int i = 0; i < 10; ++i) {
        auto resp = rm->tryAccept(
            makeScanRequest("req-" + std::to_string(i), 70e6 + i * 590e6));
        ASSERT_TRUE(resp.accepted) << "Task " << i << " was not accepted";
        ASSERT_FALSE(resp.streams.empty());
        std::string dev = resp.streams[0].device_id;
        EXPECT_TRUE(assigned_devices.insert(dev).second)
            << "Device " << dev << " assigned twice";
    }
    EXPECT_EQ(assigned_devices.size(), 10u);
}

TEST_F(TenDeviceTest, PreferredDeviceHonouredAmongTen) {
    auto resp = rm->tryAccept(
        makeScanRequest("req-pref", 200e6, 100e6, 10e6, 1, "fake-7"));
    ASSERT_TRUE(resp.accepted);
    ASSERT_FALSE(resp.streams.empty());
    EXPECT_EQ(resp.streams[0].device_id, "fake-7");
}

TEST_F(TenDeviceTest, PreemptionLeavesNineOthersRunning) {
    // Start 10 low-rank SCAN tasks
    std::vector<std::string> task_ids;
    for (int i = 0; i < 10; ++i) {
        auto resp = rm->tryAccept(
            makeScanRequest("scan-" + std::to_string(i), 70e6 + i * 590e6, 100e6, 10e6, 1));
        ASSERT_TRUE(resp.accepted);
        task_ids.push_back(resp.task_id);
    }
    waitForState(*rm, TaskState::RUNNING, 10);

    // High-rank task preempts one device
    auto hi = rm->tryAccept(
        makeContinuousRequest("hi-rank", 915e6, 10e6, 10e6, 3));
    EXPECT_TRUE(hi.accepted);
    waitForState(*rm, TaskState::RUNNING, 10);  // still 10 running (preemptee → pending)
    EXPECT_GE(rm->countByState(TaskState::RUNNING), 9);
}

TEST_F(TenDeviceTest, UdpPortsAllocatedPerTask) {
    for (int i = 0; i < 10; ++i)
        rm->tryAccept(makeScanRequest("req-" + std::to_string(i), 70e6 + i * 590e6));
    waitForState(*rm, TaskState::RUNNING, 10);
    EXPECT_EQ(rm->udpPortsUsed(), 10);
}

TEST_F(TenDeviceTest, ContinuousTasksOnDifferentFrequencies) {
    std::set<int> ports;
    for (int i = 0; i < 10; ++i) {
        double cf = 200e6 + i * 500e6;
        auto resp = rm->tryAccept(
            makeContinuousRequest("cont-" + std::to_string(i), cf, 5e6, 10e6));
        ASSERT_TRUE(resp.accepted) << "Task " << i << " rejected";
        ASSERT_FALSE(resp.streams.empty());
        EXPECT_TRUE(ports.insert(resp.streams[0].udp_port).second)
            << "Duplicate UDP port " << resp.streams[0].udp_port;
    }
}

TEST_F(TenDeviceTest, AllTasksHaveValidUdpPortRange) {
    for (int i = 0; i < 10; ++i) {
        auto resp = rm->tryAccept(
            makeScanRequest("req-" + std::to_string(i), 70e6 + i * 590e6));
        ASSERT_TRUE(resp.accepted);
        ASSERT_FALSE(resp.streams.empty());
        EXPECT_GE(resp.streams[0].udp_port, 46000);
        EXPECT_LE(resp.streams[0].udp_port, 46999);
    }
}

TEST_F(TenDeviceTest, DeviceCountQueryReturnsExpectedCount) {
    // deviceSummaries() is the same source HEALTH_QUERY uses
    auto summaries = rm->deviceSummaries();
    EXPECT_EQ(static_cast<int>(summaries.size()), 10);
    int online = std::count_if(summaries.begin(), summaries.end(),
                               [](const auto& s){ return s.online; });
    EXPECT_EQ(online, 10);
}

// ── 100-device tests ──────────────────────────────────────────────────────────

class HundredDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        FakeSoapy::reset();
        rm = std::make_unique<ResourceManager>(
            makeScaleConfig(100, 200),
            [](const TaskRecord&){}, [](const auto&, const auto&){});
        ASSERT_EQ(rm->openDevices(), 100);
    }
    std::unique_ptr<ResourceManager> rm;
};

TEST_F(HundredDeviceTest, OpenHundredFakeDevicesSucceeds) {
    EXPECT_EQ(rm->deviceSummaries().size(), 100u);
}

TEST_F(HundredDeviceTest, HundredScanTasksAllAccepted) {
    int accepted = 0;
    for (int i = 0; i < 100; ++i) {
        double slice = 70e6 + i * 59e6;
        auto resp = rm->tryAccept(
            makeScanRequest("req-" + std::to_string(i), slice, 50e6));
        if (resp.accepted) ++accepted;
        else ADD_FAILURE() << "Task " << i << " rejected: " << resp.reject_reason;
    }
    EXPECT_EQ(accepted, 100);
}

TEST_F(HundredDeviceTest, EachOfHundredTasksGetsUniqueDevice) {
    std::set<std::string> devs;
    for (int i = 0; i < 100; ++i) {
        double slice = 70e6 + i * 59e6;
        auto resp = rm->tryAccept(
            makeScanRequest("req-" + std::to_string(i), slice, 50e6));
        ASSERT_TRUE(resp.accepted) << "Task " << i << " not accepted";
        ASSERT_FALSE(resp.streams.empty());
        EXPECT_TRUE(devs.insert(resp.streams[0].device_id).second)
            << "Duplicate device: " << resp.streams[0].device_id;
    }
    EXPECT_EQ(devs.size(), 100u);
}

TEST_F(HundredDeviceTest, HundredAndFirstTaskRejected) {
    for (int i = 0; i < 100; ++i)
        rm->tryAccept(makeScanRequest("req-" + std::to_string(i), 70e6 + i * 59e6, 50e6));
    waitForState(*rm, TaskState::RUNNING, 100, 5000);
    auto extra = rm->tryAccept(makeScanRequest("req-extra", 300e6));
    EXPECT_FALSE(extra.accepted);
}

TEST_F(HundredDeviceTest, AllHundredUdpPortsUnique) {
    std::set<uint16_t> ports;
    for (int i = 0; i < 100; ++i) {
        auto resp = rm->tryAccept(
            makeScanRequest("req-" + std::to_string(i), 70e6 + i * 59e6, 50e6));
        ASSERT_TRUE(resp.accepted);
        ASSERT_FALSE(resp.streams.empty());
        uint16_t p = resp.streams[0].udp_port;
        EXPECT_TRUE(ports.insert(p).second) << "Duplicate UDP port " << p;
    }
    EXPECT_EQ(ports.size(), 100u);
}

TEST_F(HundredDeviceTest, PreferredDeviceFiftyHonouredAmongHundred) {
    auto resp = rm->tryAccept(
        makeScanRequest("req-pref", 500e6, 50e6, 10e6, 1, "fake-50"));
    ASSERT_TRUE(resp.accepted);
    ASSERT_FALSE(resp.streams.empty());
    EXPECT_EQ(resp.streams[0].device_id, "fake-50");
}

TEST_F(HundredDeviceTest, HighRankPreemptsOneOfHundred) {
    // Start 100 rank-1 scans
    for (int i = 0; i < 100; ++i)
        rm->tryAccept(makeScanRequest("scan-" + std::to_string(i), 70e6 + i * 59e6, 50e6, 10e6, 1));
    waitForState(*rm, TaskState::RUNNING, 100, 5000);

    // Rank-3 DF task preempts one device
    auto hi = rm->tryAccept(
        makeContinuousRequest("hi-df", 915e6, 10e6, 10e6, 3));
    EXPECT_TRUE(hi.accepted);
    // 99 scan tasks stay running, the preempted one goes to PENDING
    std::this_thread::sleep_for(milliseconds(100));
    EXPECT_GE(rm->countByState(TaskState::RUNNING), 99);
}

TEST_F(HundredDeviceTest, DeviceSummariesShowHundredOnline) {
    auto summaries = rm->deviceSummaries();
    ASSERT_EQ(summaries.size(), 100u);
    int online = 0;
    for (auto& s : summaries) if (s.online) ++online;
    EXPECT_EQ(online, 100);
}

TEST_F(HundredDeviceTest, ConcurrentSubmissionsFromMultipleThreads) {
    // Simulate AcquisitionApp spawning 100 IqSource threads that all
    // call tryAccept() concurrently.
    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    threads.reserve(100);
    for (int i = 0; i < 100; ++i) {
        threads.emplace_back([&, i] {
            double slice = 70e6 + i * 59e6;
            auto resp = rm->tryAccept(
                makeScanRequest("req-" + std::to_string(i), slice, 50e6));
            if (resp.accepted) ++accepted;
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(accepted.load(), 100);
    // No duplicate device IDs
    auto summaries = rm->deviceSummaries();
    int active = 0;
    for (auto& s : summaries) if (s.active_tasks > 0) ++active;
    EXPECT_EQ(active, 100);
}

TEST_F(HundredDeviceTest, NoDeadlockOrCrashUnderLoad) {
    // Submit 100 tasks, wait for them all to be running, then stop all.
    // This verifies no mutex deadlock or use-after-free at scale.
    std::vector<std::string> task_ids;
    for (int i = 0; i < 100; ++i) {
        auto resp = rm->tryAccept(
            makeScanRequest("req-" + std::to_string(i), 70e6 + i * 59e6, 50e6));
        if (resp.accepted) task_ids.push_back(resp.task_id);
    }
    waitForState(*rm, TaskState::RUNNING, static_cast<int>(task_ids.size()), 5000);
    // ResourceManager destructor stops all tasks — no assertion here,
    // just verify no crash/deadlock on teardown (ASAN/TSAN will catch issues).
    SUCCEED();
}

// ── Capacity limit tests ──────────────────────────────────────────────────────
//
// These tests probe ResourceManager until the first rejection to find the real
// ceiling. The limit is determined by whichever of these is hit first:
//   1. No free device (n_accepted == n_devices)
//   2. UDP port pool exhausted (pool_end - pool_start ports available)
//   3. max_concurrent_tasks policy limit
//
// We verify the ceiling matches the expected bottleneck and that the system
// is stable (no crash, no corruption) after hitting it.

TEST(CapacityLimit, FindsDeviceCeilingAt10) {
    FakeSoapy::reset();
    // 10 devices, large port pool, high max_tasks — device count is the limit
    ResourceManager rm(makeScaleConfig(10, 500),
                       [](const TaskRecord&){}, [](const auto&, const auto&){});
    ASSERT_EQ(rm.openDevices(), 10);

    int accepted = 0;
    int rejected_at = -1;
    for (int i = 0; i < 20; ++i) {
        auto resp = rm.tryAccept(
            makeScanRequest("req-" + std::to_string(i), 70e6 + (i % 100) * 59e6, 50e6));
        if (resp.accepted) {
            ++accepted;
        } else {
            rejected_at = i;
            break;
        }
    }
    EXPECT_EQ(accepted, 10) << "Expected ceiling at 10 devices";
    EXPECT_EQ(rejected_at, 10) << "11th task should be the first rejection";
}

TEST(CapacityLimit, FindsDeviceCeilingAt100) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(100, 500),
                       [](const TaskRecord&){}, [](const auto&, const auto&){});
    ASSERT_EQ(rm.openDevices(), 100);

    int accepted = 0;
    for (int i = 0; i < 110; ++i) {
        auto resp = rm.tryAccept(
            makeScanRequest("req-" + std::to_string(i), 70e6 + (i % 100) * 59e6, 50e6));
        if (resp.accepted) ++accepted;
        else break;
    }
    EXPECT_EQ(accepted, 100);
}

TEST(CapacityLimit, FindsUdpPortPoolCeiling) {
    FakeSoapy::reset();
    // 200 devices but only 50 UDP ports — port pool is the bottleneck
    AppConfig cfg = makeScaleConfig(200, 500);
    cfg.policy.udp_port_pool_start = 47000;
    cfg.policy.udp_port_pool_end   = 47049;  // 50 ports
    ResourceManager rm(cfg, [](const TaskRecord&){}, [](const auto&, const auto&){});
    ASSERT_EQ(rm.openDevices(), 200);

    int accepted = 0;
    for (int i = 0; i < 60; ++i) {
        auto resp = rm.tryAccept(
            makeScanRequest("req-" + std::to_string(i), 70e6 + (i % 100) * 59e6, 50e6));
        if (resp.accepted) ++accepted;
        else break;
    }
    // Should hit the 50-port ceiling before the 200-device ceiling
    EXPECT_LE(accepted, 50) << "Should not exceed port pool size";
    EXPECT_GT(accepted, 0)  << "At least some tasks should be accepted";
    EXPECT_LE(rm.udpPortsUsed(), 50);
}

TEST(CapacityLimit, FindsMaxConcurrentTasksCeiling) {
    FakeSoapy::reset();
    // 200 devices, 1000 ports, but max_concurrent_tasks = 25
    AppConfig cfg = makeScaleConfig(200, 25);
    cfg.policy.udp_port_pool_start = 47100;
    cfg.policy.udp_port_pool_end   = 47999;
    ResourceManager rm(cfg, [](const TaskRecord&){}, [](const auto&, const auto&){});
    ASSERT_EQ(rm.openDevices(), 200);

    int accepted = 0;
    for (int i = 0; i < 30; ++i) {
        auto resp = rm.tryAccept(
            makeScanRequest("req-" + std::to_string(i), 70e6 + (i % 100) * 59e6, 50e6));
        if (resp.accepted) ++accepted;
        else break;
    }
    EXPECT_EQ(accepted, 25) << "Should hit max_concurrent_tasks=25 before device limit";
}

TEST(CapacityLimit, SystemStableAfterHittingCeiling) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(5, 100),
                       [](const TaskRecord&){}, [](const auto&, const auto&){});
    ASSERT_EQ(rm.openDevices(), 5);

    // Hit the ceiling
    for (int i = 0; i < 10; ++i)
        rm.tryAccept(makeScanRequest("req-" + std::to_string(i), 70e6 + i * 590e6, 100e6));

    // System should still respond correctly
    auto summaries = rm.deviceSummaries();
    EXPECT_EQ(summaries.size(), 5u);
    // Another query should not crash
    auto summaries2 = rm.deviceSummaries();
    EXPECT_EQ(summaries2.size(), 5u);
    EXPECT_EQ(rm.udpPortsUsed(), 5);
}

TEST(CapacityLimit, IncrementalProbeFindsExactLimit) {
    // Generic probe: try with N=1..50 devices, verify accepted == N each time.
    // This confirms the ceiling scales linearly with device count.
    for (int n : {1, 2, 5, 10, 20, 50}) {
        FakeSoapy::reset();
        AppConfig cfg = makeScaleConfig(n, 200);
        cfg.policy.udp_port_pool_start = 48000;
        cfg.policy.udp_port_pool_end   = 48999;
        ResourceManager rm(cfg, [](const TaskRecord&){}, [](const auto&, const auto&){});
        ASSERT_EQ(rm.openDevices(), n) << "n=" << n;

        int accepted = 0;
        for (int i = 0; i < n + 5; ++i) {
            auto resp = rm.tryAccept(
                makeScanRequest("req-" + std::to_string(i),
                                70e6 + (i % n) * (5900e6 / n), 50e6));
            if (resp.accepted) ++accepted;
            else break;
        }
        EXPECT_EQ(accepted, n) << "Ceiling should equal device count n=" << n;
    }
}

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
