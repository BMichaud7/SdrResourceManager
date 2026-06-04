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
#include "FakeSoapyControl.hpp"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <thread>
#include <chrono>
#include <algorithm>
#include <string>
#include <vector>

// In-process SoapySDR device for unit tests.
// Registered under driver="fake" — no .so required.
// Tests control behaviour via FakeSoapy:: atomics.
class FakeSoapyDevice : public SoapySDR::Device {
public:
    explicit FakeSoapyDevice(const SoapySDR::Kwargs& = {}) {}

    std::string getDriverKey()   const override { return "fake"; }
    std::string getHardwareKey() const override { return "FakeSoapyDevice"; }
    size_t      getNumChannels(const int) const override { return 2; }

    // ── Frequency ────────────────────────────────────────────────────────
    void setFrequency(const int dir, const size_t ch, const double f,
                      const SoapySDR::Kwargs&) override { cf_[dir][ch] = f; }
    double getFrequency(const int dir, const size_t ch,
                        const std::string&) const override { return cf_[dir][ch]; }
    std::vector<std::string> listFrequencies(const int, const size_t) const override {
        return {"RF"};
    }

    // ── Sample rate ───────────────────────────────────────────────────────
    void   setSampleRate(const int dir, const size_t ch, const double r) override { rate_[dir][ch] = r; }
    double getSampleRate(const int dir, const size_t ch) const override { return rate_[dir][ch]; }

    // ── Gain ──────────────────────────────────────────────────────────────
    void   setGainMode(const int, const size_t, const bool) override {}
    bool   getGainMode(const int, const size_t) const override { return false; }
    void   setGain(const int, const size_t, const double g) override { gain_ = g; }
    double getGain(const int, const size_t) const override { return gain_; }

    // ── Stream ────────────────────────────────────────────────────────────
    SoapySDR::Stream* setupStream(const int, const std::string&,
                                   const std::vector<size_t>&,
                                   const SoapySDR::Kwargs&) override {
        return reinterpret_cast<SoapySDR::Stream*>(&stream_tok_);
    }
    int  activateStream(SoapySDR::Stream*, const int, const long long,
                        const size_t) override { return 0; }
    int  deactivateStream(SoapySDR::Stream*, const int, const long long) override { return 0; }
    void closeStream(SoapySDR::Stream*) override {}
    size_t getStreamMTU(SoapySDR::Stream*) const override { return 1024; }

    int readStream(SoapySDR::Stream*, void* const* buffs, const size_t numElems,
                   int& flags, long long& timeNs, const long) override {
        int delay = FakeSoapy::read_delay_us.load();
        if (delay > 0) std::this_thread::sleep_for(std::chrono::microseconds(delay));

        if (FakeSoapy::timeout_count.fetch_sub(1) > 0) return SOAPY_SDR_TIMEOUT;
        if (FakeSoapy::overflow_next.exchange(false))   return SOAPY_SDR_OVERFLOW;
        if (FakeSoapy::fail_read.load())                return SOAPY_SDR_NOT_SUPPORTED;

        size_t n = std::min(numElems,
                            static_cast<size_t>(FakeSoapy::samples_per_read.load()));
        float* buf = static_cast<float*>(buffs[0]);
        float  v   = FakeSoapy::sample_value.load();
        for (size_t i = 0; i < n * 2; ++i) buf[i] = v;
        flags = 0; timeNs = 0;
        return static_cast<int>(n);
    }

    // ── Sensor ────────────────────────────────────────────────────────────
    std::string readSensor(const std::string&) const override {
        return std::to_string(FakeSoapy::reported_temp.load());
    }
    std::vector<std::string> listSensors() const override { return {"temp0"}; }

private:
    double cf_[2][4]   = {};
    double rate_[2][4] = {};
    double gain_       = 30.0;
    int    stream_tok_ = 0;   // address used as opaque SoapySDR::Stream*
};

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
