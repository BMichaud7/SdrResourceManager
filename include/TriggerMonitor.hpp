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
