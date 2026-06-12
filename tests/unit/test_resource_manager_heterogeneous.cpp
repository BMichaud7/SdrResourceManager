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
 * @file test_resource_manager_heterogeneous.cpp
 * @brief Mixed-device-type tests: 4x PlutoSDR-class (2 RX/2 TX, shared_lo,
 * 70 MHz-6 GHz) + 2x RTL-SDR-class (1 RX/0 TX, independent LO, 24-1766 MHz)
 * in a single ResourceManager pool, matching the SoapySDR device-pool model
 * documented in config/devices.xml ("no code changes required to add a board").
 */
#include <gtest/gtest.h>
#include "ResourceManager.hpp"
#include "sdr/Types.hpp"
#include "sdr/MessageCodec.hpp"
#include "FakeSoapyControl.hpp"
#include <set>
#include <string>

using namespace sdr;
using namespace std::chrono;

static int64_t nowMs() {
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

// 4 Pluto-class + 2 RTL-SDR-class devices on the "fake" SoapySDR driver.
static AppConfig makeMixedConfig() {
    AppConfig cfg;
    cfg.policy.max_concurrent_tasks    = 64;
    cfg.policy.guard_band_hz           = 200e3;
    cfg.policy.usable_bw_fraction      = 0.80;
    cfg.policy.default_task_timeout_ms = 60'000;
    cfg.policy.scheduler_tick_ms       = 60'000;
    cfg.policy.watchdog_tick_ms        = 60'000;
    cfg.policy.udp_port_pool_start     = 47000;
    cfg.policy.udp_port_pool_end       = 47999;
    cfg.policy.iq_packet_samples       = 128;
    cfg.policy.retune_conflict_policy  = RetuneConflictPolicy::REJECT_NEW;
    cfg.policy.heartbeat_interval_ms   = 10'000;

    for (int i = 0; i < 4; ++i) {
        DeviceConfig dc;
        dc.id                       = "pluto-" + std::to_string(i);
        dc.driver                   = "fake";
        dc.uri                      = "fake";
        dc.label                    = "Fake PlutoSDR " + std::to_string(i);
        dc.streaming_source_ip      = "127.0.0.1";
        dc.coherency_group          = "pluto-refclk";
        dc.shared_lo                = true;
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

    for (int i = 0; i < 2; ++i) {
        DeviceConfig dc;
        dc.id                       = "rtlsdr-" + std::to_string(i);
        dc.driver                   = "fake";
        dc.uri                      = "fake";
        dc.label                    = "Fake RTL-SDR " + std::to_string(i);
        dc.streaming_source_ip      = "127.0.0.1";
        dc.coherency_group          = "rtlsdr-" + std::to_string(i); // independent
        dc.shared_lo                = false;
        dc.caps.rx_channels         = 1;
        dc.caps.tx_channels         = 0;
        dc.caps.freq_min_hz         = 24e6;
        dc.caps.freq_max_hz         = 1.766e9;
        dc.caps.bandwidth_max_hz    = 3.2e6;
        dc.caps.sample_rate_max_sps = 3.2e6;
        dc.caps.rx_gain_min_db      = 0;
        dc.caps.rx_gain_max_db      = 49.6;
        dc.caps.tx_atten_min_db     = 0;
        dc.caps.tx_atten_max_db     = 0;
        cfg.devices.push_back(dc);
    }

    return cfg;
}

static TaskRequest makeContinuousRequest(const std::string& req_id,
                                          double cf, double bw, double sr,
                                          int rx_count = 1, int tx_count = 0,
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
    r.rf.rx_count         = rx_count;
    r.rf.tx_count         = tx_count;
    r.rf.preferred_device = preferred;
    r.streaming.dest_ip   = "127.0.0.1";
    return r;
}

// ── Pool composition ─────────────────────────────────────────────────────────

TEST(MixedPool, AllSixDevicesOpen) {
    FakeSoapy::reset();
    ResourceManager rm(makeMixedConfig(), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 6);

    auto s = rm.deviceSummaries();
    EXPECT_EQ(s.size(), 6u);
    int online = std::count_if(s.begin(), s.end(), [](const auto& x){ return x.online; });
    EXPECT_EQ(online, 6);
}

// ── PlutoSDR-class devices ───────────────────────────────────────────────────

TEST(MixedPool, AllFourPlutosAcceptDualChannelTask) {
    FakeSoapy::reset();
    ResourceManager rm(makeMixedConfig(), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 6);

    std::set<std::string> devs;
    for (int i = 0; i < 4; ++i) {
        // 2 RX + 2 TX task, well within Pluto's 70MHz-6GHz / 2ch capability
        auto resp = rm.tryAccept(makeContinuousRequest(
            "pluto-task-" + std::to_string(i), 433e6 + i * 100e6, 8e6, 10e6,
            /*rx*/2, /*tx*/2));
        ASSERT_TRUE(resp.accepted) << "Pluto task " << i << " rejected: " << resp.reject_reason;
        ASSERT_FALSE(resp.streams.empty());
        devs.insert(resp.streams[0].device_id);
    }
    EXPECT_EQ(devs.size(), 4u);
    for (const auto& d : devs) EXPECT_EQ(d.rfind("pluto-", 0), 0u);
}

TEST(MixedPool, PlutoRejectsBeyond6GHz) {
    FakeSoapy::reset();
    ResourceManager rm(makeMixedConfig(), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 6);

    auto resp = rm.tryAccept(makeContinuousRequest("too-high", 7e9, 8e6, 10e6, 2, 2));
    EXPECT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::FREQ_OUT_OF_RANGE);
}

// ── RTL-SDR-class devices ────────────────────────────────────────────────────

TEST(MixedPool, BothRtlSdrsAcceptSingleChannelTask) {
    FakeSoapy::reset();
    ResourceManager rm(makeMixedConfig(), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 6);

    std::set<std::string> devs;
    for (int i = 0; i < 2; ++i) {
        // 1 RX / 0 TX task within RTL-SDR's 24MHz-1.766GHz capability
        auto resp = rm.tryAccept(makeContinuousRequest(
            "rtl-task-" + std::to_string(i), 100e6 + i * 50e6, 2e6, 2.4e6,
            /*rx*/1, /*tx*/0));
        ASSERT_TRUE(resp.accepted) << "RTL task " << i << " rejected: " << resp.reject_reason;
        ASSERT_FALSE(resp.streams.empty());
        EXPECT_EQ(resp.streams[0].channel_index, 0);
        devs.insert(resp.streams[0].device_id);
    }
    EXPECT_EQ(devs.size(), 2u);
    for (const auto& d : devs) EXPECT_EQ(d.rfind("rtlsdr-", 0), 0u);
}

// An RTL-SDR-only pool, used to confirm the device class's own capability
// limits are enforced when no Pluto-class device is present to pick up the
// task instead (preferred_device is only a placement hint, not a hard
// constraint, so these checks must be done with an RTL-SDR-only pool).
static AppConfig makeRtlOnlyConfig() {
    AppConfig cfg = makeMixedConfig();
    cfg.devices.erase(cfg.devices.begin(), cfg.devices.begin() + 4);
    return cfg;
}

TEST(MixedPool, RtlSdrRejectsAboveFreqCeiling) {
    FakeSoapy::reset();
    ResourceManager rm(makeRtlOnlyConfig(), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 2);

    // 2.4 GHz is above RTL-SDR's 1.766 GHz ceiling; with no Pluto-class
    // device in the pool, this must be rejected as out of range.
    auto resp = rm.tryAccept(makeContinuousRequest(
        "rtl-too-high", 2.4e9, 2e6, 2.4e6, 1, 0));
    EXPECT_FALSE(resp.accepted);
    EXPECT_EQ(resp.reject_code, RejectCode::FREQ_OUT_OF_RANGE);
}

TEST(MixedPool, RtlSdrRejectsTxRequest) {
    FakeSoapy::reset();
    ResourceManager rm(makeRtlOnlyConfig(), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 2);

    // RTL-SDR has tx_channels=0; with no Pluto-class device in the pool,
    // a task requiring TX must be rejected outright.
    auto resp = rm.tryAccept(makeContinuousRequest(
        "rtl-tx", 100e6, 2e6, 2.4e6, /*rx*/1, /*tx*/1));
    EXPECT_FALSE(resp.accepted);
}

// ── Full mixed pool exercised together ───────────────────────────────────────

TEST(MixedPool, AllSixDevicesServeOneTaskEach) {
    FakeSoapy::reset();
    ResourceManager rm(makeMixedConfig(), [](const TaskRecord&){}, [](const auto&,const auto&){});
    ASSERT_EQ(rm.openDevices(), 6);

    std::set<std::string> devs;

    for (int i = 0; i < 4; ++i) {
        auto resp = rm.tryAccept(makeContinuousRequest(
            "pluto-" + std::to_string(i), 433e6 + i * 100e6, 8e6, 10e6, 2, 2));
        ASSERT_TRUE(resp.accepted) << "pluto task " << i << ": " << resp.reject_reason;
        devs.insert(resp.streams[0].device_id);
    }
    for (int i = 0; i < 2; ++i) {
        auto resp = rm.tryAccept(makeContinuousRequest(
            "rtl-" + std::to_string(i), 100e6 + i * 50e6, 2e6, 2.4e6, 1, 0));
        ASSERT_TRUE(resp.accepted) << "rtl task " << i << ": " << resp.reject_reason;
        devs.insert(resp.streams[0].device_id);
    }

    EXPECT_EQ(devs.size(), 6u);

    // 7th task (another Pluto-class request) must be rejected — all exclusive
    // CONTINUOUS slots are taken.
    auto extra = rm.tryAccept(makeContinuousRequest("extra", 915e6, 8e6, 10e6, 2, 2));
    EXPECT_FALSE(extra.accepted);
    EXPECT_EQ(extra.reject_code, RejectCode::NO_DEVICE_AVAILABLE);
}

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
