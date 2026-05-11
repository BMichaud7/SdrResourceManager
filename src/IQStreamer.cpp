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
#include <algorithm>

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

// ── Multi-destination management ─────────────────────────────────────────────

void IQStreamer::addDest(const std::string& task_id, const std::string& stream_id,
                          const std::string& ip, int port)
{
    Dest d;
    d.task_id   = task_id;
    d.stream_id = stream_id;
    d.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (d.fd < 0) {
        spdlog::error("IQStreamer::addDest: socket() failed for {}:{}", ip, port);
        return;
    }
    int sndbuf = 8*1024*1024;
    ::setsockopt(d.fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons((uint16_t)port);
    dst.sin_addr.s_addr = ::inet_addr(ip.c_str());
    if (::connect(d.fd, (sockaddr*)&dst, sizeof(dst)) < 0) {
        ::close(d.fd);
        spdlog::error("IQStreamer::addDest: connect() failed for {}:{}", ip, port);
        return;
    }
    std::lock_guard lock(dests_mu_);
    dests_.push_back(std::move(d));
    spdlog::debug("IQStreamer [{}] +dest {} → {}:{}", cfg_.stream_id, task_id, ip, port);
}

int IQStreamer::removeDest(const std::string& task_id) {
    std::lock_guard lock(dests_mu_);
    auto it = std::find_if(dests_.begin(), dests_.end(),
        [&](const Dest& d){ return d.task_id == task_id; });
    if (it != dests_.end()) {
        if (it->fd >= 0) { ::close(it->fd); it->fd = -1; }
        dests_.erase(it);
        spdlog::debug("IQStreamer [{}] -dest {}", cfg_.stream_id, task_id);
    }
    return (int)dests_.size();
}

int IQStreamer::destCount() const {
    std::lock_guard lock(dests_mu_);
    return (int)dests_.size();
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

void IQStreamer::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    current_sr_hz_.store((uint32_t)cfg_.sample_rate, std::memory_order_release);
    // Auto-register the primary dest from Config (backward-compat single-dest path)
    if (!cfg_.dest_ip.empty() && cfg_.dest_port > 0)
        addDest(cfg_.task_id, cfg_.stream_id, cfg_.dest_ip, cfg_.dest_port);
    thread_ = std::thread(&IQStreamer::workerLoop, this);
    spdlog::info("IQStreamer [{}] started", cfg_.stream_id);
}

void IQStreamer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (thread_.joinable()) thread_.join();
    // Close all dest sockets
    std::lock_guard lock(dests_mu_);
    for (auto& d : dests_) if (d.fd >= 0) { ::close(d.fd); d.fd = -1; }
    dests_.clear();
    spdlog::info("IQStreamer [{}] stopped pkts={} oflw={}",
                 cfg_.stream_id, metrics_.packets_sent, metrics_.overflows);
}

// ── Tune helpers ─────────────────────────────────────────────────────────────

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

// ── Packet send ──────────────────────────────────────────────────────────────

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

    // Snapshot dest fds under lock, send outside lock
    std::vector<int> fds;
    {
        std::lock_guard lock(dests_mu_);
        fds.reserve(dests_.size());
        for (auto& d : dests_) if (d.fd >= 0) fds.push_back(d.fd);
    }

    ssize_t last_sent = 0;
    for (int fd : fds) {
        struct msghdr msg{};
        msg.msg_iov    = iov;
        msg.msg_iovlen = 2;
        last_sent = ::sendmsg(fd, &msg, MSG_DONTWAIT);
    }

    std::lock_guard lock(mu_);
    if (last_sent > 0) {
        ++metrics_.packets_sent;
        metrics_.samples_total += n;
        metrics_.throughput_mbps = 0.9*metrics_.throughput_mbps + 0.1*(last_sent*8.0/1e6);
    }
    if (flags & IQ_FLAG_OVERFLOW) ++metrics_.overflows;
}

// ── Per-fd packet send (extra channels in multi-channel mode) ─────────────────

void IQStreamer::sendPacketToFd(int fd, uint32_t& seq, int ch_idx,
                                 const float* samples, uint16_t n,
                                 uint64_t ts_ns, uint8_t flags) {
    IqPacketHeader hdr{};
    hdr.magic          = IQ_PACKET_MAGIC;
    hdr.sequence       = seq++;
    hdr.timestamp_ns   = ts_ns;
    hdr.center_freq_hz = (uint64_t)current_cf_.load(std::memory_order_relaxed);
    hdr.sample_rate    = current_sr_hz_.load(std::memory_order_relaxed);
    hdr.num_samples    = n;
    hdr.channel_index  = (uint8_t)ch_idx;
    hdr.flags          = flags;
    struct iovec iov[2];
    iov[0].iov_base = &hdr;
    iov[0].iov_len  = sizeof(hdr);
    iov[1].iov_base = const_cast<float*>(samples);
    iov[1].iov_len  = (size_t)n * 2 * sizeof(float);
    struct msghdr msg{};
    msg.msg_iov    = iov;
    msg.msg_iovlen = 2;
    ::sendmsg(fd, &msg, MSG_DONTWAIT);
}

// ── Worker loop ──────────────────────────────────────────────────────────────

void IQStreamer::workerLoop() {
    const int N    = cfg_.packet_samples;
    const int n_ch = 1 + (int)cfg_.extra_channels.size();

    // Per-channel sample buffers — one per SoapySDR channel in the stream.
    std::vector<std::vector<float>> ch_bufs(n_ch, std::vector<float>((size_t)N*2));
    std::vector<void*> soapy_bufs(n_ch);
    for (int i = 0; i < n_ch; ++i) soapy_bufs[i] = ch_bufs[i].data();

    // Open UDP sockets for extra channels (channel 0 uses the existing dests_ mechanism).
    std::vector<int> extra_fds;
    std::vector<uint32_t> extra_seqs(cfg_.extra_channels.size(), 0u);
    for (auto& ec : cfg_.extra_channels) {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        int sndbuf = 8*1024*1024;
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        sockaddr_in dst{};
        dst.sin_family      = AF_INET;
        dst.sin_port        = htons((uint16_t)ec.dest_port);
        dst.sin_addr.s_addr = ::inet_addr(ec.dest_ip.c_str());
        ::connect(fd, (sockaddr*)&dst, sizeof(dst));
        extra_fds.push_back(fd);
    }

    int flags=0; long long hw_ts=0;
    auto t0 = steady_clock::now();
    int errs=0;
    bool first=true;

    while (running_.load(std::memory_order_acquire)) {
        int ret = dev_->readStream(stream_, soapy_bufs.data(), (size_t)N, flags, hw_ts, 500'000LL);
        if (!running_.load(std::memory_order_acquire)) break;
        if (ret == SOAPY_SDR_TIMEOUT) continue;

        uint8_t pkt_flags = 0;
        if (ret == SOAPY_SDR_OVERFLOW) { pkt_flags|=IQ_FLAG_OVERFLOW; ++errs; }
        else if (ret<0) {
            ++errs;
            spdlog::error("IQStreamer [{}] readStream error {} ({})",
                          cfg_.stream_id, SoapySDR::errToStr(ret), errs);
            if (errs>=20) {
                if (on_error_) {
                    // Fire callback from a separate thread — calling it directly
                    // would deadlock: deactivateTask → stop() → join() cannot
                    // join the calling (worker) thread from itself.
                    auto cb  = on_error_;
                    auto tid = cfg_.task_id;
                    auto msg = std::string(SoapySDR::errToStr(ret));
                    std::thread([cb, tid, msg](){ cb(tid, msg); }).detach();
                }
                break;
            }
            continue;
        } else { errs=0; }

        if (int rem = drain_countdown_.load(std::memory_order_acquire); rem > 0) {
            drain_countdown_.fetch_sub(1, std::memory_order_release);
            continue;
        }

        if (first) { pkt_flags|=IQ_FLAG_FIRST_PACKET; first=false; }
        if (dwell_changed_.exchange(false, std::memory_order_acq_rel)) pkt_flags|=IQ_FLAG_DWELL_CHANGE;

        uint64_t ts_ns = (uint64_t)duration_cast<nanoseconds>(steady_clock::now()-t0).count();
        uint16_t nsamples = ret>0 ? (uint16_t)std::min(ret,N) : 0u;

        // Primary channel (index 0 in soapy_bufs → cfg_.channel_index)
        if (nsamples>0 || pkt_flags)
            sendPacket(ch_bufs[0].data(), nsamples, ts_ns, pkt_flags);

        // Extra channels: each gets its own buffer, sequence counter, and UDP socket
        for (int i = 0; i < (int)cfg_.extra_channels.size(); ++i) {
            if (nsamples>0 || pkt_flags)
                sendPacketToFd(extra_fds[i], extra_seqs[i],
                               cfg_.extra_channels[i].channel_index,
                               ch_bufs[i+1].data(), nsamples, ts_ns, pkt_flags);
        }

        if (nsamples >= 64) {
            float rms_sq=0;
            for (int i=0; i<64*2; ++i)
                rms_sq += ch_bufs[0][static_cast<size_t>(i)] * ch_bufs[0][static_cast<size_t>(i)];
            float rdbfs = 10.f*std::log10(rms_sq/64.f+1e-30f);
            std::lock_guard lock(mu_);
            metrics_.rssi_dbfs = 0.95f*metrics_.rssi_dbfs + 0.05f*rdbfs;
        }
    }

    for (int fd : extra_fds) ::close(fd);
}

StreamMetrics IQStreamer::getMetrics() const {
    std::lock_guard lock(mu_); return metrics_;
}

} // namespace sdr
