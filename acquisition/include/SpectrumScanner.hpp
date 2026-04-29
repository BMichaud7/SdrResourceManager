#pragma once
#include "SweepConfig.hpp"
#include "FftProcessor.hpp"
#include "Types.hpp"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Types.hpp>
#include <atomic>
#include <thread>
#include <functional>
#include <vector>
#include <complex>

namespace acq {

// Continuously sweeps a configurable frequency range using one or more RX
// channels.  When shared_lo=false each channel is tuned to a different
// centre frequency so the sweep advances N×faster.  When shared_lo=true
// all channels see the same spectrum and their power spectra are averaged
// for improved SNR before thresholding.
class SpectrumScanner {
public:
    using DetectionCallback = std::function<void(const Detection&)>;

    explicit SpectrumScanner(SweepConfig cfg, DetectionCallback cb);
    ~SpectrumScanner();

    SpectrumScanner(const SpectrumScanner&)            = delete;
    SpectrumScanner& operator=(const SpectrumScanner&) = delete;

    void start();
    void stop();   // blocks until the sweep thread exits

private:
    SweepConfig      cfg_;
    DetectionCallback cb_;

    SoapySDR::Device* device_{nullptr};
    SoapySDR::Stream* stream_{nullptr};

    int sweep_channels_{1};  // channels used to advance the sweep position
    int stream_channels_{1}; // channels actually open (always = rx_channels)

    std::vector<FftProcessor>              processors_;
    std::vector<std::vector<std::complex<float>>> bufs_;
    std::vector<void*>                     ptrs_;

    std::atomic<bool> running_{false};
    std::thread       thread_;

    void sweepLoop();
    void openDevice();
    void closeDevice();
    void settle();          // discard settle_samples after retune
    void tune(const std::vector<uint64_t>& freqs);

    std::vector<Detection> processChannel(int ch, uint64_t center_hz);
    std::vector<Detection> processSharedLo(uint64_t center_hz); // averaged

    uint64_t stepHz() const; // usable bandwidth per dwell
};

} // namespace acq
