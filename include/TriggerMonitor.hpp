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
/**
 * @file TriggerMonitor.hpp
 * @brief Background monitor for TRIGGERED tasks — waits for power threshold then captures.
 *
 * Samples the IQ stream power in real-time. When RMS exceeds threshold_dbfs it
triggers a capture window and decrements the max_captures counter.
 */
#pragma once
#include "sdr/Types.hpp"
#include <SoapySDR/Device.hpp>
#include <atomic>
#include <thread>
#include <functional>
#include <string>
#include <deque>
#include <vector>

namespace sdr {
class IQStreamer;

class TriggerMonitor {
public:
    using DoneCb = std::function<void(const std::string& task_id, bool ok)>;

    TriggerMonitor(const std::string& task_id, const TriggerParams& params,
                   SoapySDR::Device* dev, SoapySDR::Stream* stream,
                   IQStreamer* streamer, double sample_rate_sps, DoneCb done);
    ~TriggerMonitor();
    TriggerMonitor(const TriggerMonitor&)=delete;
    TriggerMonitor& operator=(const TriggerMonitor&)=delete;

    void start();
    void stop();
    bool isRunning() const { return running_.load(); }
    int  captureCount() const { return captures_.load(); }

private:
    std::string        task_id_;
    TriggerParams      params_;
    SoapySDR::Device*  dev_;
    SoapySDR::Stream*  stream_;
    IQStreamer*        streamer_;
    double             rate_;
    DoneCb             done_;
    std::atomic<bool>  running_{false};
    std::atomic<int>   captures_{0};
    std::thread        thread_;
    static constexpr int BLK=256;
    void loop();
    float rmsDbfs(const float* b, int n);
};
} // namespace sdr

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
