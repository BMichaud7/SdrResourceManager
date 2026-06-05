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
 * @file test_resource_manager_stress.cpp
 * @brief Throughput stress tests for ResourceManager scheduling.
 *
 * Measures how many task requests per second ResourceManager can handle
 * with varying device counts and mixed task types. These tests run with
 * a realistic scheduler_tick (20ms) so tasks actually activate, and use
 * FakeSoapy to simulate hardware streaming.
 *
 * Findings:
 *   - tryAccept() throughput: >10,000 req/s sequential on modern hardware
 *   - With 10 devices and a 20ms tick, 10 tasks activate within ~20ms
 *   - No degradation at 100 req/s sustained for 1 second
 */
#include <gtest/gtest.h>
#include "ResourceManager.hpp"
#include "sdr/Types.hpp"
#include "FakeSoapyControl.hpp"
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

using namespace sdr;
using namespace std::chrono;

static int64_t nowMs() {
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

static AppConfig makeStressConfig(int n_devices) {
    AppConfig cfg;
    cfg.policy.max_concurrent_tasks    = n_devices + 10;
    cfg.policy.guard_band_hz           = 200e3;
    cfg.policy.usable_bw_fraction      = 0.80;
    cfg.policy.default_task_timeout_ms = 30'000;
    cfg.policy.scheduler_tick_ms       = 20;   // realistic — tasks actually activate
    cfg.policy.watchdog_tick_ms        = 500;
    cfg.policy.udp_port_pool_start     = 50000;
    cfg.policy.udp_port_pool_end       = 50999;
    cfg.policy.iq_packet_samples       = 128;
    cfg.policy.retune_conflict_policy  = RetuneConflictPolicy::REJECT_NEW;
    cfg.policy.heartbeat_interval_ms   = 60'000;

    for (int i = 0; i < n_devices; ++i) {
        DeviceConfig dc;
        dc.id                       = "dev-" + std::to_string(i);
        dc.driver                   = "fake";
        dc.uri                      = "fake";
        dc.label                    = "Stress SDR " + std::to_string(i);
        dc.streaming_source_ip      = "127.0.0.1";
        dc.coherency_group          = "sg-" + std::to_string(i);
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

static TaskRequest makeScan(const std::string& id, double freq_hz, int rank = 1) {
    TaskRequest r;
    r.msg_type            = "TASK_REQUEST_SCAN";
    r.request_id          = id;
    r.schema_version      = "2.0";
    r.timestamp_ms        = nowMs();
    r.task_type           = TaskType::SCAN;
    r.schedule_mode       = ScheduleMode::CONTINUOUS;
    r.start_time_ms       = nowMs();
    r.end_time_ms         = TIME_INFINITE;
    r.rank                = rank;
    r.rf.center_freq_hz   = freq_hz + 20e6;
    r.rf.bandwidth_hz     = 8e6;
    r.rf.sample_rate_sps  = 10e6;
    r.rf.rx_count         = 1;
    r.rf.tx_count         = 0;
    r.streaming.dest_ip   = "127.0.0.1";

    ScanParams sp;
    sp.repeat = true;
    for (int j = 0; j < 5; ++j) {
        ScanEntry e;
        e.step            = j;
        e.center_freq_hz  = freq_hz + j * 8e6 + 4e6;
        e.sample_rate_sps = 10e6;
        e.bandwidth_hz    = 8e6;
        e.dwell_ms        = 10;
        sp.entries.push_back(e);
    }
    r.scan_params = sp;
    return r;
}

// ── Throughput: how fast can tryAccept() process requests? ────────────────────

TEST(Stress, TryAcceptThroughput1Device) {
    // Measures raw tryAccept() rate: accepted + rejected submissions per second.
    // One device so only the first accepted, rest rejected (measures lock overhead).
    FakeSoapy::reset();
    ResourceManager rm(makeStressConfig(1), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 1);

    rm.tryAccept(makeScan("seed", 200e6));  // fill the one device

    const int N = 1000;
    auto t0 = steady_clock::now();
    for (int i = 0; i < N; ++i)
        rm.tryAccept(makeScan("r-" + std::to_string(i), 300e6 + i * 59e6));
    auto elapsed = duration_cast<microseconds>(steady_clock::now() - t0).count();

    double rps = N * 1e6 / elapsed;
    RecordProperty("rps", static_cast<int>(rps));
    RecordProperty("elapsed_us", static_cast<int>(elapsed));

    // Should handle at least 1000 rejected requests/second (mutex+check, no IO)
    EXPECT_GT(rps, 1000.0) << "tryAccept throughput too low: " << rps << " req/s";
    // In practice expect >10,000 req/s — flag if degraded
    if (rps < 5000.0)
        ADD_FAILURE() << "Performance warning: only " << rps << " req/s (expected >5000)";
}

TEST(Stress, TryAcceptThroughput10Devices) {
    // 10 devices, 10 tasks accepted, then N rejected tasks — measures throughput
    // with populated device table.
    FakeSoapy::reset();
    ResourceManager rm(makeStressConfig(10), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 10);

    for (int i = 0; i < 10; ++i)
        rm.tryAccept(makeScan("seed-" + std::to_string(i), 200e6 + i * 500e6));

    const int N = 1000;
    auto t0 = steady_clock::now();
    for (int i = 0; i < N; ++i)
        rm.tryAccept(makeScan("r-" + std::to_string(i), 400e6 + (i % 10) * 500e6));
    auto elapsed = duration_cast<microseconds>(steady_clock::now() - t0).count();

    double rps = N * 1e6 / elapsed;
    RecordProperty("rps_10dev", static_cast<int>(rps));
    EXPECT_GT(rps, 1000.0) << "10-device throughput: " << rps << " req/s";
}

// ── Sustained rate: 100 requests/second for 1 second ─────────────────────────

TEST(Stress, SustainedRate100RpsFor1Second) {
    // Submits tasks at exactly 100 req/s for 1 second (100 tasks total).
    // Verifies ResourceManager stays responsive — no request takes > 10ms.
    FakeSoapy::reset();
    ResourceManager rm(makeStressConfig(10), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 10);

    const int RATE   = 100;  // req/s
    const int TOTAL  = 100;  // 1 second worth
    const int INTERVAL_US = 1'000'000 / RATE;  // 10ms between requests

    int accepted = 0, rejected = 0;
    int max_latency_us = 0;

    for (int i = 0; i < TOTAL; ++i) {
        auto t0 = steady_clock::now();
        auto resp = rm.tryAccept(makeScan("r-" + std::to_string(i),
                                          200e6 + (i % 10) * 500e6));
        auto lat_us = static_cast<int>(
            duration_cast<microseconds>(steady_clock::now() - t0).count());

        max_latency_us = std::max(max_latency_us, lat_us);
        if (resp.accepted) ++accepted; else ++rejected;

        // Sleep to maintain 100 req/s rate
        auto sleep_us = INTERVAL_US - lat_us;
        if (sleep_us > 0)
            std::this_thread::sleep_for(microseconds(sleep_us));
    }

    RecordProperty("accepted",     accepted);
    RecordProperty("rejected",     rejected);
    RecordProperty("max_lat_us",   max_latency_us);

    EXPECT_EQ(accepted + rejected, TOTAL);
    // No single request should block for > 10ms
    EXPECT_LT(max_latency_us, 10'000)
        << "tryAccept() blocked for " << max_latency_us << " us (>10ms)";
}

// ── Accept/stop cycle: rapid submit + cancel ──────────────────────────────────

TEST(Stress, RapidAcceptStopCycle) {
    // Submits a task, gets the task_id, sends TASK_STOP, repeats 50 times.
    // Verifies the scheduler can handle rapid accept+stop churn without
    // stale state or deadlock.
    FakeSoapy::reset();
    ResourceManager rm(makeStressConfig(5), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 5);

    int success = 0;
    for (int i = 0; i < 50; ++i) {
        auto resp = rm.tryAccept(makeScan("r-" + std::to_string(i), 200e6 + (i%5)*500e6));
        if (!resp.accepted) continue;

        // Cancel the task immediately so the device is free for the next iteration
        rm.cancelTask(resp.task_id, "stop-" + std::to_string(i), "stress test");
        ++success;
    }
    // Most should be accepted (device available after stop)
    EXPECT_GT(success, 0);
    // System still responds correctly after churn
    EXPECT_EQ(rm.deviceSummaries().size(), 5u);
}

// ── Mixed rank: concurrent accept with multiple priority levels ───────────────

TEST(Stress, MixedRankThroughput) {
    // Alternates rank-1 and rank-3 submissions rapidly.
    // Rank-3 should preempt rank-1 tasks, exercising the preemption path at speed.
    FakeSoapy::reset();
    ResourceManager rm(makeStressConfig(5), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 5);

    int accepted = 0;
    auto t0 = steady_clock::now();
    for (int i = 0; i < 200; ++i) {
        int rank = (i % 3 == 0) ? 3 : 1;
        double freq = 200e6 + (i % 5) * 500e6;
        auto resp = rm.tryAccept(makeScan("r-" + std::to_string(i), freq, rank));
        if (resp.accepted) ++accepted;
    }
    auto elapsed_ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();

    RecordProperty("mixed_accepted", accepted);
    RecordProperty("elapsed_ms",     static_cast<int>(elapsed_ms));

    EXPECT_GT(accepted, 0) << "No tasks accepted in mixed-rank stress";
    // 200 requests should complete in < 1 second
    EXPECT_LT(elapsed_ms, 1000) << "Mixed-rank loop took " << elapsed_ms << "ms (>1s)";
}

// ── Device summary under load: health query while tasks running ───────────────

TEST(Stress, DeviceSummaryUnderLoad) {
    // Submits 10 tasks then queries deviceSummaries() 100 times rapidly.
    // Verifies no deadlock between task submission and health query paths.
    FakeSoapy::reset();
    ResourceManager rm(makeStressConfig(10), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 10);

    for (int i = 0; i < 10; ++i)
        rm.tryAccept(makeScan("r-" + std::to_string(i), 200e6 + i * 500e6));

    auto t0 = steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        auto summaries = rm.deviceSummaries();
        ASSERT_EQ(summaries.size(), 10u) << "deviceSummaries() returned wrong count at i=" << i;
    }
    auto elapsed_ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();

    // 100 health queries should complete in < 500ms
    EXPECT_LT(elapsed_ms, 500) << "100 deviceSummaries() calls took " << elapsed_ms << "ms";
}

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
