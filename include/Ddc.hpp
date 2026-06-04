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
#pragma once
#include <vector>

namespace sdr {

// Digital Down-Converter: complex mix → real FIR low-pass filter → decimate.
// One instance per sub-band task. Not thread-safe internally — caller must
// ensure external synchronisation (IQStreamer holds subbands_mu_ during process).
class Ddc {
public:
    // offset_hz   – frequency to shift to baseband (positive shifts left)
    // wideband_sr – input sample rate in Hz
    // decim       – integer decimation ratio (>= 1; 1 = filter only, no decim)
    // n_taps      – FIR filter length (more taps = sharper roll-off, more latency)
    Ddc(double offset_hz, double wideband_sr, int decim, int n_taps = 64);

    // Process n_in CF32 interleaved samples from `in`, write decimated CF32 to
    // `out`.  `out` must have capacity >= (ceil(n_in / decim) + 1) * 2 floats.
    // Returns number of output *samples* written (each sample = 2 floats).
    int process(const float* in, int n_in, float* out);

    int  decimRatio() const { return decim_; }
    bool valid()      const { return decim_ > 0 && !taps_.empty(); }

private:
    double             phase_     = 0.0;
    double             phase_inc_;        // 2π·offset/sr radians per input sample
    int                decim_;
    int                input_count_ = 0;  // counts inputs toward next decimated output
    std::vector<float> taps_;             // real, symmetric LPF FIR coefficients
    std::vector<float> hist_;             // complex ring buffer (interleaved I,Q), length = n_taps*2
    int                hist_wr_   = 0;    // next write position in ring buffer
};

} // namespace sdr
