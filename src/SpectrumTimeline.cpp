#include "SpectrumTimeline.hpp"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace sdr {

FitResult SpectrumTimeline::canFit(
    int64_t t_start, int64_t t_stop,
    double cf_hz, double bw_hz, double sr_sps,
    int rx_count, int tx_count,
    int max_rx, int max_tx,
    double guard_hz,
    bool shared_lo) const
{
    std::lock_guard lock(mu_);
    FitResult res;

    // Gather overlapping slots
    std::vector<const TimeFreqSlot*> over;
    for (auto& s : slots_)
        if (timeOverlap(t_start,t_stop,s.t_start,s.t_stop))
            over.push_back(&s);

    // ── Independent-channel path ─────────────────────────────────────────
    // Each task tunes its own channel(s); no shared RF window.
    // Only check that enough channels are available.
    if (!shared_lo) {
        int used_rx=0, used_tx=0;
        std::vector<bool> rx_used(static_cast<size_t>(max_rx),false);
        std::vector<bool> tx_used(static_cast<size_t>(max_tx),false);
        for (auto* s : over) {
            used_rx += (int)s->rx_channels.size();
            used_tx += (int)s->tx_channels.size();
            for (int c : s->rx_channels) if(c<max_rx) rx_used[static_cast<size_t>(c)]=true;
            for (int c : s->tx_channels) if(c<max_tx) tx_used[static_cast<size_t>(c)]=true;
        }
        if (used_rx + rx_count > max_rx) {
            res.reject_code   = RejectCode::CHANNEL_COUNT_EXCEEDED;
            res.reject_reason = "RX " + std::to_string(used_rx) + "+" + std::to_string(rx_count)
                              + ">" + std::to_string(max_rx);
            return res;
        }
        if (used_tx + tx_count > max_tx) {
            res.reject_code   = RejectCode::CHANNEL_COUNT_EXCEEDED;
            res.reject_reason = "TX " + std::to_string(used_tx) + "+" + std::to_string(tx_count)
                              + ">" + std::to_string(max_tx);
            return res;
        }
        std::vector<int> arx, atx;
        for (int i=0;i<max_rx&&(int)arx.size()<rx_count;++i) if(!rx_used[static_cast<size_t>(i)]) arx.push_back(i);
        for (int i=0;i<max_tx&&(int)atx.size()<tx_count;++i) if(!tx_used[static_cast<size_t>(i)]) atx.push_back(i);
        res.ok          = true;
        res.device_cf   = cf_hz;
        res.device_rate = sr_sps;
        res.placed_lo   = cf_hz - bw_hz/2.0;
        res.placed_hi   = cf_hz + bw_hz/2.0;
        res.avail_rx    = arx;
        res.avail_tx    = atx;
        return res;
    }

    // ── Shared-LO path (original logic) ──────────────────────────────────
    double dev_cf   = cf_hz;
    double dev_rate = sr_sps;
    if (!over.empty()) {
        dev_cf   = over[0]->center_freq_hz;
        dev_rate = over[0]->sample_rate_sps;
        if (cf_hz != dev_cf) {
            res.reject_code   = RejectCode::RETUNE_CONFLICT;
            res.reject_reason = "Device locked to "
                + std::to_string((long long)(dev_cf/1e6))
                + " MHz; requested "
                + std::to_string((long long)(cf_hz/1e6)) + " MHz";
            return res;
        }
        if (sr_sps != dev_rate) {
            res.reject_code   = RejectCode::RETUNE_CONFLICT;
            res.reject_reason = "Device sample rate locked to "
                + std::to_string((long long)(dev_rate/1e6))
                + " MSPS; requested "
                + std::to_string((long long)(sr_sps/1e6)) + " MSPS";
            return res;
        }
    }

    // Slice must fit in device window
    double win_lo = dev_cf - dev_rate/2.0;
    double win_hi = dev_cf + dev_rate/2.0;
    double req_lo = cf_hz  - bw_hz/2.0;
    double req_hi = cf_hz  + bw_hz/2.0;
    if (req_lo < win_lo || req_hi > win_hi) {
        res.reject_code = RejectCode::SPECTRUM_CONFLICT;
        res.reject_reason = "Slice ["
            + std::to_string((long long)(req_lo/1e6)) + "–"
            + std::to_string((long long)(req_hi/1e6))
            + " MHz] outside window ["
            + std::to_string((long long)(win_lo/1e6)) + "–"
            + std::to_string((long long)(win_hi/1e6)) + " MHz]";
        return res;
    }

    // Slice packing: left-to-right fit with guard
    using IV = std::pair<double,double>;
    std::vector<IV> occ;
    for (auto* s : over)
        occ.push_back({s->slice_lo_hz - guard_hz, s->slice_hi_hz + guard_hz});
    std::sort(occ.begin(), occ.end());

    double needed = bw_hz + 2.0*guard_hz;
    double try_lo = win_lo;
    bool   placed = false;
    double p_lo=0, p_hi=0;

    for (;;) {
        double try_hi = try_lo + needed;
        if (try_hi > win_hi) break;
        bool   clash = false;
        double push  = 0;
        for (auto& [olo,ohi] : occ) {
            if (try_lo < ohi && olo < try_hi) { clash=true; push=ohi; break; }
        }
        if (!clash) {
            placed = true;
            p_lo = try_lo + guard_hz;
            p_hi = p_lo   + bw_hz;
            break;
        }
        try_lo = push;
    }
    if (!placed) {
        res.reject_code = RejectCode::SPECTRUM_CONFLICT;
        res.reject_reason = "No "
            + std::to_string((long long)(bw_hz/1e3))
            + " kHz slot available in device window";
        return res;
    }

    // Channel availability
    int used_rx=0, used_tx=0;
    std::vector<bool> rx_used(static_cast<size_t>(max_rx),false);
    std::vector<bool> tx_used(static_cast<size_t>(max_tx),false);
    for (auto* s : over) {
        used_rx += (int)s->rx_channels.size();
        used_tx += (int)s->tx_channels.size();
        for (int c : s->rx_channels) if(c<max_rx) rx_used[static_cast<size_t>(c)]=true;
        for (int c : s->tx_channels) if(c<max_tx) tx_used[static_cast<size_t>(c)]=true;
    }
    if (used_rx + rx_count > max_rx) {
        res.reject_code = RejectCode::CHANNEL_COUNT_EXCEEDED;
        res.reject_reason = "RX " + std::to_string(used_rx) + "+" + std::to_string(rx_count) + ">" + std::to_string(max_rx);
        return res;
    }
    if (used_tx + tx_count > max_tx) {
        res.reject_code = RejectCode::CHANNEL_COUNT_EXCEEDED;
        res.reject_reason = "TX " + std::to_string(used_tx) + "+" + std::to_string(tx_count) + ">" + std::to_string(max_tx);
        return res;
    }

    std::vector<int> arx, atx;
    for (int i=0; i<max_rx && (int)arx.size()<rx_count; ++i)
        if (!rx_used[static_cast<size_t>(i)]) arx.push_back(i);
    for (int i=0; i<max_tx && (int)atx.size()<tx_count; ++i)
        if (!tx_used[static_cast<size_t>(i)]) atx.push_back(i);

    res.ok         = true;
    res.device_cf  = dev_cf;
    res.device_rate= dev_rate;
    res.placed_lo  = p_lo;
    res.placed_hi  = p_hi;
    res.avail_rx   = arx;
    res.avail_tx   = atx;
    return res;
}

SpectrumTimeline::CombineResult
SpectrumTimeline::canCombine(int64_t t_start, int64_t t_stop,
                              double cf_hz, double bw_hz,
                              int rx_count, int max_rx,
                              double guard_hz, double sr_max) const
{
    std::lock_guard lock(mu_);
    CombineResult res;

    std::vector<const TimeFreqSlot*> over;
    for (auto& s : slots_)
        if (timeOverlap(t_start, t_stop, s.t_start, s.t_stop))
            over.push_back(&s);

    if (over.empty()) {
        res.reject_reason = "No existing tasks to combine with";
        return res;
    }

    double req_lo = cf_hz - bw_hz / 2.0;
    double req_hi = cf_hz + bw_hz / 2.0;
    double min_lo = req_lo, max_hi = req_hi;
    for (auto* s : over) {
        min_lo = std::min(min_lo, s->slice_lo_hz);
        max_hi = std::max(max_hi, s->slice_hi_hz);
    }

    double combined_sr = (max_hi - min_lo) + 2.0 * guard_hz;
    double combined_cf = (min_lo + max_hi) / 2.0;

    if (combined_sr > sr_max) {
        res.reject_reason = "Combined SR " + std::to_string((long long)(combined_sr / 1e6))
                          + " MSPS exceeds device max " + std::to_string((long long)(sr_max / 1e6)) + " MSPS";
        return res;
    }

    int used_rx = 0;
    std::vector<bool> rx_used(static_cast<size_t>(max_rx), false);
    for (auto* s : over) {
        used_rx += (int)s->rx_channels.size();
        for (int c : s->rx_channels) if (c < max_rx) rx_used[static_cast<size_t>(c)] = true;
    }
    if (used_rx + rx_count > max_rx) {
        res.reject_reason = "RX " + std::to_string(used_rx) + "+" + std::to_string(rx_count)
                          + ">" + std::to_string(max_rx);
        return res;
    }

    std::vector<int> arx;
    for (int i = 0; i < max_rx && (int)arx.size() < rx_count; ++i)
        if (!rx_used[static_cast<size_t>(i)]) arx.push_back(i);

    res.ok           = true;
    res.combined_cf  = combined_cf;
    res.combined_sr  = combined_sr;
    res.new_slice_lo = req_lo;
    res.new_slice_hi = req_hi;
    res.avail_rx     = arx;
    return res;
}

void SpectrumTimeline::updateDeviceTune(double new_cf, double new_sr) {
    std::lock_guard lock(mu_);
    for (auto& s : slots_) {
        s.center_freq_hz  = new_cf;
        s.sample_rate_sps = new_sr;
    }
    spdlog::debug("Timeline: updateDeviceTune cf={:.3f}MHz sr={:.3f}MSPS",
                  new_cf / 1e6, new_sr / 1e6);
}

void SpectrumTimeline::insert(const TimeFreqSlot& s) {
    std::lock_guard lock(mu_);
    slots_.push_back(s);
    spdlog::debug("Timeline: insert {} t=[{},{}] f=[{:.2f},{:.2f}]MHz",
        s.task_id, s.t_start, s.t_stop, s.slice_lo_hz/1e6, s.slice_hi_hz/1e6);
}

void SpectrumTimeline::remove(const std::string& id) {
    std::lock_guard lock(mu_);
    auto it = std::remove_if(slots_.begin(), slots_.end(),
        [&](const TimeFreqSlot& s){ return s.task_id==id; });
    if (it != slots_.end()) { slots_.erase(it, slots_.end()); }
}

std::vector<TimeFreqSlot>
SpectrumTimeline::slotsOverlapping(int64_t ts, int64_t te) const {
    std::lock_guard lock(mu_);
    std::vector<TimeFreqSlot> r;
    for (auto& s : slots_)
        if (timeOverlap(ts,te,s.t_start,s.t_stop)) r.push_back(s);
    return r;
}

double SpectrumTimeline::allocatedBw(int64_t at_ms) const {
    std::lock_guard lock(mu_);
    double sum=0;
    for (auto& s : slots_)
        if (at_ms>=s.t_start && at_ms<s.t_stop)
            sum += s.slice_hi_hz - s.slice_lo_hz;
    return sum;
}

int SpectrumTimeline::usedRx(int64_t at_ms) const {
    std::lock_guard lock(mu_);
    int n=0;
    for (auto& s : slots_)
        if (at_ms>=s.t_start && at_ms<s.t_stop)
            n += (int)s.rx_channels.size();
    return n;
}

int SpectrumTimeline::slotCount() const {
    std::lock_guard lock(mu_); return (int)slots_.size();
}

std::optional<SpectrumTimeline::Window>
SpectrumTimeline::activeWindow(int64_t at_ms) const {
    std::lock_guard lock(mu_);
    for (auto& s : slots_)
        if (at_ms>=s.t_start && at_ms<s.t_stop)
            return Window{s.center_freq_hz, s.sample_rate_sps};
    return std::nullopt;
}

} // namespace sdr
