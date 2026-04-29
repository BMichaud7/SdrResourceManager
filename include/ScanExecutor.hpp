#pragma once
#include "Types.hpp"
#include <atomic>
#include <thread>
#include <vector>
#include <functional>
#include <string>

namespace sdr {
class IQStreamer;

class ScanExecutor {
public:
    // Called by ScanExecutor to retune before each dwell.
    // Returns true on success. Signature: (center_freq_hz, sample_rate_sps).
    using RetuneFn = std::function<bool(double cf_hz, double sr_sps)>;
    using DoneCb   = std::function<void(const std::string& task_id, bool ok)>;

    ScanExecutor(const std::string& task_id, const ScanParams& params,
                 RetuneFn retune, std::vector<IQStreamer*> streamers, DoneCb done);
    ~ScanExecutor();
    ScanExecutor(const ScanExecutor&)=delete;
    ScanExecutor& operator=(const ScanExecutor&)=delete;

    void start();
    void stop();
    bool isRunning()    const { return running_.load(); }
    int  currentStep()  const { return step_.load(); }

private:
    std::string              task_id_;
    ScanParams               params_;
    RetuneFn                 retune_;
    std::vector<IQStreamer*> streamers_;
    DoneCb                   done_;
    std::atomic<bool>        running_{false};
    std::atomic<int>         step_{0};
    std::thread              thread_;
    void loop();
};
} // namespace sdr
