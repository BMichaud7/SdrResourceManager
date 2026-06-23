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
#include "ConfigParser.hpp"
#include "Controller.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
    // Signal handlers may only call async-signal-safe functions. A plain
    // atomic store is safe; spdlog::warn() (heap alloc + internal locking)
    // and Controller::stop() (joins threads, takes mutexes) are NOT.
    // Calling them directly from the handler — as this used to do — can
    // self-deadlock if the interrupted thread already holds one of those
    // locks (e.g. mid-malloc during routine logging), leaving the process
    // unresponsive to SIGTERM and requiring a hard SIGKILL. Confirmed live
    // on the Pi: `podman stop` on sdr_controller against a real RTL-SDR
    // consistently hung past the stop timeout and needed SIGKILL.
    std::atomic<bool> g_shutdown_requested{false};

    void signalHandler(int) {
        g_shutdown_requested.store(true, std::memory_order_relaxed);
    }
}

int main(int argc, char* argv[]) {
    // ── Logging setup ─────────────────────────────────────────────────
    auto console = spdlog::stdout_color_mt("sdr");
    spdlog::set_default_logger(console);

    const char* log_level = std::getenv("SDR_LOG_LEVEL");
    if (log_level) {
        auto lvl = spdlog::level::from_str(log_level);
        spdlog::set_level(lvl);
    } else {
        spdlog::set_level(spdlog::level::info);
    }
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

    // ── Config path ───────────────────────────────────────────────────
    std::string config_path = "/etc/sdr-controller/devices.xml";
    if (argc >= 2) config_path = argv[1];
    const char* env_cfg = std::getenv("SDR_CONFIG_PATH");
    if (env_cfg) config_path = env_cfg;

    spdlog::info("SDR Radio Resource Task Manager v{}", sdr::APP_VERSION);
    spdlog::info("Config: {}", config_path);

    // ── Signal handlers ───────────────────────────────────────────────
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);

    // ── Load config ───────────────────────────────────────────────────
    sdr::AppConfig cfg;
    try {
        cfg = sdr::ConfigParser::parse(config_path);
        spdlog::info("Loaded config: {} devices", cfg.devices.size());
    } catch (const std::exception& ex) {
        spdlog::critical("Config parse failed: {}", ex.what());
        return EXIT_FAILURE;
    }

    // ── Run controller ────────────────────────────────────────────────
    // ctrl.run() blocks the calling thread, so it runs on its own thread
    // here, leaving main() free to poll g_shutdown_requested and call
    // ctrl.stop() from ordinary (non-signal) context once it's set.
    try {
        sdr::Controller ctrl(cfg);
        std::thread runner([&ctrl] { ctrl.run(); });

        while (!g_shutdown_requested.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        spdlog::warn("Signal received — shutting down");
        ctrl.stop();
        runner.join();
    } catch (const std::exception& ex) {
        spdlog::critical("Controller fatal error: {}", ex.what());
        return EXIT_FAILURE;
    }

    spdlog::info("Clean shutdown complete");
    return EXIT_SUCCESS;
}

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
