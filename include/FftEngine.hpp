#pragma once
#include "sdr/Types.hpp"
#include <vector>
#include <string>

namespace sdr {

class FftEngine {
public:
    FftEngine()=default;
    ~FftEngine();
    FftEngine(const FftEngine&)=delete;
    FftEngine& operator=(const FftEngine&)=delete;

    SnapshotResult compute(
        const std::vector<float>& samples_cf32,
        int fft_size, int n_averages,
        double cf_hz, double sr_sps, double bw_hz,
        const std::string& device_id);

private:
    void ensurePlan(int n);
    void hannWindow(void* in, int n);
    void fftshift(std::vector<double>& bins);

    int   plan_size_=0;
    void* plan_=nullptr;
    void* fin_=nullptr;
    void* fout_=nullptr;
};

} // namespace sdr
