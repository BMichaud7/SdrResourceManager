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
 * @file UdpPortPool.hpp
 * @brief Thread-safe UDP port pool for IQ stream port allocation.
 *
 * Manages a contiguous range of UDP ports. allocateN() claims N ports atomically;
releaseAll() returns them. Used by ResourceManager to assign per-task UDP ports.
 */
#pragma once
#include <mutex>
#include <set>
#include <vector>
#include <stdexcept>

namespace sdr {

class UdpPortPool {
public:
    UdpPortPool(int first, int last) {
        if (first >= last) throw std::invalid_argument("UdpPortPool: first >= last");
        for (int p = first; p <= last; ++p) free_.insert(p);
    }

    int allocate() {
        std::lock_guard lock(mu_);
        if (free_.empty()) return -1;
        auto it = free_.begin();
        int p = *it; free_.erase(it); used_.insert(p);
        return p;
    }

    std::vector<int> allocateN(int n) {
        std::lock_guard lock(mu_);
        if ((int)free_.size() < n) return {};
        std::vector<int> out; out.reserve(static_cast<size_t>(n));
        auto it = free_.begin();
        for (int i = 0; i < n; ++i, ++it) {
            out.push_back(*it); used_.insert(*it);
        }
        for (int p : out) free_.erase(p);
        return out;
    }

    void release(int p) {
        std::lock_guard lock(mu_);
        if (used_.erase(p)) free_.insert(p);
    }

    void releaseAll(const std::vector<int>& ports) {
        std::lock_guard lock(mu_);
        for (int p : ports) if (used_.erase(p)) free_.insert(p);
    }

    int freeCount()  const { std::lock_guard l(mu_); return (int)free_.size(); }
    int usedCount()  const { std::lock_guard l(mu_); return (int)used_.size(); }

private:
    mutable std::mutex mu_;
    std::set<int>      free_, used_;
};

} // namespace sdr

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
