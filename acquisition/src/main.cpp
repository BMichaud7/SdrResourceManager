// ════════════════════════════════════════════════════════════════════════
//  sdr_acquisition — Continuous Spectrum Scanner
//
//  Sweeps a configurable frequency range, detects RF energy above a
//  per-dwell noise floor estimate, records detections to PostgreSQL, and
//  publishes them over AMQP so downstream apps can react in real time.
//
//  Usage:
//    sdr_acquisition [config.xml]
//    SDR_ACQ_CONFIG=/etc/sdr-acquisition/scanner.xml  sdr_acquisition
//    SDR_LOG_LEVEL=debug sdr_acquisition config/scanner.xml
// ════════════════════════════════════════════════════════════════════════
#include "SweepConfig.hpp"
#include "SpectrumScanner.hpp"
#include "DetectionDb.hpp"
#include "AmqpPublisher.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <csignal>
#include <atomic>
#include <memory>
#include <string>

static std::atomic<bool> g_running{true};

static void sigHandler(int) { g_running = false; }

static void setupLogging() {
    const char* lvl = std::getenv("SDR_LOG_LEVEL");
    if (!lvl) lvl = "info";
    std::string s(lvl);
    spdlog::level::level_enum level = spdlog::level::info;
    if      (s == "trace") level = spdlog::level::trace;
    else if (s == "debug") level = spdlog::level::debug;
    else if (s == "warn")  level = spdlog::level::warn;
    else if (s == "error") level = spdlog::level::err;
    spdlog::set_level(level);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
}

int main(int argc, char* argv[]) {
    setupLogging();

    // Resolve config path: argv[1] → env → default
    std::string config_path = "/etc/sdr-acquisition/scanner.xml";
    if (argc > 1) {
        config_path = argv[1];
    } else if (const char* env = std::getenv("SDR_ACQ_CONFIG")) {
        config_path = env;
    }

    acq::SweepConfig cfg;
    try {
        cfg = acq::SweepConfig::from_file(config_path);
    } catch (const std::exception& e) {
        spdlog::error("Config error: {}", e.what());
        return 1;
    }

    spdlog::info("SDR Acquisition — scanner_id={}", cfg.scanner_id);
    spdlog::info("Sweep: {:.3f}–{:.3f} MHz  rx_channels={}  shared_lo={}",
        cfg.sweep.start_hz / 1e6, cfg.sweep.stop_hz / 1e6,
        cfg.device.rx_channels, cfg.device.shared_lo);

    std::signal(SIGINT,  sigHandler);
    std::signal(SIGTERM, sigHandler);

    // ── Start AMQP publisher ────────────────────────────────────────────────
    std::unique_ptr<acq::AmqpPublisher> amqp;
    bool amqp_ok = false;
    try {
        amqp = std::make_unique<acq::AmqpPublisher>(
            cfg.amqp.url, cfg.amqp.detection_topic, cfg.scanner_id);
        amqp->start();
        amqp_ok = true;
    } catch (const std::exception& e) {
        spdlog::warn("AMQP publisher failed to start: {} — continuing without AMQP", e.what());
    }

    // ── Start DB writer ─────────────────────────────────────────────────────
    std::unique_ptr<acq::DetectionDb> db;
    bool db_ok = false;
    try {
        db = std::make_unique<acq::DetectionDb>(cfg.db.connection_string());
        db_ok = true;
    } catch (const std::exception& e) {
        spdlog::warn("PostgreSQL connection failed: {} — continuing without DB", e.what());
    }

    if (!amqp_ok && !db_ok) {
        spdlog::error("Neither AMQP nor DB is available — check config and exit");
        return 1;
    }

    // ── Wire detection callback ─────────────────────────────────────────────
    auto on_detection = [&](const acq::Detection& d) {
        if (db_ok)   db->push(d);
        if (amqp_ok) amqp->publish(d);
    };

    // ── Start scanner ───────────────────────────────────────────────────────
    acq::SpectrumScanner scanner(cfg, on_detection);
    scanner.start();

    spdlog::info("Running — Ctrl+C to stop");
    while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    spdlog::info("Stopping...");
    scanner.stop();
    amqp.reset();  // stop AMQP before DB so no messages are lost
    db.reset();

    spdlog::info("Shutdown complete");
    return 0;
}
