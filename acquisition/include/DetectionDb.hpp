#pragma once
#include "Types.hpp"
#include <pqxx/pqxx>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <atomic>

namespace acq {

// Writes Detection records to PostgreSQL in background batches.
class DetectionDb {
public:
    explicit DetectionDb(const std::string& conn_str,
                         int batch_size = 100,
                         int flush_interval_ms = 500);
    ~DetectionDb();

    DetectionDb(const DetectionDb&)            = delete;
    DetectionDb& operator=(const DetectionDb&) = delete;

    void push(const Detection& d);

private:
    pqxx::connection          conn_;
    std::queue<Detection>     queue_;
    std::mutex                mutex_;
    std::condition_variable   cv_;
    std::atomic<bool>         stopped_{false};
    std::thread               thread_;
    int                       batch_size_;
    int                       flush_interval_ms_;

    void workerLoop();
    void flush(std::vector<Detection>& batch);
};

} // namespace acq
