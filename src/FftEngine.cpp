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
#include "FftEngine.hpp"
#include <fftw3.h>
#include <cmath>
#include <complex>
#include <spdlog/spdlog.h>

namespace sdr {

FftEngine::~FftEngine() {
    if (plan_) fftwf_destroy_plan((fftwf_plan)plan_);
    if (fin_)  fftwf_free(fin_);
    if (fout_) fftwf_free(fout_);
}

void FftEngine::ensurePlan(int n) {
    if (plan_size_==n) return;
    if (plan_) fftwf_destroy_plan((fftwf_plan)plan_);
    if (fin_)  fftwf_free(fin_);
    if (fout_) fftwf_free(fout_);
    fin_  = fftwf_alloc_complex((size_t)n);
    fout_ = fftwf_alloc_complex((size_t)n);
    plan_ = fftwf_plan_dft_1d(n,(fftwf_complex*)fin_,(fftwf_complex*)fout_,FFTW_FORWARD,FFTW_ESTIMATE);
    plan_size_=n;
}

void FftEngine::hannWindow(void* in, int n) {
    auto* c = reinterpret_cast<std::complex<float>*>(in);
    if (n == 1) { c[0] *= 0.0f; return; }  // single sample: Hann(0/(n-1)) = 0
    for (int i=0;i<n;++i)
        c[i] *= (float)(0.5*(1.0-std::cos(2.0*M_PI*i/(n-1))));
}

void FftEngine::fftshift(std::vector<double>& b) {
    int h=(int)b.size()/2;
    std::vector<double> tmp(b.begin()+h,b.end());
    tmp.insert(tmp.end(),b.begin(),b.begin()+h);
    b=std::move(tmp);
}

SnapshotResult FftEngine::compute(
    const std::vector<float>& s, int fft_size, int n_avg,
    double cf_hz, double sr_sps, double bw_hz, const std::string& dev_id)
{
    SnapshotResult r;
    r.device_id=dev_id; r.center_freq_hz=cf_hz;
    r.bandwidth_hz=bw_hz; r.sample_rate_sps=sr_sps;
    r.fft_size=fft_size; r.n_averages=n_avg;

    if (fft_size <= 0) {
        r.error_msg="fft_size must be > 0"; return r;
    }
    if (n_avg <= 0) {
        r.error_msg="n_averages must be > 0"; return r;
    }
    // Use int64 to avoid overflow; reject unreasonably large requests.
    int64_t total64 = (int64_t)fft_size * n_avg * 2;
    if (total64 > (int64_t)s.size()) {
        r.error_msg="Insufficient samples"; return r;
    }

    r.freq_resolution_hz=sr_sps/fft_size;
    r.freq_axis_start_hz=cf_hz-sr_sps/2.0;
    r.freq_axis_step_hz=r.freq_resolution_hz;
    try {
        ensurePlan(fft_size);
        auto* in  = (fftwf_complex*)fin_;
        auto* out = (fftwf_complex*)fout_;
        std::vector<double> pwr((size_t)fft_size,0.0);
        double norm=(double)fft_size*(double)fft_size;

        for (int a=0;a<n_avg;++a) {
            const float* src=s.data()+a*fft_size*2;
            auto* cbuf=reinterpret_cast<std::complex<float>*>(in);
            for (int i=0;i<fft_size;++i) cbuf[i]={src[i*2],src[i*2+1]};
            hannWindow(in,fft_size);
            fftwf_execute((fftwf_plan)plan_);
            for (int i=0;i<fft_size;++i) {
                double re=out[i][0], im=out[i][1];
                pwr[(size_t)i]+=(re*re+im*im)/norm;
            }
        }
        r.power_bins.resize((size_t)fft_size);
        for (int i=0;i<fft_size;++i)
            r.power_bins[(size_t)i]=10.0*std::log10(pwr[(size_t)i]/n_avg+1e-30);
        fftshift(r.power_bins);
        r.success=true;
    } catch (const std::exception& ex) {
        r.error_msg=ex.what();
    }
    return r;
}

} // namespace sdr

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
