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

static std::pair<int,int> bindUdp() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return {-1, 0};
    struct timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = 0;
    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { ::close(fd); return {-1, 0}; }
    socklen_t len = sizeof(addr);
    ::getsockname(fd, (sockaddr*)&addr, &len);
    return {fd, (int)ntohs(addr.sin_port)};
}

static ssize_t recvPacket(int fd, std::vector<uint8_t>& buf) {
    buf.resize(IQ_PACKET_HEADER_SIZE + 2048 * 8);
    return ::recv(fd, buf.data(), buf.size(), 0);
}

// ── SubBand consumer count tests ──────────────────────────────────────────────

TEST(Channelization, TotalConsumersCountsDestsAndSubBands) {
    FakeSoapy::reset();
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::read_delay_us.store(200);

    auto [fd1, port1] = bindUdp();
    auto [fd2, port2] = bindUdp();
    ASSERT_GE(fd1, 0); ASSERT_GE(fd2, 0);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    IQStreamer::Config cfg;
    cfg.task_id = "primary"; cfg.stream_id = "s-primary";
    cfg.channel_index = 0; cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port1;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 2e6;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(100e6);

    // Before start: add dest + subband
    streamer.addSubBand("sub-task", "s-sub", 0, "127.0.0.1", port2,
                        100.5e6, 500e3, 2e6);

    EXPECT_EQ(streamer.destCount(),   0); // start() adds primary dest
    EXPECT_EQ(streamer.subBandCount(), 1);

    streamer.start();
    EXPECT_EQ(streamer.destCount(),    1); // primary dest added by start()
    EXPECT_EQ(streamer.subBandCount(), 1);
    EXPECT_EQ(streamer.totalConsumers(), 2);

    streamer.removeSubBand("sub-task");
    EXPECT_EQ(streamer.subBandCount(),  0);
    EXPECT_EQ(streamer.totalConsumers(), 1);

    streamer.removeDest("primary");
    EXPECT_EQ(streamer.totalConsumers(), 0);

    streamer.stop();
    ::close(fd1); ::close(fd2);
}

// ── Sub-band packet delivery ──────────────────────────────────────────────────

TEST(Channelization, SubBandPacketsDelivered) {
    FakeSoapy::reset();
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::sample_value.store(1.0f);
    FakeSoapy::read_delay_us.store(200);

    auto [fd_primary, port_primary] = bindUdp();
    auto [fd_sub,     port_sub]     = bindUdp();
    ASSERT_GE(fd_primary, 0); ASSERT_GE(fd_sub, 0);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    IQStreamer::Config cfg;
    cfg.task_id = "wideband"; cfg.stream_id = "s-wide";
    cfg.channel_index = 0; cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port_primary;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 2e6;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(100e6);

    // Sub-band: decim=4 → 128/4 = 32 output samples
    streamer.addSubBand("narrow", "s-narrow", 0, "127.0.0.1", port_sub,
                        100e6, 500e3, 2e6);

    streamer.start();

    // Receive one packet on each port
    std::vector<uint8_t> buf;
    ssize_t n_primary = recvPacket(fd_primary, buf);
    ssize_t n_sub     = recvPacket(fd_sub,     buf);

    streamer.stop();
    ::close(fd_primary); ::close(fd_sub);

    EXPECT_GE(n_primary, (ssize_t)IQ_PACKET_HEADER_SIZE) << "No primary packet received";
    EXPECT_GE(n_sub,     (ssize_t)IQ_PACKET_HEADER_SIZE) << "No sub-band packet received";
}

TEST(Channelization, SubBandHeaderHasCorrectCenterFreqAndSampleRate) {
    FakeSoapy::reset();
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::sample_value.store(1.0f);
    FakeSoapy::read_delay_us.store(200);

    auto [fd_primary, port_primary] = bindUdp();
    auto [fd_sub,     port_sub]     = bindUdp();
    ASSERT_GE(fd_primary, 0); ASSERT_GE(fd_sub, 0);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    IQStreamer::Config cfg;
    cfg.task_id = "wideband2"; cfg.stream_id = "s-wide2";
    cfg.channel_index = 0; cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port_primary;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 2e6;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(200e6);

    const double sub_cf = 200.5e6;
    const double sub_sr = 500e3;
    streamer.addSubBand("narrow2", "s-narrow2", 0, "127.0.0.1", port_sub,
                        sub_cf, sub_sr, 2e6);
    streamer.start();

    // Drain primary to avoid timing issues
    std::vector<uint8_t> buf;
    recvPacket(fd_primary, buf);

    ssize_t n = recvPacket(fd_sub, buf);
    streamer.stop();
    ::close(fd_primary); ::close(fd_sub);

    ASSERT_GE(n, (ssize_t)IQ_PACKET_HEADER_SIZE) << "No sub-band packet";
    IqPacketHeader hdr{};
    std::memcpy(&hdr, buf.data(), IQ_PACKET_HEADER_SIZE);

    EXPECT_EQ(hdr.magic,          IQ_PACKET_MAGIC);
    EXPECT_EQ(hdr.center_freq_hz, (uint64_t)sub_cf);
    EXPECT_EQ(hdr.sample_rate,    (uint32_t)sub_sr);
    EXPECT_EQ(hdr.channel_index,  0);
}

TEST(Channelization, SubBandSampleCountIsDecimated) {
    FakeSoapy::reset();
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::sample_value.store(1.0f);
    FakeSoapy::read_delay_us.store(200);

    auto [fd_primary, port_primary] = bindUdp();
    auto [fd_sub,     port_sub]     = bindUdp();
    ASSERT_GE(fd_primary, 0); ASSERT_GE(fd_sub, 0);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    IQStreamer::Config cfg;
    cfg.task_id = "decimtest"; cfg.stream_id = "s-decim";
    cfg.channel_index = 0; cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port_primary;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 2e6;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(300e6);

    // decim=4: 128/4 = 32 output samples per packet
    streamer.addSubBand("narrow3", "s-narrow3", 0, "127.0.0.1", port_sub,
                        300e6, 500e3, 2e6);
    streamer.start();

    // Accumulate a few sub-band packets
    IqPacketHeader primary_hdr{}, sub_hdr{};
    std::vector<uint8_t> buf;

    // Get primary packet to know input num_samples
    ssize_t n = recvPacket(fd_primary, buf);
    ASSERT_GE(n, (ssize_t)IQ_PACKET_HEADER_SIZE);
    std::memcpy(&primary_hdr, buf.data(), IQ_PACKET_HEADER_SIZE);

    // Get sub-band packet
    n = recvPacket(fd_sub, buf);
    ASSERT_GE(n, (ssize_t)IQ_PACKET_HEADER_SIZE);
    std::memcpy(&sub_hdr, buf.data(), IQ_PACKET_HEADER_SIZE);

    streamer.stop();
    ::close(fd_primary); ::close(fd_sub);

    // Sub-band should have ~1/4 the samples of the primary
    EXPECT_GT(primary_hdr.num_samples, 0);
    EXPECT_GT(sub_hdr.num_samples, 0);
    EXPECT_EQ(sub_hdr.num_samples, primary_hdr.num_samples / 4)
        << "primary=" << primary_hdr.num_samples << " sub=" << sub_hdr.num_samples;
}

TEST(Channelization, RemoveSubBandStopsDelivery) {
    FakeSoapy::reset();
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::sample_value.store(1.0f);
    FakeSoapy::read_delay_us.store(200);

    auto [fd_primary, port_primary] = bindUdp();
    auto [fd_sub,     port_sub]     = bindUdp();
    ASSERT_GE(fd_primary, 0); ASSERT_GE(fd_sub, 0);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    IQStreamer::Config cfg;
    cfg.task_id = "stoptest"; cfg.stream_id = "s-stop";
    cfg.channel_index = 0; cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port_primary;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 2e6;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(400e6);
    streamer.addSubBand("sub-stop", "s-ss", 0, "127.0.0.1", port_sub,
                        400e6, 500e3, 2e6);
    streamer.start();

    // Drain one packet to confirm sub-band is delivering
    std::vector<uint8_t> buf;
    recvPacket(fd_primary, buf);
    ssize_t n = recvPacket(fd_sub, buf);
    EXPECT_GE(n, (ssize_t)IQ_PACKET_HEADER_SIZE) << "Sub-band not delivering initially";

    streamer.removeSubBand("sub-stop");

    // Short timeout: sub-band fd should no longer receive
    struct timeval tv{0, 150'000}; // 150 ms
    ::setsockopt(fd_sub, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    n = recvPacket(fd_sub, buf);
    EXPECT_LE(n, 0) << "Sub-band still receiving after removeSubBand";

    streamer.stop();
    ::close(fd_primary); ::close(fd_sub);
}

// Verify that converting an existing raw dest to a DDC sub-band (what
// tryRetuneCombined does when the hardware widens for a second task) keeps the
// task receiving its original narrowband data — not the wideband stream.
TEST(Channelization, WidenedDestConvertedToSubBand) {
    FakeSoapy::reset();
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::sample_value.store(1.0f);
    FakeSoapy::read_delay_us.store(200);

    auto [fd_orig, port_orig] = bindUdp();   // Task A's UDP port
    ASSERT_GE(fd_orig, 0);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    // Task A starts: device at narrow 500 kHz, raw dest
    IQStreamer::Config cfg;
    cfg.task_id       = "task-a";
    cfg.stream_id     = "s-a";
    cfg.channel_index = 0;
    cfg.dest_ip       = "127.0.0.1";
    cfg.dest_port     = port_orig;
    cfg.packet_samples= 128;
    cfg.task_start_ms = 0;
    cfg.sample_rate   = 500e3;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(100e6);
    streamer.start();

    // Drain first packet — should be at the narrow rate
    std::vector<uint8_t> buf;
    ssize_t n = recvPacket(fd_orig, buf);
    ASSERT_GE(n, (ssize_t)IQ_PACKET_HEADER_SIZE);
    IqPacketHeader hdr{};
    std::memcpy(&hdr, buf.data(), IQ_PACKET_HEADER_SIZE);
    EXPECT_EQ(hdr.sample_rate, (uint32_t)500e3) << "Initial stream should be at narrow SR";

    // Simulate tryRetuneCombined widening the hardware for Task B:
    //   - new center = 100.25 MHz (covers both 100 MHz and 100.5 MHz)
    //   - new wideband SR = 2 MHz (decim=4 for Task A's 500 kHz output)
    streamer.pauseForRetune(4);
    streamer.updateCenterFreq(100.25e6);
    streamer.updateSampleRate(2e6);

    // Now apply the upgrade: remove raw dest, re-add as DDC sub-band
    streamer.removeDest("task-a");
    streamer.addSubBand("task-a", "s-a", 0, "127.0.0.1", port_orig,
                        100e6, 500e3, 2e6);

    // Task A should now receive narrowband packets on the same port
    n = recvPacket(fd_orig, buf);
    ASSERT_GE(n, (ssize_t)IQ_PACKET_HEADER_SIZE) << "No packet after conversion";
    std::memcpy(&hdr, buf.data(), IQ_PACKET_HEADER_SIZE);

    EXPECT_EQ(hdr.center_freq_hz, (uint64_t)100e6)
        << "Sub-band should report Task A's original CF, not wideband CF";
    EXPECT_EQ(hdr.sample_rate, (uint32_t)500e3)
        << "Sub-band should report Task A's requested SR, not wideband SR";
    EXPECT_EQ(hdr.num_samples, 128u / 4u)
        << "Sub-band should deliver decimated samples (decim=4)";

    streamer.stop();
    ::close(fd_orig);
}

TEST(Channelization, StopWithSubBandDoesNotCrash) {
    FakeSoapy::reset();
    FakeSoapy::samples_per_read.store(128);
    FakeSoapy::read_delay_us.store(500);

    auto [fd_primary, port_primary] = bindUdp();
    auto [fd_sub,     port_sub]     = bindUdp();
    ASSERT_GE(fd_primary, 0); ASSERT_GE(fd_sub, 0);

    FakeSoapyDevice dev;
    SoapySDR::Stream* stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, {});

    IQStreamer::Config cfg;
    cfg.task_id = "crashtest"; cfg.stream_id = "s-crash";
    cfg.channel_index = 0; cfg.dest_ip = "127.0.0.1"; cfg.dest_port = port_primary;
    cfg.packet_samples = 128; cfg.task_start_ms = 0; cfg.sample_rate = 2e6;

    IQStreamer streamer(cfg, &dev, stream, nullptr);
    streamer.updateCenterFreq(500e6);
    streamer.addSubBand("sub-crash", "s-sc", 0, "127.0.0.1", port_sub,
                        500e6, 500e3, 2e6);
    streamer.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    streamer.stop(); // must not crash
    streamer.stop(); // idempotent

    EXPECT_FALSE(streamer.isRunning());
    ::close(fd_primary); ::close(fd_sub);
}
