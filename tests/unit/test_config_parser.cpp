#include <gtest/gtest.h>
#include "ConfigParser.hpp"
#include <fstream>
#include <stdexcept>

using namespace sdr;

static std::string writeTmp(const std::string& xml) {
    const std::string path = "/tmp/sdr_test_config.xml";
    std::ofstream f(path);
    f << xml;
    return path;
}

static const std::string kValidXml = R"xml(<?xml version="1.0"?>
<sdr_controller version="2.0">
  <broker>
    <url>amqp://broker:5672</url>
    <username>user1</username>
    <password>pass1</password>
    <request_queue>sdr.task.request</request_queue>
    <response_queue>sdr.task.response</response_queue>
    <status_topic>sdr.status</status_topic>
    <health_topic>sdr.health</health_topic>
    <reconnect_interval_sec>3</reconnect_interval_sec>
    <max_reconnect_interval_sec>30</max_reconnect_interval_sec>
    <send_queue_depth>256</send_queue_depth>
  </broker>
  <policy>
    <max_concurrent_tasks>32</max_concurrent_tasks>
    <guard_band_hz>100000</guard_band_hz>
    <usable_bw_fraction>0.75</usable_bw_fraction>
    <default_task_timeout_ms>7200000</default_task_timeout_ms>
    <scheduler_tick_ms>100</scheduler_tick_ms>
    <watchdog_tick_ms>250</watchdog_tick_ms>
    <udp_port_pool_start>40000</udp_port_pool_start>
    <udp_port_pool_end>40099</udp_port_pool_end>
    <iq_packet_samples>128</iq_packet_samples>
    <retune_conflict_policy>CANCEL_LOWER</retune_conflict_policy>
    <heartbeat_interval_ms>5000</heartbeat_interval_ms>
  </policy>
  <devices>
    <device id="dev-0">
      <driver>fake</driver>
      <uri>fake://0</uri>
      <label>Fake Board 0</label>
      <streaming_source_ip>192.168.1.10</streaming_source_ip>
      <coherency_group>group-a</coherency_group>
      <capabilities>
        <rx_channels>2</rx_channels>
        <tx_channels>1</tx_channels>
        <freq_min_hz>70000000</freq_min_hz>
        <freq_max_hz>6000000000</freq_max_hz>
        <bandwidth_max_hz>40000000</bandwidth_max_hz>
        <sample_rate_max_sps>30720000</sample_rate_max_sps>
        <rx_gain_min_db>0</rx_gain_min_db>
        <rx_gain_max_db>60</rx_gain_max_db>
        <tx_atten_min_db>0</tx_atten_min_db>
        <tx_atten_max_db>80</tx_atten_max_db>
      </capabilities>
    </device>
    <device id="dev-1">
      <driver>fake</driver>
      <uri>fake://1</uri>
      <label>Fake Board 1</label>
      <streaming_source_ip>192.168.1.11</streaming_source_ip>
      <coherency_group>group-a</coherency_group>
      <capabilities>
        <rx_channels>2</rx_channels>
        <tx_channels>2</tx_channels>
        <freq_min_hz>70000000</freq_min_hz>
        <freq_max_hz>6000000000</freq_max_hz>
        <bandwidth_max_hz>56000000</bandwidth_max_hz>
        <sample_rate_max_sps>61440000</sample_rate_max_sps>
        <rx_gain_min_db>-3</rx_gain_min_db>
        <rx_gain_max_db>71</rx_gain_max_db>
        <tx_atten_min_db>0</tx_atten_min_db>
        <tx_atten_max_db>89</tx_atten_max_db>
      </capabilities>
    </device>
  </devices>
</sdr_controller>
)xml";

TEST(ConfigParser, ParsesValidXml_Broker) {
    auto cfg = ConfigParser::parse(writeTmp(kValidXml));
    EXPECT_EQ(cfg.broker.url,      "amqp://broker:5672");
    EXPECT_EQ(cfg.broker.username, "user1");
    EXPECT_EQ(cfg.broker.password, "pass1");
    EXPECT_EQ(cfg.broker.reconnect_interval_sec,     3);
    EXPECT_EQ(cfg.broker.max_reconnect_interval_sec, 30);
    EXPECT_EQ(cfg.broker.send_queue_depth,           256);
}

TEST(ConfigParser, ParsesValidXml_Policy) {
    auto cfg = ConfigParser::parse(writeTmp(kValidXml));
    EXPECT_EQ(cfg.policy.max_concurrent_tasks, 32);
    EXPECT_DOUBLE_EQ(cfg.policy.guard_band_hz,      100e3);
    EXPECT_DOUBLE_EQ(cfg.policy.usable_bw_fraction, 0.75);
    EXPECT_EQ(cfg.policy.scheduler_tick_ms,   100);
    EXPECT_EQ(cfg.policy.udp_port_pool_start, 40000);
    EXPECT_EQ(cfg.policy.udp_port_pool_end,   40099);
    EXPECT_EQ(cfg.policy.iq_packet_samples,   128);
    EXPECT_EQ(cfg.policy.retune_conflict_policy, RetuneConflictPolicy::CANCEL_LOWER);
    EXPECT_EQ(cfg.policy.heartbeat_interval_ms, 5000);
}

TEST(ConfigParser, ParsesValidXml_Devices) {
    auto cfg = ConfigParser::parse(writeTmp(kValidXml));
    ASSERT_EQ(cfg.devices.size(), 2u);

    auto& d0 = cfg.devices[0];
    EXPECT_EQ(d0.id,                  "dev-0");
    EXPECT_EQ(d0.driver,              "fake");
    EXPECT_EQ(d0.uri,                 "fake://0");
    EXPECT_EQ(d0.label,               "Fake Board 0");
    EXPECT_EQ(d0.streaming_source_ip, "192.168.1.10");
    EXPECT_EQ(d0.coherency_group,     "group-a");
    EXPECT_EQ(d0.caps.rx_channels,    2);
    EXPECT_EQ(d0.caps.tx_channels,    1);
    EXPECT_DOUBLE_EQ(d0.caps.bandwidth_max_hz,    40e6);
    EXPECT_DOUBLE_EQ(d0.caps.sample_rate_max_sps, 30.72e6);

    auto& d1 = cfg.devices[1];
    EXPECT_EQ(d1.id,               "dev-1");
    EXPECT_EQ(d1.caps.rx_channels, 2);
    EXPECT_EQ(d1.caps.tx_channels, 2);
    EXPECT_DOUBLE_EQ(d1.caps.rx_gain_min_db, -3.0);
    EXPECT_DOUBLE_EQ(d1.caps.rx_gain_max_db,  71.0);
}

TEST(ConfigParser, MissingRootElementThrows) {
    std::string xml = R"xml(<?xml version="1.0"?><wrong_root/>)xml";
    EXPECT_THROW(ConfigParser::parse(writeTmp(xml)), std::runtime_error);
}

TEST(ConfigParser, EmptyDeviceListThrows) {
    std::string xml = R"xml(<?xml version="1.0"?>
<sdr_controller version="2.0">
  <broker>
    <url>amqp://x:5672</url><username>u</username><password>p</password>
    <request_queue>q</request_queue><response_queue>r</response_queue>
    <status_topic>s</status_topic><health_topic>h</health_topic>
  </broker>
  <policy>
    <udp_port_pool_start>1000</udp_port_pool_start>
    <udp_port_pool_end>2000</udp_port_pool_end>
  </policy>
  <devices/>
</sdr_controller>)xml";
    EXPECT_THROW(ConfigParser::parse(writeTmp(xml)), std::runtime_error);
}

TEST(ConfigParser, FileNotFoundThrows) {
    EXPECT_THROW(ConfigParser::parse("/nonexistent/path/config.xml"),
                 std::runtime_error);
}

// ── Edge cases ────────────────────────────────────────────────────────────────

static const std::string kSingleDeviceXml = R"xml(<?xml version="1.0"?>
<sdr_controller version="2.0">
  <broker>
    <url>amqp://broker:5672</url>
    <username>u</username>
    <password>p</password>
    <request_queue>sdr.task.request</request_queue>
    <response_queue>sdr.task.response</response_queue>
    <status_topic>sdr.status</status_topic>
    <health_topic>sdr.health</health_topic>
    <reconnect_interval_sec>5</reconnect_interval_sec>
    <max_reconnect_interval_sec>60</max_reconnect_interval_sec>
    <send_queue_depth>128</send_queue_depth>
  </broker>
  <policy>
    <udp_port_pool_start>30000</udp_port_pool_start>
    <udp_port_pool_end>30099</udp_port_pool_end>
    <retune_conflict_policy>REJECT_NEW</retune_conflict_policy>
  </policy>
  <devices>
    <device id="dev-only">
      <driver>null</driver>
      <uri>null</uri>
      <label>Only device</label>
      <streaming_source_ip>127.0.0.1</streaming_source_ip>
      <capabilities>
        <rx_channels>1</rx_channels>
        <tx_channels>0</tx_channels>
        <freq_min_hz>70000000</freq_min_hz>
        <freq_max_hz>6000000000</freq_max_hz>
        <bandwidth_max_hz>10000000</bandwidth_max_hz>
        <sample_rate_max_sps>10000000</sample_rate_max_sps>
        <rx_gain_min_db>0</rx_gain_min_db>
        <rx_gain_max_db>50</rx_gain_max_db>
        <tx_atten_min_db>0</tx_atten_min_db>
        <tx_atten_max_db>0</tx_atten_max_db>
      </capabilities>
    </device>
  </devices>
</sdr_controller>
)xml";

TEST(ConfigParser, RejectNewPolicyParsed) {
    auto cfg = ConfigParser::parse(writeTmp(kSingleDeviceXml));
    EXPECT_EQ(cfg.policy.retune_conflict_policy, RetuneConflictPolicy::REJECT_NEW);
}

TEST(ConfigParser, SingleDeviceParsed) {
    auto cfg = ConfigParser::parse(writeTmp(kSingleDeviceXml));
    ASSERT_EQ(cfg.devices.size(), 1u);
    EXPECT_EQ(cfg.devices[0].id,     "dev-only");
    EXPECT_EQ(cfg.devices[0].driver, "null");
    EXPECT_EQ(cfg.devices[0].caps.rx_channels, 1);
    EXPECT_EQ(cfg.devices[0].caps.tx_channels, 0);
}

TEST(ConfigParser, DeviceWithoutCoherencyGroupHasEmptyGroup) {
    // kSingleDeviceXml has no <coherency_group> element.
    auto cfg = ConfigParser::parse(writeTmp(kSingleDeviceXml));
    ASSERT_EQ(cfg.devices.size(), 1u);
    EXPECT_TRUE(cfg.devices[0].coherency_group.empty());
}

TEST(ConfigParser, BrokerQueueNamesPreserved) {
    auto cfg = ConfigParser::parse(writeTmp(kValidXml));
    EXPECT_EQ(cfg.broker.request_queue,  "sdr.task.request");
    EXPECT_EQ(cfg.broker.response_queue, "sdr.task.response");
    EXPECT_EQ(cfg.broker.status_topic,   "sdr.status");
    EXPECT_EQ(cfg.broker.health_topic,   "sdr.health");
}

TEST(ConfigParser, DefaultTaskTimeoutMsIsPositive) {
    auto cfg = ConfigParser::parse(writeTmp(kValidXml));
    EXPECT_GT(cfg.policy.default_task_timeout_ms, 0);
}

TEST(ConfigParser, DeviceFreqRangeParsed) {
    auto cfg = ConfigParser::parse(writeTmp(kValidXml));
    ASSERT_GE(cfg.devices.size(), 1u);
    EXPECT_DOUBLE_EQ(cfg.devices[0].caps.freq_min_hz,  70e6);
    EXPECT_DOUBLE_EQ(cfg.devices[0].caps.freq_max_hz,  6000e6);
}
