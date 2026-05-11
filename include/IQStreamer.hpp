#pragma once
#include "sdr/Types.hpp"
#include <SoapySDR/Device.hpp>
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <functional>
#include <vector>
#include <netinet/in.h>

namespace sdr {

class IQStreamer {
public:
    // Extra channel descriptor for multi-channel shared streams.
    struct ChannelDest {
        int         channel_index = 0;
        std::string stream_id;
        std::string dest_ip;
        int         dest_port     = 0;
    };

    struct Config {
        std::string task_id;
        std::string stream_id;
        int         channel_index  = 0;
        std::string dest_ip;       // initial dest; additional dests via addDest()
        int         dest_port      = 0;
        int         packet_samples = 1024;
        int64_t     task_start_ms  = 0;
        double      sample_rate    = 0.0;
        // Channels 1..N of a multi-channel SoapySDR stream.
        // When non-empty, workerLoop reads N+1 buffers and demuxes each channel
        // to its own UDP port — only ONE IQStreamer owns the stream.
        std::vector<ChannelDest> extra_channels;
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
    void updateSampleRate(double new_sr_sps);

    // Drain `settle_samples` reads from the hardware without sending UDP packets.
    void pauseForRetune(int settle_samples);

    // Multi-destination fan-out. Each task subscribes with its own UDP endpoint.
    // start() auto-adds the dest from Config if dest_ip is non-empty.
    void addDest(const std::string& task_id, const std::string& stream_id,
                 const std::string& ip, int port);
    int  removeDest(const std::string& task_id); // returns remaining dest count
    int  destCount() const;

    StreamMetrics getMetrics() const;

private:
    struct Dest {
        std::string task_id;
        std::string stream_id;
        int         fd = -1;
    };

    Config            cfg_;
    SoapySDR::Device* dev_;
    SoapySDR::Stream* stream_;
    TaskErrorCb       on_error_;

    std::atomic<bool>     running_         {false};
    std::atomic<double>   current_cf_      {0.0};
    std::atomic<bool>     dwell_changed_   {false};
    std::atomic<int>      drain_countdown_ {0};
    std::atomic<uint32_t> current_sr_hz_   {0};
    std::thread           thread_;

    mutable std::mutex dests_mu_;
    std::vector<Dest>  dests_;

    mutable std::mutex mu_;
    StreamMetrics      metrics_;
    uint32_t           seq_ = 0;

    void workerLoop();
    void sendPacket(const float* s, uint16_t n, uint64_t ts_ns, uint8_t flags);
    void sendPacketToFd(int fd, uint32_t& seq, int ch_idx,
                        const float* s, uint16_t n, uint64_t ts_ns, uint8_t flags);
};

} // namespace sdr
