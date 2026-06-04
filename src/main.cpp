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
#include <csignal>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <stdexcept>

namespace {
    std::atomic<bool>               g_shutdown{false};
    sdr::Controller*                g_ctrl = nullptr;

    void signalHandler(int sig) {
        spdlog::warn("Signal {} received — shutting down", sig);
        g_shutdown.store(true);
        if (g_ctrl) g_ctrl->stop();
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
    try {
        sdr::Controller ctrl(cfg);
        g_ctrl = &ctrl;
        ctrl.run();
        g_ctrl = nullptr;
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
