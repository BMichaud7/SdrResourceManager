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
        // Slice must clear the guard zone on whichever side it was placed
        bool right_side = r.placed_lo >= CF + 2e6 + G;
        bool left_side  = r.placed_hi <= CF - 2e6 - G;
        EXPECT_TRUE(right_side || left_side)
            << "placed slice overlaps t1 guard zone";
    }
    // Either fits on the other side or rejects with SPECTRUM_CONFLICT — both valid.
    if (!r.ok) {
        EXPECT_EQ(r.reject_code, RejectCode::SPECTRUM_CONFLICT);
    }
}

// ── canCombine tests ──────────────────────────────────────────────────────────

TEST(SpectrumTimeline, CanCombineReturnsFalseOnEmptyTimeline) {
    SpectrumTimeline tl;
    auto cr = tl.canCombine(0, 60'000, CF, 100e3, 1, MRX, G, SR);
    EXPECT_FALSE(cr.ok);
}

TEST(SpectrumTimeline, CanCombineTwoNearbySlicesFitsWithinMaxSr) {
    SpectrumTimeline tl;
    // Task 1 at 100 MHz, BW 100 kHz
    tl.insert(makeSlot("t1", 0, 60'000, 100e6, SR, 99.95e6, 100.05e6, {0}));

    // Task 2 requests 101 MHz, BW 100 kHz. Combined span = 1.1 MHz + 2*guard
    auto cr = tl.canCombine(0, 60'000, 101e6, 100e3, 1, MRX, G, SR);
    ASSERT_TRUE(cr.ok);
    EXPECT_NEAR(cr.combined_cf, 100.5e6, 1.0);
    // combined_sr = (101.05M - 99.95M) + 2*200k = 1.5 MHz
    EXPECT_NEAR(cr.combined_sr, 1.5e6, 1.0);
    EXPECT_NEAR(cr.new_slice_lo, 100.95e6, 1.0);
    EXPECT_NEAR(cr.new_slice_hi, 101.05e6, 1.0);
    ASSERT_FALSE(cr.avail_rx.empty());
    EXPECT_EQ(cr.avail_rx[0], 1); // ch 0 already used by t1
}

TEST(SpectrumTimeline, CanCombineRejectsWhenCombinedSrExceedsMax) {
    SpectrumTimeline tl;
    // Existing task at 100 MHz. New request at 110 MHz, BW 5 MHz.
    // Combined span = (115M - 95M) = 20 MHz + guard → exceeds sr_max=10 MHz
    tl.insert(makeSlot("t1", 0, 60'000, 100e6, SR, 95e6, 105e6, {0}));

    auto cr = tl.canCombine(0, 60'000, 110e6, 5e6, 1, MRX, G, SR); // sr_max=10e6
    EXPECT_FALSE(cr.ok);
}

TEST(SpectrumTimeline, CanCombineRejectsWhenNoRxChannelsFree) {
    SpectrumTimeline tl;
    // Both RX channels already in use
    tl.insert(makeSlot("t1", 0, 60'000, 100e6, SR, 99.95e6, 100.05e6, {0, 1}));

    auto cr = tl.canCombine(0, 60'000, 101e6, 100e3, 1, MRX, G, SR);
    EXPECT_FALSE(cr.ok);
}

TEST(SpectrumTimeline, UpdateDeviceTuneUpdatesAllSlots) {
    SpectrumTimeline tl;
    tl.insert(makeSlot("t1", 0, 60'000, 100e6, SR, 99.95e6, 100.05e6, {0}));
    tl.insert(makeSlot("t2", 0, 60'000, 100e6, SR, 100.95e6, 101.05e6, {1}));

    tl.updateDeviceTune(100.5e6, 1.5e6);

    auto slots = tl.slotsOverlapping(0, 60'000);
    ASSERT_EQ(slots.size(), 2u);
    for (auto& s : slots) {
        EXPECT_DOUBLE_EQ(s.center_freq_hz,  100.5e6);
        EXPECT_DOUBLE_EQ(s.sample_rate_sps,   1.5e6);
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

// ── preferred_channel tests ───────────────────────────────────────────────────

TEST(SpectrumTimeline, PreferredChannel_FreeChannelAssigned) {
    // Device has channels 0 and 1; request preferred_channel=1 on empty timeline.
    SpectrumTimeline tl;
    auto r = tl.canFit(0, 60'000, CF, BW, SR, 1, 0, MRX, MTX, G,
                       /*shared_lo=*/true, /*preferred_channel=*/1);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.avail_rx, (std::vector<int>{1}));
}

TEST(SpectrumTimeline, PreferredChannel_SharedLo_ReuseOccupiedChannel) {
    // Channel 0 is in use at same CF/SR.  preferred_channel=0 should still succeed
    // so activateTask can subscribe to the existing stream (fan-out / DDC).
    SpectrumTimeline tl;
    tl.insert(makeSlot("existing", 0, TIME_INFINITE, CF, SR,
                       CF - BW/2, CF + BW/2, {0}));
    auto r = tl.canFit(0, TIME_INFINITE, CF, BW/4, SR, 1, 0, MRX, MTX, G,
                       true, /*preferred_channel=*/0);
    ASSERT_TRUE(r.ok) << r.reject_reason;
    EXPECT_EQ(r.avail_rx, (std::vector<int>{0}));
}

TEST(SpectrumTimeline, PreferredChannel_IndepLo_OccupiedChannelRejected) {
    // Independent-LO device: occupied preferred channel must fail.
    SpectrumTimeline tl;
    tl.insert(makeSlot("existing", 0, TIME_INFINITE, CF, SR,
                       CF - BW/2, CF + BW/2, {0}));
    auto r = tl.canFit(0, TIME_INFINITE, CF, BW, SR, 1, 0, MRX, MTX, G,
                       /*shared_lo=*/false, /*preferred_channel=*/0);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reject_code, RejectCode::CHANNEL_COUNT_EXCEEDED);
}

TEST(SpectrumTimeline, PreferredChannel_OutOfRange_Rejected) {
    SpectrumTimeline tl;
    auto r = tl.canFit(0, 60'000, CF, BW, SR, 1, 0, MRX, MTX, G,
                       true, /*preferred_channel=*/99);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reject_code, RejectCode::CHANNEL_COUNT_EXCEEDED);
}

TEST(SpectrumTimeline, PreferredChannel_SharedLo_SameCf_SecondSlice) {
    // Channel 0 occupied at same CF; a second task at same CF with preferred_channel=0
    // and a different (adjacent) BW should also be admitted by canFit (shared reuse).
    SpectrumTimeline tl;
    tl.insert(makeSlot("existing", 0, TIME_INFINITE, CF, SR,
                       CF - BW/2, CF + BW/2, {0}));
    // Second slice at same CF, smaller BW — canFit allows reuse of ch0
    auto r = tl.canFit(0, TIME_INFINITE, CF, BW/4, SR, 1, 0, MRX, MTX, G,
                       /*shared_lo=*/true, /*preferred_channel=*/0);
    ASSERT_TRUE(r.ok) << r.reject_reason;
    EXPECT_EQ(r.avail_rx, (std::vector<int>{0}));
    // Device CF/SR must be unchanged
    EXPECT_DOUBLE_EQ(r.device_cf,   CF);
    EXPECT_DOUBLE_EQ(r.device_rate, SR);
}
