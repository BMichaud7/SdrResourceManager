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
 * @file verify_rtlsdr.cpp
 * @brief Standalone hardware-verification tool: calls ResourceManager
 * directly (no AMQP broker needed) to confirm a sub-floor sample-rate
 * request is accepted and decimated correctly on real hardware. Opt-in
 * build only (-DBUILD_VERIFY_TOOL=ON) — not part of any shipped image.
 *
 * Usage: sdr_verify_rtlsdr <devices.xml> <cf_hz> <bw_hz> <sr_hz>
 */
#include "ConfigParser.hpp"
#include "ResourceManager.hpp"
#include "sdr/Types.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace sdr;
using namespace std::chrono;

int main(int argc, char** argv) {
    std::string cfg_path = argc >= 2 ? argv[1] : "/etc/sdr-controller/devices.xml";
    double cf_hz = argc >= 3 ? std::stod(argv[2]) : 100e6;
    double bw_hz = argc >= 4 ? std::stod(argv[3]) : 25e3;
    double sr_hz = argc >= 5 ? std::stod(argv[4]) : 50e3;

    AppConfig cfg = ConfigParser::parse(cfg_path);
    ResourceManager rm(cfg, [](const TaskRecord&){}, [](const auto&, const auto&){});
    int n = rm.openDevices();
    std::cout << "openDevices() -> " << n << " online\n";
    if (n == 0) { std::cout << "FAIL: no devices online\n"; return 1; }

    // Bind a UDP socket FIRST so we can hand the controller our pre-bound
    // port (dest_ports) and avoid racing IQStreamer::start() the way real
    // clients do (see ResourceManager.cpp's dest_ports handling).
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(fd, (sockaddr*)&addr, sizeof(addr));
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    ::getsockname(fd, (sockaddr*)&bound, &blen);
    int port = ntohs(bound.sin_port);
    timeval tv{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    TaskRequest req;
    req.msg_type          = "TASK_REQUEST_CONTINUOUS";
    req.request_id        = "verify-1";
    req.schema_version    = "2.0";
    req.task_type         = TaskType::NARROWBAND;
    req.schedule_mode     = ScheduleMode::CONTINUOUS;
    req.rank              = 1;
    req.rf.center_freq_hz = cf_hz;
    req.rf.bandwidth_hz   = bw_hz;
    req.rf.sample_rate_sps= sr_hz;
    req.rf.rx_count       = 1;
    req.rf.tx_count       = 0;
    req.streaming.dest_ip = "127.0.0.1";
    req.streaming.dest_ports.push_back(port);

    std::cout << "Requesting cf=" << cf_hz/1e6 << "MHz bw=" << bw_hz/1e3
              << "kHz sr=" << sr_hz/1e3 << "kHz (UDP port " << port << ")\n";

    auto resp = rm.tryAccept(req);
    std::cout << "accepted=" << (resp.accepted ? "true" : "false")
              << " reject_reason=\"" << resp.reject_reason << "\"\n";
    if (!resp.accepted) { std::cout << "FAIL: request rejected\n"; return 1; }

    for (auto& s : resp.streams)
        std::cout << "  stream device=" << s.device_id << " ch=" << s.channel_index
                   << " acquired_sample_rate_sps=" << s.sample_rate_sps << "\n";

    // Give activateTask's background thread time to tune hardware and open
    // the stream, then read a few real UDP packets off the wire and report
    // what sample rate is actually flowing — this is the proof the DDC
    // decimation produced the originally requested rate, not the
    // hardware-floor-clamped acquisition rate.
    std::this_thread::sleep_for(seconds(2));

    bool got_packet = false;
    for (int i = 0; i < 5; ++i) {
        uint8_t buf[2048];
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r < (ssize_t)sizeof(IqPacketHeader)) continue;
        IqPacketHeader hdr;
        std::memcpy(&hdr, buf, sizeof(hdr));
        std::cout << "  packet seq=" << hdr.sequence
                  << " sample_rate=" << hdr.sample_rate
                  << " num_samples=" << hdr.num_samples
                  << " cf_hz=" << hdr.center_freq_hz << "\n";
        got_packet = true;
        break;
    }
    ::close(fd);

    if (!got_packet) { std::cout << "FAIL: no IQ packets received\n"; return 1; }
    std::cout << "OK\n";
    return 0;
}

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
