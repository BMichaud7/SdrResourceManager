#pragma once
#include <atomic>

// Global knobs tests set before/between operations.
// All fields are atomic so IQStreamer threads can safely read them
// while the test thread writes them.
namespace FakeSoapy {

extern std::atomic<bool>   fail_open;           // make() throws on true
extern std::atomic<bool>   fail_read;           // readStream → NOT_SUPPORTED
extern std::atomic<bool>   overflow_next;       // readStream → OVERFLOW once
extern std::atomic<int>    timeout_count;       // readStream → TIMEOUT this many times
extern std::atomic<int>    samples_per_read;    // samples returned per readStream call
extern std::atomic<float>  sample_value;        // I/Q fill value (same for I and Q)
extern std::atomic<double> reported_temp;       // readSensor("temp0")
extern std::atomic<int>    read_delay_us;       // µs sleep inside readStream (throttle)

void reset();  // restore all fields to their defaults

} // namespace FakeSoapy
