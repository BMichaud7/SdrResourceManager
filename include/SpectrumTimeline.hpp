#pragma once
// ════════════════════════════════════════════════════════════
//  SpectrumTimeline.hpp
//  Per-device 2D time×frequency reservation manager.
//  Thread-safe. canFit() is read-only (never mutates).
// ════════════════════════════════════════════════════════════
#include "sdr/Types.hpp"
#include <vector>
#include <mutex>
#include <optional>
#include <string>

namespace sdr {

struct TimeFreqSlot {
    std::string      task_id;
    int64_t          t_start         = 0;
    int64_t          t_stop          = TIME_INFINITE;
    double           center_freq_hz  = 0.0;
    double           sample_rate_sps = 0.0;
    double           slice_lo_hz     = 0.0;
    double           slice_hi_hz     = 0.0;
    std::vector<int> rx_channels;
    std::vector<int> tx_channels;
};

struct FitResult {
    bool        ok               = false;
    RejectCode  reject_code      = RejectCode::NO_DEVICE_AVAILABLE;
    std::string reject_reason;
    double      device_cf        = 0.0;
    double      device_rate      = 0.0;
    double      placed_lo        = 0.0;
    double      placed_hi        = 0.0;
    std::vector<int> avail_rx;
    std::vector<int> avail_tx;
};

class SpectrumTimeline {
public:
    // shared_lo=true  (default): enforce same CF+SR for overlapping tasks and
    //                            pack slices within the shared RF window.
    // shared_lo=false:           skip CF/SR matching; only check channel count.
    //                            Each task tunes its own channels independently.
    FitResult canFit(int64_t t_start, int64_t t_stop,
                     double  cf_hz, double bw_hz, double sr_sps,
                     int rx_count, int tx_count,
                     int max_rx, int max_tx,
                     double guard_hz,
                     bool shared_lo = true) const;

    // Returns a combined RF window if a new slice at cf_hz/bw_hz can be
    // accommodated by expanding the shared device window to cover both the
    // existing slices and the new request.  Does NOT mutate the timeline.
    struct CombineResult {
        bool             ok           = false;
        double           combined_cf  = 0;
        double           combined_sr  = 0;
        double           new_slice_lo = 0;
        double           new_slice_hi = 0;
        std::vector<int> avail_rx;
        std::string      reject_reason;
    };
    CombineResult canCombine(int64_t t_start, int64_t t_stop,
                              double cf_hz, double bw_hz,
                              int rx_count, int max_rx,
                              double guard_hz, double sr_max) const;

    // Update center_freq_hz and sample_rate_sps for all existing slots.
    // Called after the device is physically retuned to a combined window.
    void updateDeviceTune(double new_cf, double new_sr);

    void insert(const TimeFreqSlot& s);
    void remove(const std::string& task_id);

    std::vector<TimeFreqSlot> slotsOverlapping(int64_t ts, int64_t te) const;
    double allocatedBw(int64_t at_ms)   const;
    int    usedRx(int64_t at_ms)        const;
    int    slotCount()                  const;

    struct Window { double cf; double rate; };
    std::optional<Window> activeWindow(int64_t at_ms) const;

private:
    mutable std::mutex        mu_;
    std::vector<TimeFreqSlot> slots_;

    static bool timeOverlap(int64_t as,int64_t ae,int64_t bs,int64_t be){
        return (as<be)&&(bs<ae);
    }
};

} // namespace sdr
