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
#include "ScanExecutor.hpp"
#include "sdr/Types.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace sdr;

static ScanParams makeParams(int n, bool repeat = false, int dwell_ms = 10) {
    ScanParams p;
    p.repeat = repeat;
    for (int i = 0; i < n; ++i) {
        ScanEntry e;
        e.step            = i + 1;
        e.center_freq_hz  = 900e6 + i * 10e6;
        e.sample_rate_sps = 10e6;
        e.bandwidth_hz    = 5e6;
        e.dwell_ms        = dwell_ms;
        p.entries.push_back(e);
    }
    return p;
}

// Wait up to timeout_ms for condition to become true.
static bool waitFor(std::function<bool()> cond, int timeout_ms = 1000) {
    for (int i = 0; i < timeout_ms / 10; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// ── Step ordering ─────────────────────────────────────────────────────────────

TEST(ScanExecutor, VisitsAllStepsInOrder) {
    std::vector<double> visited;
    std::mutex mu;
    std::atomic<bool> done_called{false};

    ScanExecutor exec("task-1", makeParams(3, false, 1),
        [&](double cf, double) { std::lock_guard lk(mu); visited.push_back(cf); return true; },
        {},
        [&](const std::string&, bool) { done_called = true; });
    exec.start();

    // isRunning() reflects the running_ flag (cleared only by stop()), not loop completion.
    // Wait for the done callback instead.
    ASSERT_TRUE(waitFor([&] { return done_called.load(); }))
        << "ScanExecutor did not invoke done callback in time";

    std::lock_guard lk(mu);
    ASSERT_EQ(visited.size(), 3u);
    EXPECT_DOUBLE_EQ(visited[0], 900e6);
    EXPECT_DOUBLE_EQ(visited[1], 910e6);
    EXPECT_DOUBLE_EQ(visited[2], 920e6);
}

// ── Done callback ─────────────────────────────────────────────────────────────

TEST(ScanExecutor, NonRepeatCallsDoneWithTrue) {
    std::atomic<bool> done_called{false};
    std::atomic<bool> done_ok{false};

    ScanExecutor exec("task-2", makeParams(2, false, 1),
        [](double, double) { return true; },
        {},
        [&](const std::string&, bool ok) { done_ok = ok; done_called = true; });
    exec.start();

    EXPECT_TRUE(waitFor([&] { return done_called.load(); }));
    EXPECT_TRUE(done_ok.load());
}

TEST(ScanExecutor, EmptyScanCallsDoneImmediately) {
    std::atomic<bool> done_called{false};
    ScanParams empty;
    ScanExecutor exec("task-3", empty,
        [](double, double) { return true; },
        {},
        [&](const std::string&, bool) { done_called = true; });
    exec.start();

    EXPECT_TRUE(waitFor([&] { return done_called.load(); }, 200));
}

TEST(ScanExecutor, RetuneFailureCallsDoneWithFalse) {
    std::atomic<bool> done_called{false};
    std::atomic<bool> done_ok{true};

    ScanExecutor exec("task-4", makeParams(3, false, 1),
        [](double, double) { return false; },  // always fails
        {},
        [&](const std::string&, bool ok) { done_ok = ok; done_called = true; });
    exec.start();

    EXPECT_TRUE(waitFor([&] { return done_called.load(); }));
    EXPECT_FALSE(done_ok.load());
}

// ── Repeat mode ───────────────────────────────────────────────────────────────

TEST(ScanExecutor, RepeatModeKeepsCyclingUntilStopped) {
    std::atomic<int> visit_count{0};

    // The dwell inner-loop sleeps 50ms minimum regardless of dwell_ms, so
    // each entry costs ~50ms.  2 entries → ~100ms/pass.  In 600ms: ~6 passes.
    ScanExecutor exec("task-5", makeParams(2, true, 1),
        [&](double, double) { ++visit_count; return true; },
        {},
        [](const std::string&, bool) {});
    exec.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    exec.stop();

    EXPECT_GT(visit_count.load(), 4);
}

// ── Stop during dwell ─────────────────────────────────────────────────────────

TEST(ScanExecutor, StopMidScanTerminatesCleanly) {
    ScanExecutor exec("task-6", makeParams(3, true, 500),  // long dwells, repeat
        [](double, double) { return true; },
        {},
        [](const std::string&, bool) {});
    exec.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    exec.stop();
    EXPECT_FALSE(exec.isRunning());
}

// ── currentStep tracking ──────────────────────────────────────────────────────

TEST(ScanExecutor, CurrentStepReflectsProgress) {
    std::atomic<int> max_step{0};

    // 5 entries, short dwell, single pass
    ScanExecutor exec("task-7", makeParams(5, false, 20),
        [&](double, double) {
            int s = exec.currentStep();  // capture inside retune
            int expected = max_step.load();
            if (s > expected) max_step.store(s);
            return true;
        },
        {},
        [](const std::string&, bool) {});
    exec.start();
    waitFor([&] { return !exec.isRunning(); });

    EXPECT_GE(max_step.load(), 4);  // 0-indexed steps 0..4
}

// ── Edge cases ────────────────────────────────────────────────────────────────

TEST(ScanExecutor, SingleEntryNonRepeatFiresDone) {
    std::atomic<bool> done{false};
    std::atomic<bool> ok_val{false};

    ScanExecutor exec("task-single", makeParams(1, false, 1),
        [](double, double) { return true; },
        {},
        [&](const std::string&, bool ok) { ok_val = ok; done = true; });
    exec.start();

    EXPECT_TRUE(waitFor([&] { return done.load(); }));
    EXPECT_TRUE(ok_val.load());
}

TEST(ScanExecutor, StopBeforeStartIsHarmless) {
    ScanExecutor exec("task-prestop", makeParams(2, false, 10),
        [](double, double) { return true; },
        {},
        [](const std::string&, bool) {});
    EXPECT_NO_THROW(exec.stop());
    EXPECT_FALSE(exec.isRunning());
}

TEST(ScanExecutor, RetuneCalledExactlyOncePerStep) {
    std::atomic<int> retune_count{0};
    std::atomic<bool> done{false};

    // 4 entries, single pass
    ScanExecutor exec("task-retune-count", makeParams(4, false, 1),
        [&](double, double) { ++retune_count; return true; },
        {},
        [&](const std::string&, bool) { done = true; });
    exec.start();

    ASSERT_TRUE(waitFor([&] { return done.load(); }));
    EXPECT_EQ(retune_count.load(), 4);
}

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
