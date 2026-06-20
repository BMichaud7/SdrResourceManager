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
        // SoapyRemote uses key "remote" for the host:port; all other drivers
        // use "uri" (libiio convention: "ip:192.168.1.x", "usb:X.Y.Z", etc.)
        if (cfg_.driver == "remote") {
            args["remote"] = cfg_.uri;
        } else if (!cfg_.uri.empty()) {
            args["uri"] = cfg_.uri;
        } else {
            // No uri configured — ask the driver to find itself instead of us
            // having to hand-maintain a device's address in devices.xml.
            // SoapyPlutoSDR's registered find function (and any other driver's)
            // tries USB scan, then zeroconf, then a PLUTO_IP env var fallback
            // before giving up; Device::make() alone skips all of that and
            // goes straight to iio_create_default_context(), which only finds
            // a USB-attached device.
            auto found = SoapySDR::Device::enumerate(args);
            if (!found.empty()) {
                args = found.front();
                spdlog::info("[{}] Discovered via enumerate(): {}", cfg_.id,
                             args.count("label") ? args.at("label")
                             : args.count("uri") ? args.at("uri") : "?");
            }
        }
        spdlog::info("[{}] Opening: driver={} uri={}", cfg_.id, cfg_.driver,
                     args.count("uri") ? args.at("uri") : cfg_.uri);
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
        // If fixed_sample_rate_hz is configured, use it unconditionally and
        // only call setSampleRate once (on first tune).  This avoids the 3-4 s
        // AD9361 BB-filter recalibration that fires on every rate change.
        const double target_rate = (cfg_.fixed_sample_rate_hz > 0.0)
                                   ? cfg_.fixed_sample_rate_hz : sr_sps;

        if (std::abs(target_rate - current_rate_) > 1.0) {
            dev_->setSampleRate(SOAPY_SDR_RX, 0, target_rate);
            dev_->setSampleRate(SOAPY_SDR_TX, 0, target_rate);
            current_rate_ = target_rate;
            spdlog::info("[{}] Sample rate set to {:.3f} MSPS", cfg_.id, target_rate/1e6);
        }

        // Pure LO hop — single IIO write, settles in ~25 µs on AD9361.
        dev_->setFrequency(SOAPY_SDR_RX, 0, cf_hz);
        dev_->setFrequency(SOAPY_SDR_TX, 0, cf_hz);
        current_cf_ = cf_hz;
        spdlog::info("[{}] Tuned {:.3f} MHz {:.3f} MSPS", cfg_.id, cf_hz/1e6, current_rate_/1e6);
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

std::vector<RadioDevice::TempSensor> RadioDevice::listTemperatures() const {
    std::lock_guard lock(mu_);
    if (!online_ || !dev_) return {};
    std::vector<TempSensor> out;
    try {
        for (auto& name : dev_->listSensors()) {
            if (name.find("temp") == std::string::npos &&
                name.find("Temp") == std::string::npos) continue;
            double val = std::numeric_limits<double>::quiet_NaN();
            try { val = std::stod(dev_->readSensor(name)); } catch (...) {}
            out.push_back({name, val});
        }
    } catch (...) {
        // listSensors() not supported — fall back to well-known "temp0"
        double val = std::numeric_limits<double>::quiet_NaN();
        try { val = std::stod(dev_->readSensor("temp0")); } catch (...) {}
        if (!std::isnan(val)) out.push_back({"temp0", val});
    }
    return out;
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

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
