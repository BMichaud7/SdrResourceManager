#include "RadioDevice.hpp"
#include <spdlog/spdlog.h>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Errors.hpp>
#include <stdexcept>
#include <limits>
#include <cmath>

namespace sdr {

RadioDevice::RadioDevice(const DeviceConfig& cfg) : cfg_(cfg) {}
RadioDevice::~RadioDevice() { close(); }

bool RadioDevice::open() {
    std::lock_guard lock(mu_);
    if (online_) return true;
    try {
        SoapySDR::Kwargs args;
        args["driver"] = cfg_.driver;
        args["uri"]    = cfg_.uri;
        spdlog::info("[{}] Opening: driver={} uri={}", cfg_.id, cfg_.driver, cfg_.uri);
        dev_ = SoapySDR::Device::make(args);
        if (!dev_) { spdlog::error("[{}] make() returned nullptr", cfg_.id); return false; }

        // Clamp configured channel count to what the hardware actually exports.
        // Prevents accepting tasks that request more channels than exist, which
        // would crash the worker thread when readStream fails on the invalid channel.
        int hw_rx = (int)dev_->getNumChannels(SOAPY_SDR_RX);
        int hw_tx = (int)dev_->getNumChannels(SOAPY_SDR_TX);
        if (cfg_.caps.rx_channels > hw_rx) {
            spdlog::warn("[{}] config rx_channels={} > hw={}, clamping",
                         cfg_.id, cfg_.caps.rx_channels, hw_rx);
            cfg_.caps.rx_channels = hw_rx;
        }
        if (cfg_.caps.tx_channels > hw_tx) {
            spdlog::warn("[{}] config tx_channels={} > hw={}, clamping",
                         cfg_.id, cfg_.caps.tx_channels, hw_tx);
            cfg_.caps.tx_channels = hw_tx;
        }

        spdlog::info("[{}] OK hw={} drv={} rx_ch={} tx_ch={}",
                     cfg_.id, dev_->getHardwareKey(), dev_->getDriverKey(),
                     cfg_.caps.rx_channels, cfg_.caps.tx_channels);
        online_.store(true, std::memory_order_release);
        return true;
    } catch (const std::exception& ex) {
        spdlog::error("[{}] open() exception: {}", cfg_.id, ex.what());
        dev_ = nullptr; return false;
    }
}

void RadioDevice::close() {
    std::lock_guard lock(mu_);
    if (!online_) return;
    if (dev_) { SoapySDR::Device::unmake(dev_); dev_=nullptr; }
    online_.store(false, std::memory_order_release);
    spdlog::info("[{}] Closed", cfg_.id);
}

bool RadioDevice::tune(double cf_hz, double sr_sps) {
    std::lock_guard lock(mu_);
    if (!online_||!dev_) return false;
    try {
        dev_->setFrequency(SOAPY_SDR_RX, 0, cf_hz);
        dev_->setFrequency(SOAPY_SDR_TX, 0, cf_hz);
        dev_->setSampleRate(SOAPY_SDR_RX, 0, sr_sps);
        dev_->setSampleRate(SOAPY_SDR_TX, 0, sr_sps);
        current_cf_   = cf_hz;
        current_rate_ = sr_sps;
        spdlog::info("[{}] Tuned {:.3f}MHz {:.3f}MSPS", cfg_.id, cf_hz/1e6, sr_sps/1e6);
        return true;
    } catch (const std::exception& ex) {
        spdlog::error("[{}] tune() failed: {}", cfg_.id, ex.what()); return false;
    }
}

bool RadioDevice::tuneChannel(int ch, double cf_hz, double sr_sps) {
    std::lock_guard lock(mu_);
    if (!online_||!dev_) return false;
    try {
        size_t c = static_cast<size_t>(ch);
        dev_->setFrequency(SOAPY_SDR_RX, c, cf_hz);
        dev_->setSampleRate(SOAPY_SDR_RX, c, sr_sps);
        spdlog::debug("[{}] ch{} tuned {:.3f}MHz {:.3f}MSPS", cfg_.id, ch, cf_hz/1e6, sr_sps/1e6);
        return true;
    } catch (const std::exception& ex) {
        spdlog::error("[{}] tuneChannel({}) failed: {}", cfg_.id, ch, ex.what()); return false;
    }
}

bool RadioDevice::setRxGain(int ch, double gain_db, bool agc) {
    std::lock_guard lock(mu_);
    if (!online_||!dev_) return false;
    try {
        size_t c = static_cast<size_t>(ch);
        dev_->setGainMode(SOAPY_SDR_RX, c, agc);
        if (!agc) dev_->setGain(SOAPY_SDR_RX, c, gain_db);
        return true;
    } catch (...) { return false; }
}

bool RadioDevice::setTxAtten(int ch, double atten_db) {
    std::lock_guard lock(mu_);
    if (!online_||!dev_) return false;
    try { dev_->setGain(SOAPY_SDR_TX, static_cast<size_t>(ch), -atten_db); return true; }
    catch (...) { return false; }
}

SoapySDR::Stream* RadioDevice::openRxStream(const std::vector<int>& channels) {
    std::lock_guard lock(mu_);
    if (!online_||!dev_) return nullptr;
    try {
        std::vector<size_t> chans;
        for (int c : channels) chans.push_back(static_cast<size_t>(c));
        return dev_->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, chans);
    } catch (const std::exception& ex) {
        spdlog::error("[{}] openRxStream: {}", cfg_.id, ex.what()); return nullptr;
    }
}

bool RadioDevice::activateStream(SoapySDR::Stream* s) {
    if (!s||!dev_) return false;
    std::lock_guard lock(mu_);
    try { return dev_->activateStream(s)==0; }
    catch (...) { return false; }
}

void RadioDevice::deactivateStream(SoapySDR::Stream* s) {
    if (!s||!dev_) return;
    std::lock_guard lock(mu_);
    try { dev_->deactivateStream(s); } catch (...) {}
}

void RadioDevice::closeStream(SoapySDR::Stream* s) {
    if (!s||!dev_) return;
    std::lock_guard lock(mu_);
    try { dev_->closeStream(s); } catch (...) {}
}

int RadioDevice::readStream(SoapySDR::Stream* s, void** buffs, size_t numElems,
                             int& flags, long long& timeNs, long timeoutUs) {
    if (!s||!dev_) return SOAPY_SDR_NOT_SUPPORTED;
    // NOTE: readStream must NOT hold mu_ — it blocks for up to timeoutUs.
    // The device pointer is stable for the lifetime of RadioDevice.
    return dev_->readStream(s, buffs, numElems, flags, timeNs, timeoutUs);
}

double RadioDevice::getTemperature() const {
    std::lock_guard lock(mu_);
    if (!online_||!dev_) return std::numeric_limits<double>::quiet_NaN();
    try { return std::stod(dev_->readSensor("temp0")); } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

double RadioDevice::currentCF() const { std::lock_guard l(mu_); return current_cf_; }
double RadioDevice::currentRate() const { std::lock_guard l(mu_); return current_rate_; }

RadioDevice::Status RadioDevice::getStatus() const {
    Status s;
    s.device_id       = cfg_.id;
    s.driver          = cfg_.driver;
    s.uri             = cfg_.uri;
    s.coherency_group = cfg_.coherency_group;
    s.online          = online_.load(std::memory_order_acquire);
    { std::lock_guard l(mu_); s.center_freq_hz=current_cf_; s.sample_rate=current_rate_; }
    s.temperature_c   = getTemperature();
    return s;
}

} // namespace sdr
