#pragma once
#include <chrono>
#include <string>
#include <cstdint>

namespace acq {

inline constexpr const char* SCHEMA_VERSION = "1.0";

struct Detection {
    std::chrono::system_clock::time_point timestamp;
    uint64_t center_freq_hz{0};
    uint32_t bandwidth_hz{0};
    float    power_db{0.f};
    std::string scanner_id;
    int      channel{0};
};

} // namespace acq
