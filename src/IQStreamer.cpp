#include "IQStreamer.hpp"
#include <spdlog/spdlog.h>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <cmath>
#include <chrono>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/uio.h>

namespace sdr {
using namespace std::chrono;

IQStreamer::IQStreamer(const Config& cfg, SoapySDR::Device* dev,
                       SoapySDR::Stream* stream, TaskErrorCb on_error)
    : cfg_(cfg), dev_(dev), stream_(stream), on_error_(std::move(on_error))
{
    metrics_.stream_id    = cfg_.stream_id;
    metrics_.channel_type = "RX";
    metrics_.channel_index= cfg_.channel_index;
    metrics_.udp_port     = cfg_.dest_port;
    current_cf_.store(0.0);
}

IQStreamer::~IQStreamer() { stop(); }

bool IQStreamer::openUdpSocket() {
    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) return false;
    int sndbuf = 8*1024*1024;
    ::setsockopt(udp_fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons((uint16_t)cfg_.dest_port);
    dst.sin_addr.s_addr = ::inet_addr(cfg_.dest_ip.c_str());
    if (::connect(udp_fd_, (sockaddr*)&dst, sizeof(dst)) < 0) {
        ::close(udp_fd_); udp_fd_=-1; return false;
    }
    return true;
}

void IQStreamer::closeUdpSocket() {
    if (udp_fd_>=0) { ::close(udp_fd_); udp_fd_=-1; }
}

void IQStreamer::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    current_sr_hz_.store((uint32_t)cfg_.sample_rate, std::memory_order_release);
    if (!openUdpSocket()) {
        running_.store(false, std::memory_order_release);
        throw std::runtime_error("IQStreamer: UDP open failed → "+cfg_.dest_ip+":"+std::to_string(cfg_.dest_port));
    }
    thread_ = std::thread(&IQStreamer::workerLoop, this);
    spdlog::info("IQStreamer [{}] started → {}:{}", cfg_.stream_id, cfg_.dest_ip, cfg_.dest_port);
}

void IQStreamer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (thread_.joinable()) thread_.join();
    closeUdpSocket();
    spdlog::info("IQStreamer [{}] stopped pkts={} oflw={}", cfg_.stream_id, metrics_.packets_sent, metrics_.overflows);
}

void IQStreamer::updateCenterFreq(double new_cf_hz) {
    current_cf_.store(new_cf_hz, std::memory_order_release);
    dwell_changed_.store(true, std::memory_order_release);
}

void IQStreamer::updateSampleRate(double new_sr_sps) {
    current_sr_hz_.store((uint32_t)new_sr_sps, std::memory_order_release);
}

void IQStreamer::pauseForRetune(int settle_samples) {
    drain_countdown_.store(settle_samples, std::memory_order_release);
}

void IQStreamer::sendPacket(const float* samples, uint16_t n, uint64_t ts_ns, uint8_t flags) {
    IqPacketHeader hdr{};
    hdr.magic          = IQ_PACKET_MAGIC;
    hdr.sequence       = seq_++;
    hdr.timestamp_ns   = ts_ns;
    hdr.center_freq_hz = (uint64_t)current_cf_.load(std::memory_order_relaxed);
    hdr.sample_rate    = current_sr_hz_.load(std::memory_order_relaxed);
    hdr.num_samples    = n;
    hdr.channel_index  = (uint8_t)cfg_.channel_index;
    hdr.flags          = flags;

    struct iovec iov[2];
    iov[0].iov_base = &hdr;
    iov[0].iov_len  = sizeof(hdr);
    iov[1].iov_base = const_cast<float*>(samples);
    iov[1].iov_len  = (size_t)n * 2 * sizeof(float);
    struct msghdr msg{};
    msg.msg_iov = iov; msg.msg_iovlen = 2;
    ssize_t sent = ::sendmsg(udp_fd_, &msg, MSG_DONTWAIT);

    std::lock_guard lock(mu_);
    if (sent>0) {
        ++metrics_.packets_sent;
        metrics_.samples_total += n;
        metrics_.throughput_mbps = 0.9*metrics_.throughput_mbps + 0.1*(sent*8.0/1e6);
    }
    if (flags & IQ_FLAG_OVERFLOW) ++metrics_.overflows;
}

void IQStreamer::workerLoop() {
    const int N = cfg_.packet_samples;
    std::vector<float> buf((size_t)N*2);
    void* bufs[1] = {buf.data()};
    int flags=0; long long hw_ts=0;
    auto t0 = steady_clock::now();
    int errs=0;
    bool first=true;

    while (running_.load(std::memory_order_acquire)) {
        int ret = dev_->readStream(stream_, bufs, (size_t)N, flags, hw_ts, 500'000LL);
        if (!running_.load(std::memory_order_acquire)) break;
        if (ret == SOAPY_SDR_TIMEOUT) continue;

        uint8_t pkt_flags = 0;
        if (ret == SOAPY_SDR_OVERFLOW) { pkt_flags|=IQ_FLAG_OVERFLOW; ++errs; }
        else if (ret<0) {
            ++errs;
            spdlog::error("IQStreamer [{}] readStream error {} ({})", cfg_.stream_id, SoapySDR::errToStr(ret), errs);
            if (errs>=20) {
                if (on_error_) on_error_(cfg_.task_id, SoapySDR::errToStr(ret));
                break;
            }
            continue;
        } else { errs=0; }

        // Drain settle samples after a retune — read hardware but discard UDP send
        if (int rem = drain_countdown_.load(std::memory_order_acquire); rem > 0) {
            drain_countdown_.fetch_sub(1, std::memory_order_release);
            continue;
        }

        if (first) { pkt_flags|=IQ_FLAG_FIRST_PACKET; first=false; }
        if (dwell_changed_.exchange(false, std::memory_order_acq_rel)) pkt_flags|=IQ_FLAG_DWELL_CHANGE;

        uint64_t ts_ns = (uint64_t)duration_cast<nanoseconds>(steady_clock::now()-t0).count();
        uint16_t n = ret>0 ? (uint16_t)std::min(ret,N) : 0u;
        if (n>0||pkt_flags) sendPacket(buf.data(), n, ts_ns, pkt_flags);

        if (n>=64) {
            float rms_sq=0;
            for (int i=0;i<64*2;++i) rms_sq+=buf[static_cast<size_t>(i)]*buf[static_cast<size_t>(i)];
            float rdbfs = 10.f*std::log10(rms_sq/64.f+1e-30f);
            std::lock_guard lock(mu_);
            metrics_.rssi_dbfs = 0.95f*metrics_.rssi_dbfs + 0.05f*rdbfs;
        }
    }
}

StreamMetrics IQStreamer::getMetrics() const {
    std::lock_guard lock(mu_); return metrics_;
}

} // namespace sdr
