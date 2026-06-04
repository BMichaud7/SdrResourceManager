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
#include "Ddc.hpp"
#include <cmath>
#include <algorithm>

namespace sdr {

// Windowed-sinc low-pass FIR (Hamming window).
// cutoff_norm: normalised cutoff in (0, 0.5] relative to input sample rate.
static std::vector<float> designLpf(int n_taps, double cutoff_norm) {
    std::vector<float> taps(n_taps);
    int    M   = n_taps - 1;
    double sum = 0.0;
    for (int i = 0; i < n_taps; ++i) {
        double n      = i - M / 2.0;
        double sinc   = (n == 0.0) ? 2.0 * cutoff_norm
                                   : std::sin(2.0 * M_PI * cutoff_norm * n) / (M_PI * n);
        double window = 0.54 - 0.46 * std::cos(2.0 * M_PI * i / M);
        taps[i] = (float)(sinc * window);
        sum    += taps[i];
    }
    // Normalise to unity DC gain
    for (auto& t : taps) t /= (float)sum;
    return taps;
}

Ddc::Ddc(double offset_hz, double wideband_sr, int decim, int n_taps)
    : decim_(decim)
{
    // Multiply by exp(-j*2π*offset*t) to shift signal at +offset to DC.
    // phase_ accumulates positively; mixer computes exp(-j*phase_).
    phase_inc_ = (wideband_sr > 0.0) ? 2.0 * M_PI * offset_hz / wideband_sr : 0.0;

    // Cutoff slightly inside Nyquist of the output rate to avoid aliasing
    double cutoff_norm = (decim > 1) ? 0.45 / decim : 0.45;
    taps_ = designLpf(n_taps, cutoff_norm);

    // Complex (I,Q) ring buffer — initialised to zero
    hist_.assign((size_t)n_taps * 2, 0.0f);
}

int Ddc::process(const float* in, int n_in, float* out) {
    const int n_taps  = (int)taps_.size();
    int       out_cnt = 0;

    for (int i = 0; i < n_in; ++i) {
        const float xi = in[i * 2];
        const float xq = in[i * 2 + 1];

        // Mix: (I+jQ) * exp(-j*phase) = (I+jQ)*(cos(phase) - j*sin(phase))
        const float cp =  (float)std::cos(phase_);
        const float sp =  (float)std::sin(phase_);
        const float mi =  xi * cp + xq * sp;
        const float mq = -xi * sp + xq * cp;

        phase_ += phase_inc_;
        // Constrain to [-π, π] to avoid floating-point drift
        if      (phase_ >  M_PI) phase_ -= 2.0 * M_PI;
        else if (phase_ < -M_PI) phase_ += 2.0 * M_PI;

        // Write mixed sample into ring buffer
        hist_[hist_wr_ * 2]     = mi;
        hist_[hist_wr_ * 2 + 1] = mq;
        hist_wr_ = (hist_wr_ + 1) % n_taps;

        // Decimate: compute FIR output every decim_ input samples
        if (++input_count_ >= decim_) {
            input_count_ = 0;
            float oi = 0.0f, oq = 0.0f;
            for (int k = 0; k < n_taps; ++k) {
                // Ring-buffer traversal: tap[0] multiplies most-recent sample,
                // tap[k] multiplies sample k steps back.
                int pos = (hist_wr_ - 1 - k + n_taps * 2) % n_taps;
                oi += taps_[k] * hist_[pos * 2];
                oq += taps_[k] * hist_[pos * 2 + 1];
            }
            out[out_cnt * 2]     = oi;
            out[out_cnt * 2 + 1] = oq;
            ++out_cnt;
        }
    }
    return out_cnt;
}

} // namespace sdr
