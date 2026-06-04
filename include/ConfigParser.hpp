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
#pragma once
#include <string>
#include <vector>

namespace sdr {

// Per-device RF capability envelope. All fields are read from <capabilities>
// in devices.xml; the defaults below are used only when a field is absent from
// the XML.  They are intentionally set to cover a wide range so that an
// incomplete config still works — do NOT treat them as global hard limits.
// Override every field to match your actual hardware.
struct DeviceCapabilities {
    int    rx_channels         = 2;         // AD9361/LimeSDR MIMO default
    int    tx_channels         = 2;
    double freq_min_hz         = 70e6;      // AD9361 lower bound; RTL-SDR is 24 MHz, HackRF 1 MHz
    double freq_max_hz         = 6e9;       // AD9361/HackRF upper bound
    double bandwidth_max_hz    = 56e6;      // AD9361; LimeSDR goes to 130 MHz
    double sample_rate_max_sps = 61.44e6;   // AD9361; LimeSDR/USRP N-series go higher
    double rx_gain_min_db      = -3.0;
    double rx_gain_max_db      = 71.0;
    double tx_atten_min_db     = 0.0;
    double tx_atten_max_db     = 89.0;
};

struct DeviceConfig {
    std::string        id;
    std::string        driver;
    std::string        uri;
    std::string        label;
    std::string        streaming_source_ip;
    std::string        coherency_group;
    DeviceCapabilities caps;
    // true  = channels share one LO/RF window (e.g. AD9361, LimeSDR MIMO).
    //         All tasks on this device during overlapping windows must share CF+SR.
    // false = channels are independently tunable (e.g. RTL-SDR, HackRF, USRP B210).
    //         Different tasks may use different CFs concurrently.
    bool shared_lo = true;
    // When > 0, the hardware sample rate is locked to this value on first tune
    // and never changed again.  Tasks requesting a different rate are served at
    // this rate; the IQ stream header reports the actual rate so consumers can
    // decimate.  Eliminates the 3-4 s AD9361 BB-filter recalibration that fires
    // on every setSampleRate() call.
    double fixed_sample_rate_hz = 0.0;
};

struct BrokerConfig {
    std::string url                        = "amqp://localhost:5672";
    std::string username                   = "guest";
    std::string password                   = "guest";
    std::string request_queue              = "sdr.task.request";
    std::string response_queue             = "sdr.task.response";
    std::string status_topic               = "sdr.status";
    std::string health_topic               = "sdr.health";
    int         reconnect_interval_sec     = 5;
    int         max_reconnect_interval_sec = 60;
    int         send_queue_depth           = 512;
};

enum class RetuneConflictPolicy { REJECT_NEW, CANCEL_LOWER };

struct PolicyConfig {
    int                  max_concurrent_tasks    = 64;
    double               guard_band_hz           = 200e3;
    double               usable_bw_fraction      = 0.80;
    int64_t              default_task_timeout_ms = 3'600'000LL;
    int                  scheduler_tick_ms        = 200;
    int                  watchdog_tick_ms         = 1000;
    int                  udp_port_pool_start      = 30000;
    int                  udp_port_pool_end        = 31999;
    int                  iq_packet_samples        = 1024;
    RetuneConflictPolicy retune_conflict_policy   = RetuneConflictPolicy::REJECT_NEW;
    int                  heartbeat_interval_ms    = 30000;
};

struct AppConfig {
    BrokerConfig              broker;
    PolicyConfig              policy;
    std::vector<DeviceConfig> devices;
};

class ConfigParser {
public:
    static AppConfig parse(const std::string& xml_path);
};

} // namespace sdr
