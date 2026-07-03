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
/**
 * @file RadioDevice.hpp
 * @brief SoapySDR device wrapper with tune/gain/stream helpers.
 *
 * RadioDevice owns one SoapySDR device handle and exposes the operations
 * ResourceManager needs: open/close, tune, gain control, stream management,
 * and temperature readout.
 *
 * ## Thread safety
 * open(), close(), tune(), and setRxGain() are protected by an internal mutex.
 * isOnline() uses an atomic — safe to call from any thread without locking.
 * readStream() is called from IQStreamer threads; it is NOT mutex-protected
 * (SoapySDR streams are thread-safe by design for concurrent reads).
 */
#include "ConfigParser.hpp"
#include "sdr/Types.hpp"
#include <SoapySDR/Device.hpp>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace sdr {

/**
 * @brief Thin wrapper around one SoapySDR device.
 *
 * Non-copyable.  Created by ResourceManager::openDevices().
 */
class RadioDevice {
public:
    /**
     * @brief Construct without opening the device.
     * @param cfg Device configuration from devices.xml.
     */
    explicit RadioDevice(const DeviceConfig& cfg);
    ~RadioDevice();
    RadioDevice(const RadioDevice&)=delete;
    RadioDevice& operator=(const RadioDevice&)=delete;

    /**
     * @brief Open the SoapySDR device (calls SoapySDR::Device::make()).
     * @return true on success; false if make() throws.
     */
    bool open();
    /// @brief Close the SoapySDR device.
    void close();

    /// @brief True while the device is open and usable.
    bool isOnline() const { return online_.load(std::memory_order_acquire); }
    /// @brief Raw SoapySDR device pointer — valid only while isOnline().
    SoapySDR::Device* soapyDevice() const { return dev_; }

    const std::string&   id()     const { return cfg_.id; }
    const std::string&   group()  const { return cfg_.coherency_group; }
    const DeviceConfig&  config() const { return cfg_; }

    /**
     * @brief Tune all channels to @p cf_hz at @p sr_sps (shared_lo devices).
     * @param cf_hz  Centre frequency (Hz).
     * @param sr_sps Sample rate (samples/s).
     * @return true on success.
     */
    bool tune(double cf_hz, double sr_sps);

    /**
     * @brief Tune a single channel independently (shared_lo=false devices).
     * @param ch     Channel index.
     * @param cf_hz  Centre frequency (Hz).
     * @param sr_sps Sample rate (samples/s).
     * @return true on success.
     */
    bool tuneChannel(int ch, double cf_hz, double sr_sps);

    /**
     * @brief Set the RX gain for one channel.
     * @param ch      Channel index.
     * @param gain_db Gain in dB.
     * @param agc     Enable automatic gain control.
     * @return true on success.
     */
    bool setRxGain(int ch, double gain_db, bool agc);

    /**
     * @brief Set the TX attenuation for one channel.
     * @param ch       Channel index.
     * @param atten_db Attenuation in dB.
     * @return true on success.
     */
    bool setTxAtten(int ch, double atten_db);

    /**
     * @brief Open a SoapySDR RX stream on the given channels.
     * @param channels Physical channel indices to include in the stream.
     * @return Stream handle; nullptr on failure.
     */
    SoapySDR::Stream* openRxStream(const std::vector<int>& channels);
    bool  activateStream(SoapySDR::Stream* s);
    void  deactivateStream(SoapySDR::Stream* s);
    void  closeStream(SoapySDR::Stream* s);

    /**
     * @brief Direct synchronous stream read (used by the snapshot path).
     *
     * Not mutex-protected; SoapySDR streams are thread-safe for concurrent reads.
     *
     * @param s         Stream handle.
     * @param buffs     Per-channel buffer pointers.
     * @param numElems  Maximum samples to read.
     * @param flags     SoapySDR flags (output).
     * @param timeNs    Timestamp of first sample (output).
     * @param timeoutUs Read timeout (microseconds).
     * @return Number of samples read, or negative error code.
     */
    int   readStream(SoapySDR::Stream* s, void** buffs, size_t numElems,
                     int& flags, long long& timeNs, long timeoutUs = 500'000LL);

    /// @brief Read the primary temperature sensor (°C).  Returns NaN on failure.
    double getTemperature() const;

    /// @brief One temperature sensor reading.
    struct TempSensor { std::string name; double value_c = 0.0; };

    /**
     * @brief Enumerate all temperature sensors exposed by the device.
     *
     * Uses SoapySDR::Device::listSensors() filtered to "temp" names.
     * Falls back to reading "temp0" directly when listSensors() is unavailable.
     * value_c is NaN for sensors that fail to read.
     *
     * @return Vector of TempSensor, one per available sensor.
     */
    std::vector<TempSensor> listTemperatures() const;

    double currentCF()   const;  ///< Last tuned centre frequency (Hz).
    double currentRate() const;  ///< Last set sample rate (samples/s).

    /// @brief Snapshot of device status for health reporting.
    struct Status {
        std::string device_id, driver, uri, coherency_group;
        bool   online         = false;
        double center_freq_hz = 0.0;
        double sample_rate    = 0.0;
        double temperature_c  = 0.0;
    };
    /// @brief Return a status snapshot (safe to call from any thread).
    Status getStatus() const;

    /**
     * @brief Close and reopen the device to recover from a broken connection.
     *
     * Called automatically by openRxStream() after consecutive failures to
     * recover from stale network state (e.g. a dead iiod TCP connection that
     * would otherwise cause connect() to block for minutes on the next attempt).
     *
     * @return true if the reopen succeeded.
     */
    bool reopen();

private:
    DeviceConfig      cfg_;
    SoapySDR::Device* dev_    = nullptr;
    std::atomic<bool> online_ {false};
    mutable std::mutex mu_;
    double current_cf_   = 0.0;
    double current_rate_ = 0.0;
    int consecutive_stream_failures_ = 0;

    // Identifier (uri/hostname/serial/label) claimed via Device::enumerate()
    // when devices.xml leaves <uri> empty, so a second RadioDevice instance
    // of the same driver doesn't grab the same physical unit. Empty when
    // this device was opened with an explicit uri instead of discovery.
    std::string claimed_key_;
    static std::mutex&            claimMutex();
    static std::set<std::string>& claimedUris();
};

} // namespace sdr

/*
========================================================================
End of file — OpenRFStack
Subject to Personal Use License
https://github.com/OpenRFStack
========================================================================
*/
