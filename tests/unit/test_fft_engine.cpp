#include <gtest/gtest.h>
#include "FftEngine.hpp"
#include <cmath>
#include <vector>

using namespace sdr;

// Generate n_avg * fft_size CF32 samples of a complex tone at normalized
// frequency f_norm ∈ [0,1) (maps to 0..sr). Each call fills one FFT frame.
static std::vector<float> makeTone(int fft_size, int n_avg, double f_norm) {
    std::vector<float> s((size_t)fft_size * n_avg * 2);
    for (int a = 0; a < n_avg; ++a)
        for (int i = 0; i < fft_size; ++i) {
            double phi = 2.0 * M_PI * f_norm * i;
            s[((size_t)a * fft_size + i) * 2 + 0] = (float)std::cos(phi);
            s[((size_t)a * fft_size + i) * 2 + 1] = (float)std::sin(phi);
        }
    return s;
}

// ── Basic correctness ─────────────────────────────────────────────────────────

TEST(FftEngine, DcTonePeakAtCenter) {
    // DC tone (f_norm=0): after fftshift the peak lands at bin fft_size/2.
    FftEngine eng;
    const int fft_size = 256;
    auto s = makeTone(fft_size, 1, 0.0);

    auto r = eng.compute(s, fft_size, 1, 915e6, 10e6, 10e6, "dev-0");
    ASSERT_TRUE(r.success);
    ASSERT_EQ((int)r.power_bins.size(), fft_size);

    int center = fft_size / 2;
    EXPECT_GT(r.power_bins[center], r.power_bins[center - 8] + 15.0)
        << "Center bin should dominate its neighbors by >15 dB";
    EXPECT_GT(r.power_bins[center], r.power_bins[center + 8] + 15.0);
}

TEST(FftEngine, QuarterBandTonePeakAtCorrectBin) {
    // f_norm=0.25 → bin fft_size/4 before fftshift → bin 3*fft_size/4 after.
    FftEngine eng;
    const int fft_size = 256;
    auto s = makeTone(fft_size, 1, 0.25);

    auto r = eng.compute(s, fft_size, 1, 915e6, 10e6, 10e6, "dev-0");
    ASSERT_TRUE(r.success);

    // Find peak bin
    int peak = 0;
    for (int i = 1; i < fft_size; ++i)
        if (r.power_bins[i] > r.power_bins[peak]) peak = i;

    int expected = 3 * fft_size / 4;
    EXPECT_NEAR(peak, expected, 2) << "Peak should be near bin " << expected;
}

TEST(FftEngine, InsufficientSamplesReturnsFail) {
    FftEngine eng;
    std::vector<float> tiny(16);  // only 8 complex samples; need 256
    auto r = eng.compute(tiny, 256, 1, 915e6, 10e6, 10e6, "dev-0");
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.error_msg.empty());
}

// ── Metadata ─────────────────────────────────────────────────────────────────

TEST(FftEngine, MetadataFieldsPopulated) {
    FftEngine eng;
    const int fft_size = 512, n_avg = 4;
    std::vector<float> s((size_t)fft_size * n_avg * 2, 0.0f);

    auto r = eng.compute(s, fft_size, n_avg, 2400e6, 20e6, 10e6, "dev-1");
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.device_id,         "dev-1");
    EXPECT_DOUBLE_EQ(r.center_freq_hz,    2400e6);
    EXPECT_DOUBLE_EQ(r.sample_rate_sps,    20e6);
    EXPECT_EQ(r.fft_size,          fft_size);
    EXPECT_EQ(r.n_averages,        n_avg);
    EXPECT_NEAR(r.freq_resolution_hz, 20e6 / 512.0, 1.0);
    EXPECT_NEAR(r.freq_axis_start_hz, 2400e6 - 10e6, 1.0);
    EXPECT_EQ((int)r.power_bins.size(), fft_size);
}

// ── Averaging ─────────────────────────────────────────────────────────────────

TEST(FftEngine, AveragingWithMultipleFramesSucceeds) {
    FftEngine eng;
    const int fft_size = 256, n_avg = 8;
    // Use a tone that exercises all averaging frames
    auto s = makeTone(fft_size, n_avg, 0.125);

    auto r = eng.compute(s, fft_size, n_avg, 915e6, 10e6, 10e6, "dev-0");
    ASSERT_TRUE(r.success);
    EXPECT_EQ((int)r.power_bins.size(), fft_size);

    // With coherent averaging of a tone, peak should be prominent
    int peak = 0;
    for (int i = 1; i < fft_size; ++i)
        if (r.power_bins[i] > r.power_bins[peak]) peak = i;
    EXPECT_GT(r.power_bins[peak], r.power_bins[(peak + fft_size/2) % fft_size] + 10.0);
}

// ── Plan lifecycle ────────────────────────────────────────────────────────────

TEST(FftEngine, PlanReusedProducesSameResult) {
    FftEngine eng;
    const int fft_size = 256;
    auto s = makeTone(fft_size, 1, 0.0);

    auto r1 = eng.compute(s, fft_size, 1, 915e6, 10e6, 10e6, "dev-0");
    auto r2 = eng.compute(s, fft_size, 1, 915e6, 10e6, 10e6, "dev-0");
    ASSERT_TRUE(r1.success);
    ASSERT_TRUE(r2.success);
    ASSERT_EQ(r1.power_bins.size(), r2.power_bins.size());
    for (size_t i = 0; i < r1.power_bins.size(); ++i)
        EXPECT_NEAR(r1.power_bins[i], r2.power_bins[i], 1e-6);
}

TEST(FftEngine, PlanReallocatedForDifferentFftSize) {
    FftEngine eng;
    std::vector<float> s1((size_t)256 * 2, 0.5f);
    std::vector<float> s2((size_t)512 * 2, 0.5f);

    auto r1 = eng.compute(s1, 256, 1, 915e6, 10e6, 10e6, "dev-0");
    auto r2 = eng.compute(s2, 512, 1, 915e6, 10e6, 10e6, "dev-0");
    ASSERT_TRUE(r1.success);
    ASSERT_TRUE(r2.success);
    EXPECT_EQ(r1.fft_size, 256);
    EXPECT_EQ(r2.fft_size, 512);
    EXPECT_EQ((int)r1.power_bins.size(), 256);
    EXPECT_EQ((int)r2.power_bins.size(), 512);
}

// ── Edge cases ────────────────────────────────────────────────────────────────

TEST(FftEngine, AllZeroInputSucceeds) {
    // Zero samples must not crash; output bins should be defined (very negative dB).
    FftEngine eng;
    const int fft_size = 256;
    std::vector<float> zeros((size_t)fft_size * 2, 0.0f);
    auto r = eng.compute(zeros, fft_size, 1, 915e6, 10e6, 10e6, "dev-0");
    ASSERT_TRUE(r.success);
    EXPECT_EQ((int)r.power_bins.size(), fft_size);
}

TEST(FftEngine, FreqAxisSymmetricAroundCenter) {
    // When sample_rate == 2 * bandwidth_hz the axis runs center±bw.
    FftEngine eng;
    const int fft_size = 256;
    const double center = 1000e6, sr = 20e6, bw = 10e6;
    std::vector<float> s((size_t)fft_size * 2, 0.0f);
    auto r = eng.compute(s, fft_size, 1, center, sr, bw, "dev-0");
    ASSERT_TRUE(r.success);

    EXPECT_NEAR(r.freq_axis_start_hz, center - bw, 1.0);
    double end = r.freq_axis_start_hz + fft_size * r.freq_resolution_hz;
    EXPECT_NEAR(end, center + bw, 1.0);
}

TEST(FftEngine, DeviceIdCarriedThrough) {
    FftEngine eng;
    std::vector<float> s((size_t)128 * 2, 0.0f);
    auto r = eng.compute(s, 128, 1, 433e6, 5e6, 5e6, "radio-7");
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.device_id, "radio-7");
}

TEST(FftEngine, ZeroAveragesReturnsFail) {
    FftEngine eng;
    const int fft_size = 256;
    std::vector<float> s((size_t)fft_size * 2, 0.5f);
    // n_avg = 0 means no frames to process — should fail gracefully.
    auto r = eng.compute(s, fft_size, 0, 915e6, 10e6, 10e6, "dev-0");
    EXPECT_FALSE(r.success);
}
