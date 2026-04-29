#include "DetectionDb.hpp"
#include <spdlog/spdlog.h>
#include <chrono>

namespace acq {

using namespace std::chrono;

DetectionDb::DetectionDb(const std::string& conn_str, int batch_size, int flush_interval_ms)
    : conn_(conn_str), batch_size_(batch_size), flush_interval_ms_(flush_interval_ms)
{
    // Ensure table exists (idempotent — schema/init.sql should be run first)
    pqxx::work txn(conn_);
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS detections (
            id            BIGSERIAL PRIMARY KEY,
            detected_at   TIMESTAMPTZ NOT NULL,
            center_freq_hz BIGINT     NOT NULL,
            bandwidth_hz  INTEGER     NOT NULL,
            power_db      REAL        NOT NULL,
            scanner_id    TEXT        NOT NULL,
            channel       SMALLINT    NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_detections_time ON detections (detected_at);
        CREATE INDEX IF NOT EXISTS idx_detections_freq ON detections (center_freq_hz);
    )");
    txn.commit();

    thread_ = std::thread([this]{ workerLoop(); });
}

DetectionDb::~DetectionDb() {
    stopped_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void DetectionDb::push(const Detection& d) {
    {
        std::lock_guard lock(mutex_);
        queue_.push(d);
    }
    cv_.notify_one();
}

void DetectionDb::workerLoop() {
    std::vector<Detection> batch;
    batch.reserve((size_t)batch_size_);

    while (!stopped_) {
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock,
                milliseconds(flush_interval_ms_),
                [this]{ return (int)queue_.size() >= batch_size_ || stopped_; });

            while (!queue_.empty() && (int)batch.size() < batch_size_) {
                batch.push_back(queue_.front());
                queue_.pop();
            }
        }

        if (!batch.empty()) {
            try { flush(batch); }
            catch (const std::exception& e) {
                spdlog::error("[DetectionDb] flush failed: {}", e.what());
            }
            batch.clear();
        }
    }

    // Drain remaining items on shutdown
    {
        std::lock_guard lock(mutex_);
        while (!queue_.empty()) {
            batch.push_back(queue_.front());
            queue_.pop();
        }
    }
    if (!batch.empty()) {
        try { flush(batch); }
        catch (const std::exception& e) {
            spdlog::error("[DetectionDb] final flush failed: {}", e.what());
        }
    }
}

void DetectionDb::flush(std::vector<Detection>& batch) {
    pqxx::work txn(conn_);
    for (const auto& d : batch) {
        auto ms = duration_cast<milliseconds>(
            d.timestamp.time_since_epoch()).count();
        // PostgreSQL to_timestamp() takes Unix seconds as double
        txn.exec_params(
            "INSERT INTO detections "
            "(detected_at, center_freq_hz, bandwidth_hz, power_db, scanner_id, channel) "
            "VALUES (to_timestamp($1::double precision / 1000.0), $2, $3, $4, $5, $6)",
            ms,
            (long long)d.center_freq_hz,
            (int)d.bandwidth_hz,
            d.power_db,
            d.scanner_id,
            d.channel);
    }
    txn.commit();
    spdlog::debug("[DetectionDb] wrote {} detections", batch.size());
}

} // namespace acq
