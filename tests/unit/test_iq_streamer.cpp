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
#include <gtest/gtest.h>
#include "IQStreamer.hpp"
#include "sdr/Types.hpp"
#include "FakeSoapyDevice.hpp"
#include <SoapySDR/Formats.hpp>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <cstring>

using namespace sdr;

// Bind a UDP socket on an ephemeral port; return {fd, port}.
// Returns fd=-1 on failure; caller must ASSERT_GE(fd, 0).
static std::pair<int,int> bindUdp() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return {-1, 0};

    struct timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = 0;
    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        ::close(fd);
        return {-1, 0};
    }
    socklen_t len = sizeof(addr);
    ::getsockname(fd, (sockaddr*)&addr, &len);
    return {fd, (int)ntohs(addr.sin_port)};
}

static ssize_t recvPacket(int fd, std::vector<uint8_t>& buf) {
    buf.resize(IQ_PACKET_HEADER_SIZE + 1024 * 8);
    return ::recv(fd, buf.data(), buf.size(), 0);
}

TEST(IQStreamer, FirstPacketHasCorrectMagicAndFirstPacketFlag) {
    FakeSoapy::reset();
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::read_delay_us.store(100);

    auto [fd, port] = bindUdp();
    ASSERT_GE(fd, 0) << "Failed to create/bind UDP socket";

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    IQStreamer::Config cfg;
    cfg.task_id       = "test-task";
    cfg.stream_id     = "test-stream";
    cfg.channel_index = 0;
    cfg.dest_ip       = "127.0.0.1";
    cfg.dest_port     = port;
    cfg.packet_samples= 128;
    cfg.task_start_ms = 0;
    cfg.sample_rate   = 10e6;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(915e6);
    streamer.start();

    std::vector<uint8_t> buf;
    ssize_t n = recvPacket(fd, buf);
    streamer.stop();
    ::close(fd);

    ASSERT_GE(n, (ssize_t)IQ_PACKET_HEADER_SIZE);
    IqPacketHeader hdr;
    std::memcpy(&hdr, buf.data(), IQ_PACKET_HEADER_SIZE);

    EXPECT_EQ(hdr.magic,       IQ_PACKET_MAGIC);
    EXPECT_EQ(hdr.sequence,    0u);
    EXPECT_NE(hdr.flags & IQ_FLAG_FIRST_PACKET, 0);
    EXPECT_EQ(hdr.channel_index, 0);
    EXPECT_EQ(hdr.num_samples,   128);
    EXPECT_EQ((uint32_t)hdr.sample_rate, (uint32_t)10e6);
    EXPECT_EQ(hdr.center_freq_hz, (uint64_t)915e6);
}

TEST(IQStreamer, SequenceNumbersIncrement) {
    FakeSoapy::reset();
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::read_delay_us.store(100);

    auto [fd, port] = bindUdp();
    ASSERT_GE(fd, 0) << "Failed to create/bind UDP socket";

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    IQStreamer::Config cfg;
    cfg.task_id = "test-seq"; cfg.stream_id = "test-stream-seq";
    cfg.channel_index = 0; cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 10e6;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(915e6);
    streamer.start();

    IqPacketHeader prev{};
    bool first = true;
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> buf;
        ssize_t n = recvPacket(fd, buf);
        if (n < (ssize_t)IQ_PACKET_HEADER_SIZE) break;
        IqPacketHeader hdr;
        std::memcpy(&hdr, buf.data(), IQ_PACKET_HEADER_SIZE);
        if (!first)
            EXPECT_EQ(hdr.sequence, prev.sequence + 1);
        prev  = hdr;
        first = false;
    }
    streamer.stop();
    ::close(fd);
    EXPECT_FALSE(first) << "Did not receive any packets";
}

TEST(IQStreamer, DwellChangeFlagSetAfterUpdateCenterFreq) {
    FakeSoapy::reset();
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::read_delay_us.store(100);

    auto [fd, port] = bindUdp();
    ASSERT_GE(fd, 0) << "Failed to create/bind UDP socket";

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    IQStreamer::Config cfg;
    cfg.task_id = "test-dwell"; cfg.stream_id = "test-stream-dwell";
    cfg.channel_index = 0; cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 10e6;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(915e6);
    streamer.start();

    std::vector<uint8_t> buf;
    recvPacket(fd, buf);  // drain FIRST_PACKET

    streamer.updateCenterFreq(2400e6);

    bool found = false;
    for (int i = 0; i < 10 && !found; ++i) {
        ssize_t n = recvPacket(fd, buf);
        if (n < (ssize_t)IQ_PACKET_HEADER_SIZE) break;
        IqPacketHeader hdr;
        std::memcpy(&hdr, buf.data(), IQ_PACKET_HEADER_SIZE);
        if (hdr.flags & IQ_FLAG_DWELL_CHANGE) {
            found = true;
            EXPECT_EQ(hdr.center_freq_hz, (uint64_t)2400e6);
        }
    }
    streamer.stop();
    ::close(fd);
    EXPECT_TRUE(found) << "DWELL_CHANGE flag never appeared";
}

TEST(IQStreamer, MetricsAccumulatePacketsAndSamples) {
    FakeSoapy::reset();
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::read_delay_us.store(50);

    auto [fd, port] = bindUdp();
    ASSERT_GE(fd, 0) << "Failed to create/bind UDP socket";

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    IQStreamer::Config cfg;
    cfg.task_id = "test-metrics"; cfg.stream_id = "test-stream-metrics";
    cfg.channel_index = 0; cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 10e6;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(915e6);
    streamer.start();

    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> buf;
        recvPacket(fd, buf);
    }
    streamer.stop();
    ::close(fd);

    auto m = streamer.getMetrics();
    EXPECT_GE(m.packets_sent,  3);
    EXPECT_GE(m.samples_total, 3 * 128);
    EXPECT_EQ(m.overflows,     0);
    EXPECT_EQ(m.stream_id,     "test-stream-metrics");
    EXPECT_EQ(m.channel_index, 0);
}

TEST(IQStreamer, StopIsIdempotent) {
    FakeSoapy::reset();
    FakeSoapy::read_delay_us.store(1000);

    auto [fd, port] = bindUdp();
    ASSERT_GE(fd, 0) << "Failed to create/bind UDP socket";

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    IQStreamer::Config cfg;
    cfg.task_id = "test-stop"; cfg.stream_id = "s"; cfg.channel_index = 0;
    cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 10e6;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(915e6);
    streamer.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    streamer.stop();
    streamer.stop();  // must not crash

    ::close(fd);
    EXPECT_FALSE(streamer.isRunning());
}

// ── Error-path tests ─────────────────────────────────────────────────────────

TEST(IQStreamer, OverflowFlagAppearsInPacketAndMetrics) {
    FakeSoapy::reset();
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::read_delay_us.store(100);

    auto [fd, port] = bindUdp();
    ASSERT_GE(fd, 0) << "Failed to create/bind UDP socket";

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    IQStreamer::Config cfg;
    cfg.task_id = "test-overflow"; cfg.stream_id = "s-overflow";
    cfg.channel_index = 0; cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 10e6;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(915e6);
    streamer.start();

    // Drain the FIRST_PACKET, then inject one overflow.
    std::vector<uint8_t> buf;
    recvPacket(fd, buf);
    FakeSoapy::overflow_next.store(true);

    bool overflow_seen = false;
    for (int i = 0; i < 20 && !overflow_seen; ++i) {
        ssize_t n = recvPacket(fd, buf);
        if (n < (ssize_t)IQ_PACKET_HEADER_SIZE) break;
        IqPacketHeader hdr;
        std::memcpy(&hdr, buf.data(), IQ_PACKET_HEADER_SIZE);
        if (hdr.flags & IQ_FLAG_OVERFLOW) overflow_seen = true;
    }
    streamer.stop();
    ::close(fd);

    EXPECT_TRUE(overflow_seen);
    EXPECT_GE(streamer.getMetrics().overflows, 1u);
}

TEST(IQStreamer, OnErrorCalledAfter20ConsecutiveReadFailures) {
    FakeSoapy::reset();
    FakeSoapy::fail_read.store(true);   // every readStream → SOAPY_SDR_NOT_SUPPORTED
    FakeSoapy::read_delay_us.store(50); // fast so 20 errors accumulate quickly

    auto [fd, port] = bindUdp();
    ASSERT_GE(fd, 0) << "Failed to create/bind UDP socket";

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    std::atomic<bool> error_cb_called{false};
    std::string       error_task_id;

    IQStreamer::Config cfg;
    cfg.task_id = "test-error"; cfg.stream_id = "s-error";
    cfg.channel_index = 0; cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 10e6;

    IQStreamer streamer(cfg, &dev, stream,
        [&](const std::string& id, const std::string&) {
            error_task_id = id;
            error_cb_called.store(true);
        });
    streamer.updateCenterFreq(915e6);
    streamer.start();

    for (int i = 0; i < 200 && !error_cb_called.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    streamer.stop();
    ::close(fd);

    EXPECT_TRUE(error_cb_called.load());
    EXPECT_EQ(error_task_id, "test-error");
}

TEST(IQStreamer, TimeoutDoesNotCountTowardErrorLimit) {
    FakeSoapy::reset();
    FakeSoapy::timeout_count.store(10); // 10 timeouts, then normal reads
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::read_delay_us.store(100);

    auto [fd, port] = bindUdp();
    ASSERT_GE(fd, 0) << "Failed to create/bind UDP socket";

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    std::atomic<bool> error_cb_called{false};

    IQStreamer::Config cfg;
    cfg.task_id = "test-timeout"; cfg.stream_id = "s-timeout";
    cfg.channel_index = 0; cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 10e6;

    IQStreamer streamer(cfg, &dev, stream,
        [&](const std::string&, const std::string&) { error_cb_called.store(true); });
    streamer.updateCenterFreq(915e6);
    streamer.start();

    // Receive packets that arrive after the timeouts clear
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> buf;
        recvPacket(fd, buf);
    }
    streamer.stop();
    ::close(fd);

    EXPECT_FALSE(error_cb_called.load());
}

// ── Multi-destination (multicast) tests ──────────────────────────────────────

TEST(IQStreamer, MulticastDeliversToTwoDestinations) {
    FakeSoapy::reset();
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::read_delay_us.store(100);

    auto [fd1, port1] = bindUdp();
    auto [fd2, port2] = bindUdp();
    ASSERT_GE(fd1, 0); ASSERT_GE(fd2, 0);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    // Primary dest via Config
    IQStreamer::Config cfg;
    cfg.task_id = "task-1"; cfg.stream_id = "stream-1";
    cfg.channel_index = 0; cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port1;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 10e6;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(915e6);
    // Add second dest before start (order doesn't matter)
    streamer.addDest("task-2", "stream-2", "127.0.0.1", port2);
    streamer.start();

    EXPECT_EQ(streamer.destCount(), 2);

    std::vector<uint8_t> buf;
    ssize_t n1 = recvPacket(fd1, buf);
    ssize_t n2 = recvPacket(fd2, buf);

    streamer.stop();
    ::close(fd1); ::close(fd2);

    EXPECT_GE(n1, (ssize_t)IQ_PACKET_HEADER_SIZE);
    EXPECT_GE(n2, (ssize_t)IQ_PACKET_HEADER_SIZE);
    EXPECT_EQ(streamer.destCount(), 0); // stop clears all dests
}

TEST(IQStreamer, RemoveDestStopsDeliveryToThatClient) {
    FakeSoapy::reset();
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::read_delay_us.store(100);

    auto [fd1, port1] = bindUdp();
    auto [fd2, port2] = bindUdp();
    ASSERT_GE(fd1, 0); ASSERT_GE(fd2, 0);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    IQStreamer::Config cfg;
    cfg.task_id = "task-a"; cfg.stream_id = "s-a";
    cfg.channel_index = 0; cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port1;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 10e6;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(915e6);
    streamer.addDest("task-b", "s-b", "127.0.0.1", port2);
    streamer.start();

    // Drain a packet on each dest to confirm both are live
    std::vector<uint8_t> buf;
    recvPacket(fd1, buf);
    recvPacket(fd2, buf);

    // Remove task-b
    int remaining = streamer.removeDest("task-b");
    EXPECT_EQ(remaining, 1);

    // fd2 should now time out (no more packets)
    struct timeval tv{0, 100'000}; // 100 ms timeout
    ::setsockopt(fd2, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recvPacket(fd2, buf);
    EXPECT_LE(n, 0) << "task-b still receiving after removeDest";

    streamer.stop();
    ::close(fd1); ::close(fd2);
}

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
