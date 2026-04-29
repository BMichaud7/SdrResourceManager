#include "FftProcessor.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace acq {

FftProcessor::FftProcessor(int fft_size) : fft_size_(fft_size) {
    if (fft_size < 64 || (fft_size & (fft_size - 1)) != 0)
        throw std::invalid_argument("fft_size must be a power of 2 >= 64");

    in_  = fftwf_alloc_complex((size_t)fft_size);
    out_ = fftwf_alloc_complex((size_t)fft_size);
    plan_ = fftwf_plan_dft_1d(fft_size,
        (fftwf_complex*)in_, (fftwf_complex*)out_,
        FFTW_FORWARD, FFTW_ESTIMATE);

    window_.resize((size_t)fft_size);
    for (int i = 0; i < fft_size; ++i)
        window_[i] = 0.5f * (1.f - std::cos(2.f * M_PIf * i / (fft_size - 1)));

    power_db_.resize((size_t)fft_size);
}

FftProcessor::~FftProcessor() {
    if (plan_) fftwf_destroy_plan(plan_);
    if (in_)   fftwf_free(in_);
    if (out_)  fftwf_free(out_);
}

void FftProcessor::computePower(const std::complex<float>* samples) {
    // Apply Hann window and copy into FFTW input
    auto* c = reinterpret_cast<std::complex<float>*>(in_);
    for (int i = 0; i < fft_size_; ++i)
        c[i] = samples[i] * window_[i];

    fftwf_execute(plan_);

    // Power in dBFS, fftshift so DC is at centre
    const float norm = 1.f / (float)(fft_size_ * fft_size_);
    const int   half = fft_size_ / 2;
    auto* o = reinterpret_cast<std::complex<float>*>(out_);
    for (int k = 0; k < fft_size_; ++k) {
        int shifted = (k + half) % fft_size_;
        float re = o[shifted].real(), im = o[shifted].imag();
        float p  = (re * re + im * im) * norm;
        power_db_[k] = 10.f * std::log10(p + 1e-30f);
    }
}

float FftProcessor::estimateNoise(int start_bin, int end_bin) {
    // Median of the usable bins — robust to signals (minority of bins)
    std::vector<float> tmp(power_db_.begin() + start_bin,
                           power_db_.begin() + end_bin + 1);
    size_t mid = tmp.size() / 2;
    std::nth_element(tmp.begin(), tmp.begin() + (long)mid, tmp.end());
    return tmp[mid];
}

std::vector<FftProcessor::Signal> FftProcessor::detect(
    const std::complex<float>* samples,
    float   threshold_db,
    float   usable_fraction,
    double  sample_rate,
    uint32_t min_signal_bw_hz)
{
    computePower(samples);

    // Usable bin range (discard roll-off edges)
    int margin     = (int)(fft_size_ * (1.0 - usable_fraction) / 2.0);
    int start_bin  = margin;
    int end_bin    = fft_size_ - 1 - margin;

    float noise_floor = estimateNoise(start_bin, end_bin);
    float threshold   = noise_floor + threshold_db;

    double bin_width_hz = sample_rate / fft_size_;
    int    min_bins     = std::max(1, (int)std::ceil(min_signal_bw_hz / bin_width_hz));

    // Find contiguous runs of bins above threshold
    std::vector<Signal> signals;
    int run_start = -1;
    float peak = -1e30f, sum = 0.f;
    int count = 0;

    auto flush = [&](int end) {
        if (run_start < 0) return;
        int width = end - run_start + 1;
        if (width >= min_bins)
            signals.push_back({run_start, end, peak, sum / (float)count});
        run_start = -1;
        peak = -1e30f; sum = 0.f; count = 0;
    };

    for (int k = start_bin; k <= end_bin; ++k) {
        if (power_db_[k] >= threshold) {
            if (run_start < 0) run_start = k;
            peak = std::max(peak, power_db_[k]);
            sum += power_db_[k];
            ++count;
        } else {
            flush(k - 1);
        }
    }
    flush(end_bin);

    return signals;
}

uint64_t FftProcessor::binToHz(int bin, double sample_rate, uint64_t center_hz) const {
    // After fftshift: bin 0 = center_hz - sample_rate/2
    double offset = (bin - fft_size_ / 2) * (sample_rate / fft_size_);
    return (uint64_t)((double)center_hz + offset);
}

} // namespace acq
