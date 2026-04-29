#pragma once
#include "Types.hpp"
#include <SoapySDR/Device.hpp>
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <functional>
#include <vector>

namespace sdr {

class IQStreamer {
public:
    struct Config {
        std::string task_id;
        std::string stream_id;
        int         channel_index  = 0;
        std::string dest_ip;
        int         dest_port      = 0;
        int         packet_samples = 1024;
        int64_t     task_start_ms  = 0;
        double      sample_rate    = 0.0;  // populated at stream start; used in packet header
    };

    IQStreamer(const Config& cfg,
               SoapySDR::Device* dev,
               SoapySDR::Stream* stream,
               TaskErrorCb       on_error = nullptr);
    ~IQStreamer();
    IQStreamer(const IQStreamer&)=delete;
    IQStreamer& operator=(const IQStreamer&)=delete;

    void start();
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    void updateCenterFreq(double new_cf_hz);
    StreamMetrics getMetrics() const;

private:
    Config            cfg_;
    SoapySDR::Device* dev_;
    SoapySDR::Stream* stream_;
    TaskErrorCb       on_error_;

    std::atomic<bool>   running_       {false};
    std::atomic<double> current_cf_    {0.0};
    std::atomic<bool>   dwell_changed_ {false};
    std::thread         thread_;
    int                 udp_fd_        = -1;

    mutable std::mutex mu_;
    StreamMetrics      metrics_;
    uint32_t           seq_ = 0;

    void     workerLoop();
    bool     openUdpSocket();
    void     closeUdpSocket();
    void     sendPacket(const float* s, uint16_t n, uint64_t ts_ns, uint8_t flags);
};

} // namespace sdr
