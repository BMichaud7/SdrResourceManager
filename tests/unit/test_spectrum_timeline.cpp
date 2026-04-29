#include <gtest/gtest.h>
#include "SpectrumTimeline.hpp"

using namespace sdr;

static TimeFreqSlot makeSlot(const std::string& id,
                              int64_t t0, int64_t t1,
                              double cf, double sr,
                              double lo, double hi,
                              std::vector<int> rx = {0}) {
    TimeFreqSlot s;
    s.task_id        = id;
    s.t_start        = t0;  s.t_stop = t1;
    s.center_freq_hz = cf;  s.sample_rate_sps = sr;
    s.slice_lo_hz    = lo;  s.slice_hi_hz     = hi;
    s.rx_channels    = std::move(rx);
    return s;
}

static constexpr double CF  = 915e6;
static constexpr double SR  = 10e6;
static constexpr double BW  =  5e6;
static constexpr double G   = 200e3;
static constexpr int    MRX = 2;
static constexpr int    MTX = 2;

// ── Shared-LO (default) tests ─────────────────────────────────────────────────

TEST(SpectrumTimeline, EmptyTimelineFitsAnything) {
    SpectrumTimeline tl;
    auto r = tl.canFit(0, 60'000, CF, BW, SR, 1, 0, MRX, MTX, G);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.device_cf,   CF);
    EXPECT_EQ(r.device_rate, SR);
    EXPECT_GE(r.placed_lo, CF - SR/2.0);
    EXPECT_LE(r.placed_hi, CF + SR/2.0);
    EXPECT_EQ(r.avail_rx, (std::vector<int>{0}));
}

TEST(SpectrumTimeline, TwoNarrowTasksFitInSameWindow) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 60'000, CF, SR, CF - 2e6, CF + 2e6, {0}));

    auto r = tl.canFit(0, 60'000, CF, 1e6, SR, 1, 0, MRX, MTX, G);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.avail_rx, (std::vector<int>{1}));
}

TEST(SpectrumTimeline, RetuneConflictWhenCfDiffers) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 60'000, CF, SR, CF - 2e6, CF + 2e6, {0}));

    auto r = tl.canFit(0, 60'000, 2400e6, BW, SR, 1, 0, MRX, MTX, G);
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.reject_code, RejectCode::RETUNE_CONFLICT);
}

TEST(SpectrumTimeline, RetuneConflictWhenSrDiffers) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 60'000, CF, SR, CF - 2e6, CF + 2e6, {0}));

    auto r = tl.canFit(0, 60'000, CF, BW, 20e6, 1, 0, MRX, MTX, G);
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.reject_code, RejectCode::RETUNE_CONFLICT);
}

TEST(SpectrumTimeline, SpectrumConflictWhenWindowFull) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 60'000, CF, SR, CF - SR/2, CF + SR/2, {0}));

    auto r = tl.canFit(0, 60'000, CF, BW, SR, 1, 0, MRX, MTX, G);
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.reject_code, RejectCode::SPECTRUM_CONFLICT);
}

TEST(SpectrumTimeline, ChannelCountExceeded) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 60'000, CF, SR, CF - 2e6, CF + 2e6, {0, 1}));

    auto r = tl.canFit(0, 60'000, CF, 1e6, SR, 1, 0, MRX, MTX, G);
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.reject_code, RejectCode::CHANNEL_COUNT_EXCEEDED);
}

TEST(SpectrumTimeline, NonOverlappingWindowsAllowDifferentCf) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 30'000, CF, SR, CF - 2e6, CF + 2e6, {0}));

    auto r = tl.canFit(60'000, 90'000, 2400e6, BW, 20e6, 1, 0, MRX, MTX, G);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.device_cf, 2400e6);
}

TEST(SpectrumTimeline, RemoveClearsSlot) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 60'000, CF, SR, CF - SR/2, CF + SR/2, {0, 1}));

    ASSERT_FALSE(tl.canFit(0, 60'000, CF, BW, SR, 1, 0, MRX, MTX, G).ok);
    tl.remove("t1");
    EXPECT_TRUE(tl.canFit(0, 60'000, CF, BW, SR, 1, 0, MRX, MTX, G).ok);
}

TEST(SpectrumTimeline, AllocatedBwAndUsedRxAccounting) {
    SpectrumTimeline tl;
    EXPECT_EQ(tl.allocatedBw(1000), 0.0);
    EXPECT_EQ(tl.usedRx(1000), 0);

    tl.insert(makeSlot("t1", 0, 60'000, CF, SR, CF - 2e6, CF + 2e6, {0}));
    EXPECT_NEAR(tl.allocatedBw(1000), 4e6, 1.0);
    EXPECT_EQ(tl.usedRx(1000), 1);

    tl.insert(makeSlot("t2", 0, 60'000, CF, SR, CF + 2.2e6, CF + 3.2e6, {1}));
    EXPECT_EQ(tl.usedRx(1000), 2);

    EXPECT_EQ(tl.usedRx(90'000), 0);
}

TEST(SpectrumTimeline, ActiveWindowReportsCurrentCfAndSr) {
    SpectrumTimeline tl;
    EXPECT_FALSE(tl.activeWindow(0).has_value());

    tl.insert(makeSlot("t1", 1000, 5000, CF, SR, CF - 2e6, CF + 2e6, {0}));
    EXPECT_FALSE(tl.activeWindow(999).has_value());
    ASSERT_TRUE(tl.activeWindow(1000).has_value());
    EXPECT_EQ(tl.activeWindow(1000)->cf,   CF);
    EXPECT_EQ(tl.activeWindow(1000)->rate, SR);
    EXPECT_FALSE(tl.activeWindow(5000).has_value());
}

// ── shared_lo=false (independent-channel) tests ──────────────────────────────

TEST(SpectrumTimeline, IndepLo_DifferentCfsAcceptedOnSeparateChannels) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 60'000, CF, SR, CF - 2e6, CF + 2e6, {0}));

    auto r = tl.canFit(0, 60'000, 2400e6, BW, 20e6, 1, 0, MRX, MTX, G, /*shared_lo=*/false);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.avail_rx, (std::vector<int>{1}));
}

TEST(SpectrumTimeline, IndepLo_SameCfDifferentSrAccepted) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 60'000, CF, SR, CF - 2e6, CF + 2e6, {0}));

    auto r = tl.canFit(0, 60'000, CF, BW, 20e6, 1, 0, MRX, MTX, G, /*shared_lo=*/false);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.avail_rx, (std::vector<int>{1}));
}

TEST(SpectrumTimeline, IndepLo_ChannelExhaustionStillRejects) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 60'000,  CF,    SR, CF     - 2e6, CF     + 2e6, {0}));
    tl.insert(makeSlot("t2", 0, 60'000, 2400e6, SR, 2400e6 - 2e6, 2400e6 + 2e6, {1}));

    auto r = tl.canFit(0, 60'000, 433e6, BW, SR, 1, 0, MRX, MTX, G, /*shared_lo=*/false);
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.reject_code, RejectCode::CHANNEL_COUNT_EXCEEDED);
}

TEST(SpectrumTimeline, IndepLo_NonOverlappingTimesAllowReuse) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 30'000, CF, SR, CF - 2e6, CF + 2e6, {0, 1}));

    auto r = tl.canFit(60'000, 90'000, 2400e6, BW, 20e6, 2, 0, MRX, MTX, G, /*shared_lo=*/false);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.avail_rx, (std::vector<int>{0, 1}));
}
