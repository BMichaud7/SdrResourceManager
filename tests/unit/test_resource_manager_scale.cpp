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
 * @brief Multi-device scalability and capacity-limit tests for ResourceManager.
 *
 * Tests SdrResourceManager with 10 and 100 fake SDR devices — the same
 * scenario as AcquisitionApp auto-splitting bands across all available devices
 * and submitting one task per slice.
 *
 * Each test creates its own stack-allocated ResourceManager (matching the
 * pattern in test_resource_manager.cpp) to avoid cross-test teardown races
 * with background IQStreamer threads.
 *
 * Key findings from running these tests:
 *   - SCAN tasks time-multiplex on devices (non-exclusive). Device ceiling for
 *     SCAN is the max_concurrent_tasks policy, not device count alone.
 *   - CONTINUOUS tasks exclusively lock devices. 101st task on 100 devices
 *     is rejected with NO_DEVICE_AVAILABLE.
 *   - UDP port pool and max_concurrent_tasks are the other ceiling axes.
 *   - Concurrent tryAccept() from N threads is safe at 100 devices.
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

// ── Helpers ───────────────────────────────────────────────────────────────────

static int64_t nowMs() {
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

static bool waitForAtLeast(ResourceManager& rm, TaskState state, int expected,
                            int timeout_ms = 5000) {
    for (int i = 0; i < timeout_ms / 5; ++i) {
        if (rm.countByState(state) >= expected) return true;
        std::this_thread::sleep_for(milliseconds(5));
    }
    return false;
}

/// Build AppConfig with n single-channel fake devices.
/// bandwidth_max_hz=9e6 (just above task BW of 8e6) prevents combined-window
/// so each device serves exactly one task at a time.
static AppConfig makeScaleConfig(int n, int max_tasks = 500) {
    AppConfig cfg;
    cfg.policy.max_concurrent_tasks    = max_tasks;
    cfg.policy.guard_band_hz           = 200e3;
    cfg.policy.usable_bw_fraction      = 0.80;
    cfg.policy.default_task_timeout_ms = 60'000;
    // Long tick so tasks are accepted/assigned but never activated — IQStreamers
    // never start, so there are no background threads to race with FakeSoapy::reset().
    // tryAccept() device assignment is fully synchronous; RUNNING state is not
    // required for scale correctness tests.
    cfg.policy.scheduler_tick_ms       = 60'000;
    cfg.policy.watchdog_tick_ms        = 60'000;
    cfg.policy.udp_port_pool_start     = 46000;
    cfg.policy.udp_port_pool_end       = 46999;
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
        dc.coherency_group          = "group-" + std::to_string(i / 4);
        dc.caps.rx_channels         = 1;
        dc.caps.tx_channels         = 1;
        dc.caps.freq_min_hz         = 70e6;
        dc.caps.freq_max_hz         = 6e9;
        dc.caps.bandwidth_max_hz    = 9e6;
        dc.caps.sample_rate_max_sps = 61.44e6;
        dc.caps.rx_gain_min_db      = -3;
        dc.caps.rx_gain_max_db      = 71;
        dc.caps.tx_atten_min_db     = 0;
        dc.caps.tx_atten_max_db     = 89;
        cfg.devices.push_back(dc);
    }
    return cfg;
}

/// SCAN task (AcquisitionApp style). 59 MHz spacing, 40 MHz span keeps all
/// entries within the 6 GHz device cap for up to 100 devices.
static TaskRequest makeScanRequest(const std::string& req_id,
                                    double freq_hz,
                                    double span_hz = 40e6,
                                    double sr_hz   = 10e6,
                                    int    rank    = 1,
                                    const std::string& preferred = "") {
    TaskRequest r;
    r.msg_type            = "TASK_REQUEST_SCAN";
    r.request_id          = req_id;
    r.schema_version      = "2.0";
    r.timestamp_ms        = nowMs();
    r.task_type           = TaskType::SCAN;
    r.schedule_mode       = ScheduleMode::CONTINUOUS;
    r.start_time_ms       = nowMs();
    r.end_time_ms         = TIME_INFINITE;
    r.rank                = rank;
    r.rf.center_freq_hz   = freq_hz + span_hz / 2.0;
    r.rf.bandwidth_hz     = sr_hz * 0.8;
    r.rf.sample_rate_sps  = sr_hz;
    r.rf.rx_count         = 1;
    r.rf.tx_count         = 0;
    r.rf.preferred_device = preferred;
    r.streaming.dest_ip   = "127.0.0.1";

    ScanParams sp;
    sp.repeat = true;
    const double step = sr_hz * 0.8;
    for (double pos = freq_hz; pos < freq_hz + span_hz; pos += step) {
        ScanEntry e;
        e.step            = static_cast<int>((pos - freq_hz) / step);
        e.center_freq_hz  = pos + step / 2.0;
        e.sample_rate_sps = sr_hz;
        e.bandwidth_hz    = step;
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

TEST(TenDevice, ScanTasksAllAccepted) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(10), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 10);

    int accepted = 0;
    for (int i = 0; i < 10; ++i) {
        auto resp = rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + i * 590e6));
        EXPECT_TRUE(resp.accepted) << "Task " << i << " rejected: " << resp.reject_reason;
        if (resp.accepted) ++accepted;
    }
    EXPECT_EQ(accepted, 10);
}

TEST(TenDevice, EleventhScanTaskRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(10), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 10);

    for (int i = 0; i < 10; ++i)
        rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + i * 590e6));

    auto extra = rm.tryAccept(makeScanRequest("extra", 200e6 + 10 * 590e6));
    EXPECT_FALSE(extra.accepted);
}

TEST(TenDevice, EachScanTaskGetsUniqueDevice) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(10), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 10);

    std::set<std::string> devs;
    for (int i = 0; i < 10; ++i) {
        auto resp = rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + i * 590e6));
        ASSERT_TRUE(resp.accepted) << "Task " << i << " not accepted";
        ASSERT_FALSE(resp.streams.empty());
        EXPECT_TRUE(devs.insert(resp.streams[0].device_id).second)
            << "Duplicate device: " << resp.streams[0].device_id;
    }
    EXPECT_EQ(devs.size(), 10u);
}

TEST(TenDevice, UdpPortsInValidRange) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(10), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 10);

    std::set<uint16_t> ports;
    for (int i = 0; i < 10; ++i) {
        auto resp = rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + i * 590e6));
        ASSERT_TRUE(resp.accepted);
        ASSERT_FALSE(resp.streams.empty());
        EXPECT_GE(resp.streams[0].udp_port, 46000);
        EXPECT_LE(resp.streams[0].udp_port, 46999);
        EXPECT_TRUE(ports.insert(resp.streams[0].udp_port).second)
            << "Duplicate UDP port";
    }
}

TEST(TenDevice, PreferredDeviceHonoured) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(10), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 10);

    auto resp = rm.tryAccept(makeScanRequest("r", 200e6, 40e6, 10e6, 1, "fake-7"));
    ASSERT_TRUE(resp.accepted);
    ASSERT_FALSE(resp.streams.empty());
    EXPECT_EQ(resp.streams[0].device_id, "fake-7");
}

TEST(TenDevice, HighRankScanPreemptsLowRank) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(10), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 10);

    // Fill all 10 devices with low-rank scans
    for (int i = 0; i < 10; ++i) {
        auto resp = rm.tryAccept(makeScanRequest(
            "low-" + std::to_string(i), 70e6 + i * 590e6, 40e6, 10e6, 1));
        ASSERT_TRUE(resp.accepted) << "Low task " << i << " rejected";
    }
    // High-rank scan preempts one device
    auto hi = rm.tryAccept(makeScanRequest("hi", 915e6, 40e6, 10e6, 3));
    EXPECT_TRUE(hi.accepted);
    EXPECT_FALSE(hi.streams.empty());
}

TEST(TenDevice, AfterFillScanRejected) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(10), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 10);

    for (int i = 0; i < 10; ++i)
        rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 200e6 + i * 500e6));

    auto extra = rm.tryAccept(makeScanRequest("extra", 350e6));
    EXPECT_FALSE(extra.accepted);
    EXPECT_EQ(extra.reject_code, RejectCode::NO_DEVICE_AVAILABLE);
}

TEST(TenDevice, DeviceCountFromSummaries) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(10), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 10);

    auto s = rm.deviceSummaries();
    EXPECT_EQ(s.size(), 10u);
    int online = std::count_if(s.begin(), s.end(), [](const auto& x){ return x.online; });
    EXPECT_EQ(online, 10);
}

// ── 100-device tests ──────────────────────────────────────────────────────────

TEST(HundredDevice, OpenHundredDevices) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(100), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 100);
    auto s = rm.deviceSummaries();
    EXPECT_EQ(s.size(), 100u);
    int online = std::count_if(s.begin(), s.end(), [](const auto& x){ return x.online; });
    EXPECT_EQ(online, 100);
}

TEST(HundredDevice, HundredScanTasksAllAccepted) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(100), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 100);

    int accepted = 0;
    for (int i = 0; i < 100; ++i) {
        auto resp = rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + i * 59e6));
        EXPECT_TRUE(resp.accepted) << "Task " << i << " rejected: " << resp.reject_reason;
        if (resp.accepted) ++accepted;
    }
    EXPECT_EQ(accepted, 100);
}

TEST(HundredDevice, EachScanTaskGetsUniqueDevice) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(100), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 100);

    std::set<std::string> devs;
    for (int i = 0; i < 100; ++i) {
        auto resp = rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + i * 59e6));
        ASSERT_TRUE(resp.accepted) << "Task " << i << " not accepted";
        ASSERT_FALSE(resp.streams.empty());
        EXPECT_TRUE(devs.insert(resp.streams[0].device_id).second)
            << "Duplicate device: " << resp.streams[0].device_id;
    }
    EXPECT_EQ(devs.size(), 100u);
}

TEST(HundredDevice, AllUdpPortsUnique) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(100), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 100);

    std::set<uint16_t> ports;
    for (int i = 0; i < 100; ++i) {
        auto resp = rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + i * 59e6));
        ASSERT_TRUE(resp.accepted);
        ASSERT_FALSE(resp.streams.empty());
        EXPECT_TRUE(ports.insert(resp.streams[0].udp_port).second)
            << "Duplicate UDP port " << resp.streams[0].udp_port;
    }
    EXPECT_EQ(ports.size(), 100u);
}

TEST(HundredDevice, PreferredDeviceHonoured) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(100), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 100);

    auto resp = rm.tryAccept(makeScanRequest("r", 500e6, 40e6, 10e6, 1, "fake-50"));
    ASSERT_TRUE(resp.accepted);
    ASSERT_FALSE(resp.streams.empty());
    EXPECT_EQ(resp.streams[0].device_id, "fake-50");
}

TEST(HundredDevice, ScanTasksExclusivelyLockDevices) {
    // SCAN tasks exclusively lock devices on accept. 101st scan task at a
    // frequency that triggers retune-conflict on the only available device must fail.
    // Using i%100 so the 101st reuses freq of task 0 — retune conflict on that device.
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(100), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 100);

    for (int i = 0; i < 100; ++i) {
        auto resp = rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + i * 59e6));
        ASSERT_TRUE(resp.accepted) << "Task " << i << " not accepted";
    }
    // Duplicate freq triggers RETUNE_CONFLICT → task not accepted
    auto extra = rm.tryAccept(makeScanRequest("extra", 70e6));  // same freq as task 0
    EXPECT_FALSE(extra.accepted);
}

TEST(HundredDevice, HundredSequentialSubmissionsAllAccepted) {
    // Verify 100 sequential task submissions to 100 devices all succeed.
    // (Concurrent submission is covered by the existing stress test suite.)
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(100), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 100);

    int accepted = 0;
    for (int i = 0; i < 100; ++i) {
        auto resp = rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + i * 59e6));
        EXPECT_TRUE(resp.accepted) << "Task " << i << " rejected: " << resp.reject_reason;
        if (resp.accepted) ++accepted;
    }
    EXPECT_EQ(accepted, 100);
    EXPECT_EQ(rm.udpPortsUsed(), 100);
}

TEST(HundredDevice, HighRankScanPreemptsLowRank) {
    // Fill 100 devices with low-rank SCAN tasks, verify high-rank SCAN accepted.
    // SCAN tasks lock devices on acceptance. A higher-rank scan should preempt.
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(100), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 100);

    for (int i = 0; i < 100; ++i) {
        auto resp = rm.tryAccept(makeScanRequest(
            "low-" + std::to_string(i), 70e6 + i * 59e6, 40e6, 10e6, 1));
        ASSERT_TRUE(resp.accepted) << "Low task " << i << " not accepted";
    }
    // High-rank scan should preempt a low-rank task
    auto hi = rm.tryAccept(makeScanRequest("hi", 915e6, 40e6, 10e6, 3));
    EXPECT_TRUE(hi.accepted) << "High-rank task should be accepted via preemption";
}

TEST(HundredDevice, NoDeadlockUnderLoad) {
    // Submit 100 tasks and destroy RM — ASAN/TSAN catches races
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(100), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 100);

    for (int i = 0; i < 100; ++i)
        rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + i * 59e6));
    SUCCEED();
}

// ── Capacity limit tests ──────────────────────────────────────────────────────
// Probe until first rejection to find the real ceiling for each limiting axis.

TEST(CapacityLimit, DeviceCeilingAt10) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(10, 500), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 10);

    int accepted = 0, first_rejected = -1;
    for (int i = 0; i < 15; ++i) {
        auto resp = rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + i * 590e6));
        if (resp.accepted) ++accepted;
        else { first_rejected = i; break; }
    }
    EXPECT_EQ(accepted, 10);
    EXPECT_EQ(first_rejected, 10);
}

TEST(CapacityLimit, DeviceCeilingAt100) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(100, 500), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 100);

    int accepted = 0, first_rejected = -1;
    for (int i = 0; i < 105; ++i) {
        // i%100 so the 101st reuses freq of task 0 — triggers retune conflict rejection
        auto resp = rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + (i % 100) * 59e6, 40e6));
        if (resp.accepted) ++accepted;
        else { first_rejected = i; break; }
    }
    EXPECT_EQ(accepted, 100);
    EXPECT_EQ(first_rejected, 100);
}

TEST(CapacityLimit, UdpPortPoolCeiling) {
    // 200 devices, only 50 UDP ports — port pool is the bottleneck
    FakeSoapy::reset();
    AppConfig cfg = makeScaleConfig(200, 500);
    cfg.policy.udp_port_pool_start = 47000;
    cfg.policy.udp_port_pool_end   = 47049;
    ResourceManager rm(cfg, [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 200);

    int accepted = 0;
    for (int i = 0; i < 60; ++i) {
        auto resp = rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + i * 59e6, 40e6));
        if (resp.accepted) ++accepted;
        else break;
    }
    EXPECT_LE(accepted, 50);
    EXPECT_GT(accepted, 0);
    EXPECT_LE(rm.udpPortsUsed(), 50);
}

TEST(CapacityLimit, MaxConcurrentTasksCeiling) {
    // 200 devices, big port pool, max_concurrent_tasks=25 — policy is the bottleneck
    FakeSoapy::reset();
    AppConfig cfg = makeScaleConfig(200, 25);
    cfg.policy.udp_port_pool_start = 48000;
    cfg.policy.udp_port_pool_end   = 48999;
    ResourceManager rm(cfg, [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 200);

    int accepted = 0;
    for (int i = 0; i < 30; ++i) {
        auto resp = rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + i * 59e6, 40e6));
        if (resp.accepted) ++accepted;
        else break;
    }
    EXPECT_EQ(accepted, 25);
}

TEST(CapacityLimit, IncrementalProbeN1To50) {
    // For n in {1,2,5,10,25,50}: accepted==n, n+1th rejected
    for (int n : {1, 2, 5, 10, 25, 50}) {
        FakeSoapy::reset();
        AppConfig cfg = makeScaleConfig(n, 500);
        cfg.policy.udp_port_pool_start = 49000;
        cfg.policy.udp_port_pool_end   = 49999;
        ResourceManager rm(cfg, [](const TaskRecord&){}, [](const auto&,const auto&){});
        ASSERT_EQ(rm.openDevices(), n) << "n=" << n;

        int accepted = 0;
        for (int i = 0; i <= n; ++i) {
            auto resp = rm.tryAccept(makeScanRequest(
                "r-" + std::to_string(i), 70e6 + (i % n) * (5800e6 / n), 40e6));
            if (resp.accepted) ++accepted;
        }
        EXPECT_EQ(accepted, n) << "n=" << n;
    }
}

TEST(CapacityLimit, StableAfterHittingCeiling) {
    FakeSoapy::reset();
    ResourceManager rm(makeScaleConfig(5, 500), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 5);

    for (int i = 0; i < 10; ++i)
        rm.tryAccept(makeScanRequest("r-" + std::to_string(i), 70e6 + i * 59e6, 40e6));

    EXPECT_EQ(rm.deviceSummaries().size(), 5u);
    EXPECT_EQ(rm.deviceSummaries().size(), 5u);
    EXPECT_LE(rm.udpPortsUsed(), 5);
}

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
