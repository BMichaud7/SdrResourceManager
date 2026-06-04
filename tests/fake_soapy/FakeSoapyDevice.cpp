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
#include "FakeSoapyControl.hpp"
#include "FakeSoapyDevice.hpp"
#include <SoapySDR/Registry.hpp>
#include <stdexcept>

// ── Global control variable definitions ──────────────────────────────────────
namespace FakeSoapy {
    std::atomic<bool>   fail_open        {false};
    std::atomic<bool>   fail_read        {false};
    std::atomic<bool>   overflow_next    {false};
    std::atomic<int>    timeout_count    {0};
    std::atomic<int>    samples_per_read {256};
    std::atomic<float>  sample_value     {0.0f};
    std::atomic<double> reported_temp    {25.0};
    std::atomic<int>    read_delay_us    {500};  // 0.5 ms — throttles IQStreamer threads

    void reset() {
        fail_open       .store(false);
        fail_read       .store(false);
        overflow_next   .store(false);
        timeout_count   .store(0);
        samples_per_read.store(256);
        sample_value    .store(0.0f);
        reported_temp   .store(25.0);
        read_delay_us   .store(500);
    }
}

// ── SoapySDR plugin registration ──────────────────────────────────────────────
static SoapySDR::KwargsList findFake(const SoapySDR::Kwargs&) {
    if (FakeSoapy::fail_open.load()) return {};
    SoapySDR::Kwargs k;
    k["driver"] = "fake";
    k["label"]  = "Fake SDR Device (unit-test)";
    return {k};
}

static SoapySDR::Device* makeFake(const SoapySDR::Kwargs& args) {
    if (FakeSoapy::fail_open.load())
        throw std::runtime_error("FakeSoapy: forced open failure");
    return new FakeSoapyDevice(args);
}

// Static constructor registers "fake" driver before any test runs.
static SoapySDR::Registry gFakeRegistry("fake", &findFake, &makeFake,
                                         SOAPY_SDR_ABI_VERSION);

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
