#pragma once
#include "sdr/Types.hpp"
#include "Ddc.hpp"
#include <SoapySDR/Device.hpp>
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <functional>
#include <vector>
#include <memory>
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

    void   updateCenterFreq(double new_cf_hz);
    void   updateSampleRate(double new_sr_sps);
    double currentCF() const { return current_cf_.load(std::memory_order_relaxed); }
    double currentSR() const { return (double)current_sr_hz_.load(std::memory_order_relaxed); }

    // Drain `settle_samples` reads from the hardware without sending UDP packets.
    void pauseForRetune(int settle_samples);

    // Multi-destination fan-out. Each task subscribes with its own UDP endpoint.
    // start() auto-adds the dest from Config if dest_ip is non-empty.
    void addDest(const std::string& task_id, const std::string& stream_id,
                 const std::string& ip, int port);
    int  removeDest(const std::string& task_id); // returns remaining dest count
    int  destCount() const;

    // DDC sub-band fan-out. The wideband stream is mixed, filtered, and decimated
    // to the task's requested band before sending. One sub-band per narrowband task.
    // cf_hz: sub-band center frequency; output_sr_hz: decimated sample rate;
    // wideband_sr_hz: current hardware sample rate (used to compute decimation ratio).
    void addSubBand(const std::string& task_id, const std::string& stream_id,
                    int channel_index, const std::string& ip, int port,
                    double cf_hz, double output_sr_hz, double wideband_sr_hz);
    int  removeSubBand(const std::string& task_id); // returns remaining total consumers
    int  subBandCount() const;
    int  totalConsumers() const; // destCount() + subBandCount()

    StreamMetrics getMetrics() const;

private:
    struct Dest {
        std::string task_id;
        std::string stream_id;
        int         fd = -1;
    };

    // DDC sub-band: one per narrowband task sharing this wideband stream.
    struct SubBand {
        std::string          task_id;
        std::string          stream_id;
        int                  channel_index = 0;
        int                  dest_fd       = -1;
        uint32_t             seq           = 0;
        uint64_t             center_freq_hz = 0;
        uint32_t             sample_rate_hz = 0;
        std::unique_ptr<Ddc> ddc;
        std::vector<float>   out_buf; // decimated output (cfg_.packet_samples+1)*2 floats
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

    // Both dests_ and subbands_ are protected by dests_mu_.
    mutable std::mutex   dests_mu_;
    std::vector<Dest>    dests_;
    std::vector<SubBand> subbands_;

    mutable std::mutex mu_;
    StreamMetrics      metrics_;
    uint32_t           seq_ = 0;

    void workerLoop();
    void sendPacket(const float* s, uint16_t n, uint64_t ts_ns, uint8_t flags);
    void sendPacketToFd(int fd, uint32_t& seq, int ch_idx,
                        const float* s, uint16_t n, uint64_t ts_ns, uint8_t flags);
    void sendSubBandPacket(SubBand& sb, uint16_t n, uint64_t ts_ns, uint8_t flags);
};

} // namespace sdr
