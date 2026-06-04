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
#include "ScanExecutor.hpp"
#include "IQStreamer.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>

namespace sdr {
using namespace std::chrono;

ScanExecutor::ScanExecutor(const std::string& id, const ScanParams& p,
    RetuneFn retune, std::vector<IQStreamer*> s, DoneCb done)
    : task_id_(id), params_(p), retune_(std::move(retune)),
      streamers_(std::move(s)), done_(std::move(done)) {}

ScanExecutor::~ScanExecutor() { stop(); }

void ScanExecutor::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&ScanExecutor::loop, this);
    spdlog::info("ScanExecutor [{}] started {} entries repeat={}", task_id_, params_.entries.size(), params_.repeat);
}

void ScanExecutor::stop() {
    running_.store(false);
    if (!thread_.joinable()) return;
    // If called from the scan thread itself (via done-callback), joining would deadlock.
    if (thread_.get_id() == std::this_thread::get_id())
        thread_.detach();
    else
        thread_.join();
}

void ScanExecutor::loop() {
    if (params_.entries.empty()) { if(done_) done_(task_id_,false); return; }
    bool go=true;
    while (go && running_.load()) {
        for (size_t i=0; i<params_.entries.size() && running_.load(); ++i) {
            const auto& e = params_.entries[i];
            step_.store((int)i);
            if (!retune_(e.center_freq_hz, e.sample_rate_sps)) {
                spdlog::error("ScanExecutor [{}] retune failed step {}", task_id_, i);
                if (done_) done_(task_id_, false);
                return;
            }
            for (auto* s : streamers_)
                if (s && s->isRunning()) s->updateCenterFreq(e.center_freq_hz);
            spdlog::debug("ScanExecutor [{}] step {} cf={:.3f}MHz dwell={}ms", task_id_, i+1, e.center_freq_hz/1e6, e.dwell_ms);
            auto dl = steady_clock::now() + milliseconds(e.dwell_ms);
            while (running_.load()) {
                auto now = steady_clock::now();
                if (now >= dl) break;
                // Sleep until the deadline or 10ms before it (to stay responsive
                // to stop() without burning CPU on a busy-wait).
                auto wake = dl - milliseconds(10);
                if (wake > now) std::this_thread::sleep_until(wake);
            }
        }
        if (!params_.repeat) go=false;
    }
    spdlog::info("ScanExecutor [{}] done", task_id_);
    if(done_) done_(task_id_,true);
}
} // namespace sdr
