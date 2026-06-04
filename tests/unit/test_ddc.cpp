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
#include "Ddc.hpp"
#include <cmath>
#include <vector>
#include <numeric>

using namespace sdr;

// Generate a complex sinusoid at freq_hz, sampled at sr_hz, length n_samples.
static std::vector<float> makeTone(double freq_hz, double sr_hz, int n_samples) {
    std::vector<float> buf(n_samples * 2);
    for (int i = 0; i < n_samples; ++i) {
        double phase = 2.0 * M_PI * freq_hz / sr_hz * i;
        buf[i * 2]     = (float)std::cos(phase);
        buf[i * 2 + 1] = (float)std::sin(phase);
    }
    return buf;
}

// RMS amplitude of CF32 signal.
static float rmsAmplitude(const float* buf, int n_samples) {
    float sum = 0.0f;
    for (int i = 0; i < n_samples; ++i)
        sum += buf[i*2]*buf[i*2] + buf[i*2+1]*buf[i*2+1];
    return std::sqrt(sum / n_samples);
}

TEST(Ddc, ValidFlag) {
    Ddc d(0.0, 1e6, 4);
    EXPECT_TRUE(d.valid());
    EXPECT_EQ(d.decimRatio(), 4);

    Ddc d2(0.0, 1e6, 0);
    EXPECT_FALSE(d2.valid());
}

TEST(Ddc, DecimationRatio) {
    const double sr = 2e6;
    const int decim = 4;
    const int n_in  = 128;
    Ddc d(0.0, sr, decim);

    auto in = makeTone(0.0, sr, n_in);
    std::vector<float> out(n_in * 2, 0.0f);
    int n_out = d.process(in.data(), n_in, out.data());

    // Each decim input samples produces one output sample
    EXPECT_EQ(n_out, n_in / decim);
}

TEST(Ddc, DecimationRatioPartialBlock) {
    // Process a block not evenly divisible by decim
    const double sr    = 1e6;
    const int    decim = 3;
    const int    n_in  = 100;
    Ddc d(0.0, sr, decim);

    auto in = makeTone(0.0, sr, n_in);
    std::vector<float> out((n_in / decim + 2) * 2, 0.0f);
    int n_out = d.process(in.data(), n_in, out.data());

    EXPECT_EQ(n_out, n_in / decim);
}

TEST(Ddc, DcInputPassthroughOffset0) {
    // DC input (I=1, Q=0) with zero offset should pass through the FIR
    // and appear at DC in the output (non-zero after filter settles).
    const double sr    = 1e6;
    const int    decim = 2;
    const int    n_in  = 512;
    Ddc d(0.0, sr, decim);

    std::vector<float> in(n_in * 2, 0.0f);
    for (int i = 0; i < n_in; ++i) in[i * 2] = 1.0f; // I=1, Q=0

    std::vector<float> out((n_in / decim + 2) * 2, 0.0f);
    int n_out = d.process(in.data(), n_in, out.data());

    ASSERT_GT(n_out, 10);
    // Skip first n_taps/decim samples for filter to settle (default 64 taps, decim=2 → 32)
    int settle = 32 + 4;
    float amp = rmsAmplitude(out.data() + settle * 2, n_out - settle);
    EXPECT_GT(amp, 0.5f) << "DC signal should survive filter; got amplitude " << amp;
}

TEST(Ddc, ToneDownconvertedToDc) {
    // A tone at offset_hz should appear as ~DC after DDC with that offset.
    const double sr        = 2e6;
    const double offset_hz = 200e3;
    const int    decim     = 4;
    const int    n_in      = 1024;
    Ddc d(offset_hz, sr, decim);

    auto in = makeTone(offset_hz, sr, n_in);
    std::vector<float> out((n_in / decim + 4) * 2, 0.0f);
    int n_out = d.process(in.data(), n_in, out.data());

    ASSERT_GT(n_out, 20);
    // After filter settles, the output should have roughly constant amplitude
    // (a DC complex phasor). Check that amplitude is non-zero and stable.
    int settle = 16 + 2; // 64 taps / 4 decim = 16
    float amp_first = rmsAmplitude(out.data() + settle * 2, 4);
    float amp_last  = rmsAmplitude(out.data() + (n_out - 8) * 2, 4);

    EXPECT_GT(amp_first, 0.3f) << "Tone not present in output after downconversion";
    // Amplitude should be approximately stable (within 20%)
    EXPECT_NEAR(amp_first, amp_last, amp_first * 0.2f)
        << "Amplitude varies — DDC not producing a steady DC phasor";
}

TEST(Ddc, OutOfBandToneRejected) {
    // A tone far from the DDC center (at wideband_sr/4) should be attenuated.
    const double sr        = 2e6;
    const double offset_hz = 0.0;        // DDC at DC
    const double tone_hz   = sr / 4.0;   // far from passband (relative to output rate)
    const int    decim     = 8;           // output rate = 250 kHz; tone at 500 kHz alias
    const int    n_in      = 2048;
    Ddc d(offset_hz, sr, decim);

    auto in = makeTone(tone_hz, sr, n_in);
    std::vector<float> out((n_in / decim + 4) * 2, 0.0f);
    int n_out = d.process(in.data(), n_in, out.data());

    ASSERT_GT(n_out, 20);
    int settle = 64 / decim + 2;
    float amp = rmsAmplitude(out.data() + settle * 2, n_out - settle);
    // The LPF should attenuate the out-of-band tone significantly
    EXPECT_LT(amp, 0.2f) << "Out-of-band tone not sufficiently rejected; amp=" << amp;
}

TEST(Ddc, MultipleBlocksProduceConsistentOutput) {
    // Processing one big block should produce the same total sample count as
    // two half-sized blocks processed sequentially (DDC maintains state correctly).
    const double sr    = 1e6;
    const int    decim = 2;
    const int    half  = 64;
    auto in_buf = makeTone(50e3, sr, half * 2);

    std::vector<float> out_big((half * 2 / decim + 4) * 2, 0.0f);
    Ddc d_big(50e3, sr, decim);
    int n_big = d_big.process(in_buf.data(), half * 2, out_big.data());

    std::vector<float> out_split((half * 2 / decim + 4) * 2, 0.0f);
    Ddc d_split(50e3, sr, decim);
    int n1 = d_split.process(in_buf.data(), half, out_split.data());
    int n2 = d_split.process(in_buf.data() + half * 2, half,
                              out_split.data() + n1 * 2);

    EXPECT_EQ(n_big, n1 + n2);
}
