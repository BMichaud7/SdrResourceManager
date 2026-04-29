#pragma once
#include <complex>
#include <vector>
#include <cstdint>
#include <fftw3.h>

namespace acq {

// Processes one buffer of CF32 samples and returns detected signals.
// Thread-compatible: one instance per thread / channel.
class FftProcessor {
public:
    struct Signal {
        int   start_bin;
        int   end_bin;   // inclusive
        float peak_db;
        float mean_db;
    };

    explicit FftProcessor(int fft_size);
    ~FftProcessor();

    FftProcessor(const FftProcessor&)            = delete;
    FftProcessor& operator=(const FftProcessor&) = delete;

    // Detect signals in one dwell's worth of samples.
    // usable_fraction: fraction of FFT bins to examine (discard roll-off edges).
    // threshold_db: detection level above estimated per-dwell noise floor.
    // min_bw_hz, sample_rate: used to compute minimum bin width filter.
    std::vector<Signal> detect(
        const std::complex<float>* samples,
        float   threshold_db,
        float   usable_fraction,
        double  sample_rate,
        uint32_t min_signal_bw_hz);

    // Convert a post-fftshift bin index to an absolute frequency in Hz.
    uint64_t binToHz(int bin, double sample_rate, uint64_t center_hz) const;

    int fft_size() const { return fft_size_; }

private:
    int fft_size_;
    fftwf_complex* in_{nullptr};
    fftwf_complex* out_{nullptr};
    fftwf_plan     plan_{nullptr};
    std::vector<float> window_;    // Hann coefficients
    std::vector<float> power_db_;  // reused scratch buffer

    void computePower(const std::complex<float>* samples);
    float estimateNoise(int start_bin, int end_bin);
};

} // namespace acq
