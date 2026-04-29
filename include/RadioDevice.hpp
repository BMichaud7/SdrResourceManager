#pragma once
#include "ConfigParser.hpp"
#include "sdr/Types.hpp"
#include <SoapySDR/Device.hpp>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace sdr {

class RadioDevice {
public:
    explicit RadioDevice(const DeviceConfig& cfg);
    ~RadioDevice();
    RadioDevice(const RadioDevice&)=delete;
    RadioDevice& operator=(const RadioDevice&)=delete;

    bool open();
    void close();
    bool isOnline() const { return online_.load(std::memory_order_acquire); }
    // Raw SoapySDR device pointer — only valid while isOnline(). Used by IQStreamer/TriggerMonitor.
    SoapySDR::Device* soapyDevice() const { return dev_; }
    const std::string& id()     const { return cfg_.id; }
    const std::string& group()  const { return cfg_.coherency_group; }
    const DeviceConfig& config()const { return cfg_; }

    // Tune all channels via the shared LO (shared_lo=true devices).
    bool tune(double cf_hz, double sr_sps);
    // Tune a specific channel independently (shared_lo=false devices).
    bool tuneChannel(int ch, double cf_hz, double sr_sps);
    bool setRxGain(int ch, double gain_db, bool agc);
    bool setTxAtten(int ch, double atten_db);

    SoapySDR::Stream* openRxStream(const std::vector<int>& channels);
    bool  activateStream(SoapySDR::Stream* s);
    void  deactivateStream(SoapySDR::Stream* s);
    void  closeStream(SoapySDR::Stream* s);

    // Direct stream read — used by snapshot path (synchronous, no IQStreamer)
    int   readStream(SoapySDR::Stream* s, void** buffs, size_t numElems,
                     int& flags, long long& timeNs, long timeoutUs = 500'000LL);

    double getTemperature() const;
    double currentCF()   const;
    double currentRate() const;

    struct Status {
        std::string device_id, driver, uri, coherency_group;
        bool   online         = false;
        double center_freq_hz = 0.0;
        double sample_rate    = 0.0;
        double temperature_c  = 0.0;
    };
    Status getStatus() const;

private:
    DeviceConfig      cfg_;
    SoapySDR::Device* dev_    = nullptr;
    std::atomic<bool> online_ {false};
    mutable std::mutex mu_;
    double current_cf_   = 0.0;
    double current_rate_ = 0.0;
};

} // namespace sdr
