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
#include "TriggerMonitor.hpp"
#include "IQStreamer.hpp"
#include <spdlog/spdlog.h>
#include <SoapySDR/Errors.hpp>
#include <cmath>
#include <chrono>
#include <thread>

namespace sdr {
using namespace std::chrono;

TriggerMonitor::TriggerMonitor(const std::string& id, const TriggerParams& p,
    SoapySDR::Device* dev, SoapySDR::Stream* stream,
    IQStreamer* s, double rate, DoneCb done)
    : task_id_(id), params_(p), dev_(dev), stream_(stream),
      streamer_(s), rate_(rate), done_(std::move(done)) {}

TriggerMonitor::~TriggerMonitor() { stop(); }

void TriggerMonitor::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&TriggerMonitor::loop, this);
    spdlog::info("TriggerMonitor [{}] started thr={:.1f}dBFS", task_id_, params_.threshold_dbfs);
}

void TriggerMonitor::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

float TriggerMonitor::rmsDbfs(const float* b, int n) {
    float sq=0;
    for (int i=0;i<n*2;++i) sq+=b[i]*b[i];
    return 10.f*std::log10(sq/(float)n+1e-30f);
}

void TriggerMonitor::loop() {
    int pre_blks = std::max(1, (int)((params_.pre_trigger_ms/1000.0)*rate_/BLK));
    std::deque<std::vector<float>> ring;
    std::vector<float> buf((size_t)BLK*2);
    void* bufs[1]={buf.data()};
    int flags=0; long long hw_ts=0;
    bool triggered=false;
    int post_rem=0, caps_done=0;

    while (running_.load()) {
        int ret=dev_->readStream(stream_, bufs, (size_t)BLK, flags, hw_ts, 200'000LL);
        if (ret==SOAPY_SDR_TIMEOUT||ret<0) continue;
        int n=std::min(ret,BLK);
        float rms=rmsDbfs(buf.data(),n);

        if (!triggered) {
            std::vector<float> blk(buf.begin(), buf.begin()+n*2);
            ring.push_back(std::move(blk));
            if ((int)ring.size()>pre_blks) ring.pop_front();
            if (rms >= params_.threshold_dbfs) {
                triggered=true;
                post_rem=(int)((params_.post_trigger_ms/1000.0)*rate_);
                spdlog::info("TriggerMonitor [{}] TRIGGERED {:.1f}dBFS", task_id_, rms);
                ring.clear();
                if (streamer_ && !streamer_->isRunning()) streamer_->start();
            }
        } else {
            post_rem -= n;
            if (post_rem<=0) {
                triggered=false;
                if (streamer_ && streamer_->isRunning()) streamer_->stop();
                ++caps_done; captures_.store(caps_done);
                spdlog::info("TriggerMonitor [{}] capture #{} done", task_id_, caps_done);
                if (params_.max_captures>0 && caps_done>=params_.max_captures) break;
            }
        }
    }
    if(done_) done_(task_id_,true);
}
} // namespace sdr

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
