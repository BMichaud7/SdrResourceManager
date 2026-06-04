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
#include "TriggerMonitor.hpp"
#include "FakeSoapyDevice.hpp"
#include "FakeSoapyControl.hpp"
#include <SoapySDR/Formats.hpp>
#include <atomic>
#include <chrono>
#include <thread>

using namespace sdr;

// With sample_value=v and BLK=256:
//   rms = 10*log10(2*v^2 + 1e-30)
//   v=1.0 → ~3 dBFS   (above -50 threshold)
//   v=0.0 → ~-297 dBFS (below any threshold)
static constexpr float THRESHOLD = -50.0f;

static TriggerParams makeParams(int max_caps = 1, int pre_ms = 0, int post_ms = 1) {
    TriggerParams p;
    p.threshold_dbfs  = THRESHOLD;
    p.max_captures    = max_caps;
    p.pre_trigger_ms  = pre_ms;
    p.post_trigger_ms = post_ms;
    return p;
}

static bool waitFor(std::function<bool()> cond, int timeout_ms = 1000) {
    for (int i = 0; i < timeout_ms / 5; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// ── No trigger ───────────────────────────────────────────────────────────────

TEST(TriggerMonitor, BelowThresholdNeverCaptures) {
    FakeSoapy::reset();
    FakeSoapy::sample_value.store(0.0f);    // ~-297 dBFS — well below threshold
    FakeSoapy::read_delay_us.store(200);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    TriggerMonitor mon("trig-1", makeParams(1),
                       &dev, stream, nullptr, 1000.0,
                       [](const std::string&, bool) {});
    mon.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mon.stop();

    EXPECT_EQ(mon.captureCount(), 0);
}

// ── Single capture ────────────────────────────────────────────────────────────

TEST(TriggerMonitor, AboveThresholdCompletesOneCapture) {
    FakeSoapy::reset();
    FakeSoapy::sample_value.store(1.0f);    // ~3 dBFS — above threshold
    FakeSoapy::read_delay_us.store(100);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    std::atomic<bool> done_called{false};
    TriggerMonitor mon("trig-2", makeParams(1, 0, 1),
                       &dev, stream, nullptr, 1000.0,
                       [&](const std::string&, bool) { done_called = true; });
    mon.start();

    EXPECT_TRUE(waitFor([&] { return done_called.load(); }));
    mon.stop();

    EXPECT_EQ(mon.captureCount(), 1);
    EXPECT_FALSE(mon.isRunning());
}

// ── max_captures limit ────────────────────────────────────────────────────────

TEST(TriggerMonitor, MaxCapturesLimitHonored) {
    FakeSoapy::reset();
    FakeSoapy::sample_value.store(1.0f);
    FakeSoapy::read_delay_us.store(100);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    TriggerMonitor mon("trig-3", makeParams(3, 0, 1),
                       &dev, stream, nullptr, 1000.0,
                       [](const std::string&, bool) {});
    mon.start();

    // isRunning() reflects running_ (cleared only by stop()), not loop completion.
    // Wait for captureCount to reach the limit instead.
    EXPECT_TRUE(waitFor([&] { return mon.captureCount() >= 3; }));
    mon.stop();
    EXPECT_EQ(mon.captureCount(), 3);
}

// ── max_captures=0 runs indefinitely ─────────────────────────────────────────

TEST(TriggerMonitor, ZeroMaxCapturesRunsUntilExplicitStop) {
    FakeSoapy::reset();
    FakeSoapy::sample_value.store(1.0f);
    FakeSoapy::read_delay_us.store(100);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    TriggerMonitor mon("trig-4", makeParams(0, 0, 1),
                       &dev, stream, nullptr, 1000.0,
                       [](const std::string&, bool) {});
    mon.start();

    // Let it accumulate multiple captures
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(mon.isRunning());
    EXPECT_GT(mon.captureCount(), 0);

    mon.stop();
    EXPECT_FALSE(mon.isRunning());
}

// ── Timeout/error resilience ──────────────────────────────────────────────────

TEST(TriggerMonitor, TimeoutsSkippedWithoutSpuriousTrigger) {
    FakeSoapy::reset();
    FakeSoapy::timeout_count.store(20); // 20 timeouts, then below-threshold reads
    FakeSoapy::sample_value.store(0.0f);
    FakeSoapy::read_delay_us.store(100);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    TriggerMonitor mon("trig-5", makeParams(1),
                       &dev, stream, nullptr, 1000.0,
                       [](const std::string&, bool) {});
    mon.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mon.stop();

    EXPECT_EQ(mon.captureCount(), 0);
}

// ── Done callback fires ───────────────────────────────────────────────────────

TEST(TriggerMonitor, DoneCallbackFiredWhenMaxCapturesReached) {
    FakeSoapy::reset();
    FakeSoapy::sample_value.store(1.0f);
    FakeSoapy::read_delay_us.store(100);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    std::atomic<bool> done_called{false};
    std::atomic<bool> done_ok{false};
    TriggerMonitor mon("trig-6", makeParams(2, 0, 1),
                       &dev, stream, nullptr, 1000.0,
                       [&](const std::string&, bool ok) { done_ok = ok; done_called = true; });
    mon.start();

    EXPECT_TRUE(waitFor([&] { return done_called.load(); }));
    mon.stop();
    EXPECT_TRUE(done_ok.load());
    EXPECT_EQ(mon.captureCount(), 2);
}
