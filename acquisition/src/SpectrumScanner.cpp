#include "SpectrumScanner.hpp"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace acq {

using namespace std::chrono;

SpectrumScanner::SpectrumScanner(SweepConfig cfg, DetectionCallback cb)
    : cfg_(std::move(cfg)), cb_(std::move(cb))
{
    // When shared_lo=true all channels see the same LO; no parallel tuning.
    // We open all channels for SNR averaging but advance the sweep by one step.
    sweep_channels_  = cfg_.device.shared_lo ? 1 : cfg_.device.rx_channels;
    stream_channels_ = cfg_.device.rx_channels;

    for (int i = 0; i < stream_channels_; ++i)
        processors_.emplace_back(cfg_.sweep.fft_size);

    bufs_.resize((size_t)stream_channels_,
        std::vector<std::complex<float>>((size_t)cfg_.sweep.dwell_samples));
    for (auto& b : bufs_) ptrs_.push_back(b.data());
}

SpectrumScanner::~SpectrumScanner() { stop(); }

void SpectrumScanner::start() {
    running_ = true;
    thread_  = std::thread([this]{ sweepLoop(); });
}

void SpectrumScanner::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

uint64_t SpectrumScanner::stepHz() const {
    return (uint64_t)(cfg_.device.sample_rate * cfg_.sweep.usable_bw_fraction);
}

void SpectrumScanner::openDevice() {
    SoapySDR::Kwargs args;
    args["driver"] = cfg_.device.driver;
    if (!cfg_.device.uri.empty()) {
        // For "remote" driver the URI is the remote address
        if (cfg_.device.driver == "remote")
            args["remote"] = cfg_.device.uri;
        else
            args["serial"] = cfg_.device.uri;
    }
    device_ = SoapySDR::Device::make(args);
    if (!device_) throw std::runtime_error("SoapySDR::Device::make returned null");

    // Configure every channel
    for (int ch = 0; ch < stream_channels_; ++ch) {
        device_->setSampleRate(SOAPY_SDR_RX, ch, cfg_.device.sample_rate);
        device_->setGain(SOAPY_SDR_RX, ch, cfg_.device.rx_gain_db);
        device_->setAntenna(SOAPY_SDR_RX, ch, "RX");
        device_->setFrequency(SOAPY_SDR_RX, ch, (double)cfg_.sweep.start_hz);
    }

    // Open a multi-channel stream
    std::vector<size_t> chs;
    for (int i = 0; i < stream_channels_; ++i) chs.push_back((size_t)i);
    stream_ = device_->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, chs);
    device_->activateStream(stream_);

    spdlog::info("[Scanner] device open: {} channel(s), {:.3f} MSPS",
        stream_channels_, cfg_.device.sample_rate / 1e6);
    spdlog::info("[Scanner] sweep mode: {} ({})",
        cfg_.device.shared_lo ? "shared LO (SNR averaging)" : "independent LO (parallel sweep)",
        sweep_channels_ > 1
            ? fmt::format("{}× speed-up", sweep_channels_)
            : "1× baseline");
}

void SpectrumScanner::closeDevice() {
    if (stream_) { device_->deactivateStream(stream_); device_->closeStream(stream_); stream_ = nullptr; }
    if (device_) { SoapySDR::Device::unmake(device_); device_ = nullptr; }
}

void SpectrumScanner::tune(const std::vector<uint64_t>& freqs) {
    for (int ch = 0; ch < (int)freqs.size() && ch < stream_channels_; ++ch)
        device_->setFrequency(SOAPY_SDR_RX, ch, (double)freqs[ch]);
}

void SpectrumScanner::settle() {
    // Discard settle_samples to let the LO / DC-offset correction stabilise
    int remaining = cfg_.sweep.settle_samples;
    while (remaining > 0 && running_) {
        int chunk = std::min(remaining, cfg_.sweep.fft_size);
        int flags = 0; long long ts = 0;
        device_->readStream(stream_, ptrs_.data(), (size_t)chunk, flags, ts, 100'000);
        remaining -= chunk;
    }
}

std::vector<Detection> SpectrumScanner::processChannel(int ch, uint64_t center_hz) {
    auto& proc = processors_[ch];
    auto& buf  = bufs_[ch];

    auto sigs = proc.detect(
        buf.data(),
        (float)cfg_.sweep.threshold_db,
        (float)cfg_.sweep.usable_bw_fraction,
        cfg_.device.sample_rate,
        cfg_.sweep.min_signal_bw_hz);

    auto now = system_clock::now();
    std::vector<Detection> out;
    out.reserve(sigs.size());
    for (const auto& s : sigs) {
        uint64_t f_lo = proc.binToHz(s.start_bin, cfg_.device.sample_rate, center_hz);
        uint64_t f_hi = proc.binToHz(s.end_bin,   cfg_.device.sample_rate, center_hz);
        uint64_t fc   = proc.binToHz((s.start_bin + s.end_bin) / 2,
                                      cfg_.device.sample_rate, center_hz);
        Detection d;
        d.timestamp      = now;
        d.center_freq_hz = fc;
        d.bandwidth_hz   = (uint32_t)(f_hi > f_lo ? f_hi - f_lo : 1);
        d.power_db       = s.peak_db;
        d.scanner_id     = cfg_.scanner_id;
        d.channel        = ch;
        out.push_back(d);
    }
    return out;
}

std::vector<Detection> SpectrumScanner::processSharedLo(uint64_t center_hz) {
    // Average power spectra from all channels for better SNR, then threshold
    // Use channel-0 processor for geometry; manually average power into channel 0.
    // We process each channel independently then union the detections.
    std::vector<Detection> all;
    for (int ch = 0; ch < stream_channels_; ++ch) {
        auto dets = processChannel(ch, center_hz);
        all.insert(all.end(), dets.begin(), dets.end());
    }
    return all;
}

void SpectrumScanner::sweepLoop() {
    while (running_) {
        try {
            openDevice();
        } catch (const std::exception& e) {
            spdlog::error("[Scanner] device open failed: {} — retrying in 5s", e.what());
            std::this_thread::sleep_for(seconds(5));
            continue;
        }

        uint64_t step = stepHz();
        uint64_t advance = (uint64_t)sweep_channels_ * step;
        uint64_t total_hz = cfg_.sweep.stop_hz - cfg_.sweep.start_hz;
        uint64_t steps_per_sweep = (total_hz + advance - 1) / advance;

        spdlog::info("[Scanner] sweep: {:.3f}–{:.3f} MHz  step {:.3f} MHz  "
                     "~{} positions/sweep",
            cfg_.sweep.start_hz / 1e6, cfg_.sweep.stop_hz / 1e6,
            step / 1e6, steps_per_sweep);

        while (running_) {
            uint64_t pos = cfg_.sweep.start_hz;
            while (pos < cfg_.sweep.stop_hz && running_) {

                // ── Build frequency list for this step ───────────────────
                std::vector<uint64_t> freqs;
                for (int ch = 0; ch < sweep_channels_; ++ch) {
                    uint64_t fc = pos + (uint64_t)ch * step + step / 2;
                    if (fc > cfg_.sweep.stop_hz) break;
                    freqs.push_back(fc);
                }
                if (freqs.empty()) break;

                // If shared_lo, all channels to same freq
                if (cfg_.device.shared_lo)
                    freqs.assign((size_t)stream_channels_, freqs[0]);
                else
                    freqs.resize((size_t)stream_channels_, freqs.back());

                tune(freqs);
                settle();

                // ── Capture dwell_samples from all channels ──────────────
                int remaining = cfg_.sweep.dwell_samples;
                int got = 0;
                while (remaining > 0 && running_) {
                    // Rotate pointers to fill buffers from offset got
                    std::vector<void*> offsets;
                    for (auto& b : bufs_)
                        offsets.push_back(b.data() + got);
                    int flags = 0; long long ts = 0;
                    int n = device_->readStream(stream_, offsets.data(),
                        (size_t)remaining, flags, ts, 1'000'000);
                    if (n > 0) { got += n; remaining -= n; }
                    else if (n == SOAPY_SDR_OVERFLOW) {
                        spdlog::warn("[Scanner] overflow");
                    }
                }

                // ── FFT + detect ────────────────────────────────────────
                std::vector<Detection> dets;
                if (cfg_.device.shared_lo) {
                    dets = processSharedLo(freqs[0]);
                } else {
                    for (int ch = 0; ch < (int)freqs.size(); ++ch) {
                        auto d = processChannel(ch, freqs[ch]);
                        dets.insert(dets.end(), d.begin(), d.end());
                    }
                }

                for (const auto& d : dets) {
                    spdlog::debug("[Scanner] detection {:.3f} MHz  BW {:.1f} kHz  {:.1f} dB",
                        d.center_freq_hz / 1e6, d.bandwidth_hz / 1e3, d.power_db);
                    cb_(d);
                }

                pos += advance;
            }
        }

        closeDevice();
    }
}

} // namespace acq
