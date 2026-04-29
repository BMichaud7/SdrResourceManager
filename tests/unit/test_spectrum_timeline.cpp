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

// ── Time boundary edge cases ──────────────────────────────────────────────────

TEST(SpectrumTimeline, AdjacentWindowsDoNotConflict) {
    // Slot ends at t=30000; new slot starts at t=30000 — no overlap.
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 30'000, CF, SR, CF - 2e6, CF + 2e6, {0}));

    auto r = tl.canFit(30'000, 60'000, 2400e6, BW, 20e6, 1, 0, MRX, MTX, G);
    EXPECT_TRUE(r.ok) << "Touching (non-overlapping) time windows should not conflict";
}

TEST(SpectrumTimeline, OneMillisecondOverlapCausesConflict) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 30'001, CF, SR, CF - 2e6, CF + 2e6, {0}));

    // Starts 1 ms before t1 ends: [30000, 60000) overlaps [0, 30001) by 1 ms.
    auto r = tl.canFit(30'000, 60'000, 2400e6, BW, 20e6, 1, 0, MRX, MTX, G);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reject_code, RejectCode::RETUNE_CONFLICT);
}

TEST(SpectrumTimeline, InfiniteStopTimeBlocksAllFutureTasks) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, TIME_INFINITE, CF, SR, CF - 2e6, CF + 2e6, {0, 1}));

    // Any future time window overlaps with an infinite slot.
    auto r = tl.canFit(1'000'000, 2'000'000, CF, BW, SR, 1, 0, MRX, MTX, G);
    EXPECT_FALSE(r.ok);
}

TEST(SpectrumTimeline, InfiniteStopTimeSlotCanBeRemoved) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, TIME_INFINITE, CF, SR, CF - 2e6, CF + 2e6, {0, 1}));
    ASSERT_FALSE(tl.canFit(1000, 2000, CF, BW, SR, 1, 0, MRX, MTX, G).ok);

    tl.remove("t1");
    EXPECT_TRUE(tl.canFit(1000, 2000, CF, BW, SR, 1, 0, MRX, MTX, G).ok);
}

TEST(SpectrumTimeline, RemoveNonexistentIdIsSafe) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 60'000, CF, SR, CF - 2e6, CF + 2e6, {0}));

    EXPECT_NO_THROW(tl.remove("does-not-exist"));
    // Existing slot is unaffected.
    EXPECT_EQ(tl.slotCount(), 1);
}

// ── slotCount and slotsOverlapping ────────────────────────────────────────────

TEST(SpectrumTimeline, SlotCountTracksInsertAndRemove) {
    SpectrumTimeline tl;
    EXPECT_EQ(tl.slotCount(), 0);

    tl.insert(makeSlot("t1", 0, 60'000, CF, SR, CF - 2e6, CF + 2e6, {0}));
    EXPECT_EQ(tl.slotCount(), 1);

    tl.insert(makeSlot("t2", 0, 60'000, CF, SR, CF + 2.5e6, CF + 3.5e6, {1}));
    EXPECT_EQ(tl.slotCount(), 2);

    tl.remove("t1");
    EXPECT_EQ(tl.slotCount(), 1);

    tl.remove("t2");
    EXPECT_EQ(tl.slotCount(), 0);
}

TEST(SpectrumTimeline, SlotsOverlappingReturnsOnlyOverlapping) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("early", 0,      30'000,  CF, SR, CF - 2e6, CF + 2e6, {0}));
    tl.insert(makeSlot("mid",   15'000, 45'000,  CF, SR, CF + 2.5e6, CF + 3.5e6, {1}));
    tl.insert(makeSlot("late",  40'000, 70'000,  CF, SR, CF - 2e6, CF + 2e6, {0}));

    // Query [14000, 16000) — overlaps "early" and "mid", not "late"
    auto overlapping = tl.slotsOverlapping(14'000, 16'000);
    EXPECT_EQ(overlapping.size(), 2u);

    // Query [60000, 80000) — only "late"
    overlapping = tl.slotsOverlapping(60'000, 80'000);
    ASSERT_EQ(overlapping.size(), 1u);
    EXPECT_EQ(overlapping[0].task_id, "late");

    // Query [100000, 200000) — none
    overlapping = tl.slotsOverlapping(100'000, 200'000);
    EXPECT_TRUE(overlapping.empty());
}

// ── Guard band ────────────────────────────────────────────────────────────────

TEST(SpectrumTimeline, GuardBandPreventsImmediatelyAdjacentSlice) {
    SpectrumTimeline tl;
    // First slot occupies [CF-2M, CF+2M].
    tl.insert(makeSlot("t1", 0, 60'000, CF, SR, CF - 2e6, CF + 2e6, {0}));

    // New slice starts exactly where t1 ends (CF+2M) — within guard band G=200kHz.
    auto r = tl.canFit(0, 60'000, CF, 1e6, SR, 1, 0, MRX, MTX, G);
    // The canFit must place the new slice with at least G Hz gap; if no room, reject.
    // With SR=10M and used [CF-2M..CF+2M] plus guard, there's room on the other side.
    if (r.ok) {
        EXPECT_GE(r.placed_lo, CF + 2e6 + G);
    }
    // Either fits on the other side or rejects with SPECTRUM_CONFLICT — both valid.
    if (!r.ok) {
        EXPECT_EQ(r.reject_code, RejectCode::SPECTRUM_CONFLICT);
    }
}

// ── allocatedBw edge cases ────────────────────────────────────────────────────

TEST(SpectrumTimeline, AllocatedBwIsZeroWhenNoActiveSlots) {
    SpectrumTimeline tl;
    EXPECT_DOUBLE_EQ(tl.allocatedBw(999'999), 0.0);
}

TEST(SpectrumTimeline, AllocatedBwExcludesExpiredSlots) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 1000, CF, SR, CF - 2e6, CF + 2e6, {0}));
    EXPECT_NEAR(tl.allocatedBw(500),  4e6, 1.0);
    EXPECT_DOUBLE_EQ(tl.allocatedBw(1000), 0.0);  // slot ends at t=1000 (exclusive)
}
