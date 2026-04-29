# Hardware Compatibility Reference
## SDR Radio Resource Task Manager

All hardware capabilities are declared in `devices.xml`. The controller enforces only
what is listed there — there are no global hardcoded limits. The values below come from
hardware datasheets; verify against your specific board revision.

---

## Supported Hardware Summary

| Board | Driver | RX ch | TX ch | Freq range | Max BW | Max SR | shared_lo |
|-------|--------|-------|-------|-----------|--------|--------|-----------|
| AD9361 MIMO (PlutoSDR+, ADRV9361) | `plutosdr` or `remote` | 2 | 2 | 70 MHz – 6 GHz | 56 MHz | 61.44 MSPS | **true** |
| LimeSDR-USB / LimeSDR Mini 2 | `lime` or `remote` | 2 | 2 | 100 kHz – 3.8 GHz | 130 MHz | 200 MSPS | **true** |
| USRP B210 (AD9361, indep. channels) | `uhd` or `remote` | 2 | 2 | 70 MHz – 6 GHz | 56 MHz | 61.44 MSPS | **false** |
| HackRF One | `hackrf` or `remote` | 1 | 1 | 1 MHz – 6 GHz | 20 MHz | 20 MSPS | false |
| RTL-SDR (R820T2) | `rtlsdr` or `remote` | 1 | 0 | 24 MHz – 1.766 GHz | 8 MHz | 3.2 MSPS | false |
| RTL-SDR V4 (R828D) | `rtlsdr` or `remote` | 1 | 0 | 0.5 MHz – 1.766 GHz | 8 MHz | 3.2 MSPS | false |

---

## Per-Board Configuration

### AD9361 / ADRV9361 (e.g. PlutoSDR+, custom AD9361 MIMO boards)

Both RX channels share a single LO. This is the primary hardware for coherent DF —
two boards provide 4 phase-locked channels when connected to a shared 10 MHz reference.

```xml
<device id="pluto-0">
  <driver>remote</driver>
  <uri>soapy://192.168.10.100:55132</uri>
  <label>AD9361 Board 0</label>
  <streaming_source_ip>10.0.0.10</streaming_source_ip>
  <coherency_group>refclk-group-0</coherency_group>
  <shared_lo>true</shared_lo>
  <capabilities>
    <rx_channels>2</rx_channels>
    <tx_channels>2</tx_channels>
    <freq_min_hz>70000000</freq_min_hz>        <!-- 70 MHz -->
    <freq_max_hz>6000000000</freq_max_hz>      <!-- 6 GHz -->
    <bandwidth_max_hz>56000000</bandwidth_max_hz>
    <sample_rate_max_sps>61440000</sample_rate_max_sps>
    <rx_gain_min_db>-3</rx_gain_min_db>
    <rx_gain_max_db>71</rx_gain_max_db>
    <tx_atten_min_db>0</tx_atten_min_db>
    <tx_atten_max_db>89</tx_atten_max_db>
  </capabilities>
</device>
```

**Notes:**
- `shared_lo=true`: two tasks at the same CF+SR can share the device (different spectrum slices).
  Two tasks at different CFs during the same window get `RETUNE_CONFLICT`.
- Maximum instantaneous bandwidth: 56 MHz. Setting `sample_rate_sps > 56 MSPS` may produce
  aliasing at the edges; keep `usable_bw_fraction ≤ 0.80` in policy.
- The Pluto SDR (original, single channel) differs from the PlutoSDR+ (two channels).
  Verify `SoapySDRUtil --probe=driver=plutosdr` reports `numRxChannels = 2`.
- For cross-board coherent DF, both boards must be in the same `coherency_group` and
  share a 10 MHz external reference. See `README.md §3`.

---

### LimeSDR-USB / LimeSDR Mini 2

```xml
<device id="limesdr-0">
  <driver>remote</driver>
  <uri>soapy://192.168.10.103:55132</uri>
  <label>LimeSDR-USB</label>
  <streaming_source_ip>10.0.0.13</streaming_source_ip>
  <coherency_group>lime-group-0</coherency_group>
  <shared_lo>true</shared_lo>
  <capabilities>
    <rx_channels>2</rx_channels>
    <tx_channels>2</tx_channels>
    <freq_min_hz>100000</freq_min_hz>           <!-- 100 kHz -->
    <freq_max_hz>3800000000</freq_max_hz>       <!-- 3.8 GHz -->
    <bandwidth_max_hz>130000000</bandwidth_max_hz>
    <sample_rate_max_sps>200000000</sample_rate_max_sps>
    <rx_gain_min_db>-12</rx_gain_min_db>
    <rx_gain_max_db>61</rx_gain_max_db>
    <tx_atten_min_db>0</tx_atten_min_db>
    <tx_atten_max_db>60</tx_atten_max_db>
  </capabilities>
</device>
```

**Notes:**
- Extremely wide instantaneous bandwidth (up to 130 MHz) and high sample rate (up to 200 MSPS).
- `shared_lo=true`: channels share LO, same coherence constraints as AD9361.
- LimeSDR Mini has 1 RX channel and a narrower frequency range (10 MHz–3.5 GHz).
  Adjust `rx_channels=1` and `freq_min_hz=10000000` for the Mini.
- LimeSDR gains are in dB, range -12 to +61 dBm. Verify with `SoapySDRUtil --probe`.

---

### USRP B210

The B210 uses the same AD9361 chip as above but exposes both channels as independently
tunable via UHD. Use `shared_lo=false`.

```xml
<device id="b210-0">
  <driver>remote</driver>
  <uri>soapy://192.168.10.104:55132</uri>
  <label>USRP B210</label>
  <streaming_source_ip>10.0.0.14</streaming_source_ip>
  <coherency_group>uhd-group-0</coherency_group>
  <shared_lo>false</shared_lo>
  <capabilities>
    <rx_channels>2</rx_channels>
    <tx_channels>2</tx_channels>
    <freq_min_hz>70000000</freq_min_hz>
    <freq_max_hz>6000000000</freq_max_hz>
    <bandwidth_max_hz>56000000</bandwidth_max_hz>
    <sample_rate_max_sps>61440000</sample_rate_max_sps>
    <rx_gain_min_db>0</rx_gain_min_db>
    <rx_gain_max_db>76</rx_gain_max_db>
    <tx_atten_min_db>0</tx_atten_min_db>
    <tx_atten_max_db>89</tx_atten_max_db>
  </capabilities>
</device>
```

**Notes:**
- `shared_lo=false`: two tasks at completely different frequencies (e.g., 433 MHz and 2.4 GHz)
  can run simultaneously on channels 0 and 1.
- Despite using `shared_lo=false`, RX0 and RX1 still share a reference oscillator, so
  relative phase between channels is stable for short-duration measurements.
  For long-term phase tracking across sessions, calibrate.
- The B210 uses USB 3.0 — sustained sample rates above 30 MSPS require USB 3.0 and a
  fast host machine. For Kubernetes, the host node must have a physical USB 3.0 port
  directly connected (no hubs).

---

### HackRF One

Single-channel, half-duplex (RX or TX, not simultaneously).

```xml
<device id="hackrf-0">
  <driver>remote</driver>
  <uri>soapy://192.168.10.105:55132</uri>
  <label>HackRF One</label>
  <streaming_source_ip>10.0.0.15</streaming_source_ip>
  <coherency_group></coherency_group>
  <shared_lo>false</shared_lo>
  <capabilities>
    <rx_channels>1</rx_channels>
    <tx_channels>1</tx_channels>
    <freq_min_hz>1000000</freq_min_hz>          <!-- 1 MHz -->
    <freq_max_hz>6000000000</freq_max_hz>       <!-- 6 GHz -->
    <bandwidth_max_hz>20000000</bandwidth_max_hz>
    <sample_rate_max_sps>20000000</sample_rate_max_sps>
    <rx_gain_min_db>0</rx_gain_min_db>
    <rx_gain_max_db>47</rx_gain_max_db>
    <tx_atten_min_db>0</tx_atten_min_db>
    <tx_atten_max_db>47</tx_atten_max_db>
  </capabilities>
</device>
```

**Notes:**
- Only 1 RX channel; requests with `rx_count > 1` are rejected.
- Half-duplex: do not schedule RX and TX tasks at the same time on the same board.
  The controller does not enforce this constraint — ensure your DSP pods coordinate.
- HackRF samples are 8-bit (CS8 internally), upsampled to CF32 by SoapySDR driver.
  Dynamic range is limited compared to 12-bit devices.
- No internal TCXO; frequency accuracy depends on the onboard oscillator (~20 ppm).
  Not suitable for coherent multi-board DF without an external reference.

---

### RTL-SDR (R820T2 / R828D)

Single-channel, receive-only. Lowest cost option; useful for single-channel narrowband.

```xml
<device id="rtlsdr-0">
  <driver>remote</driver>
  <uri>soapy://192.168.10.106:55132</uri>
  <label>RTL-SDR R820T2</label>
  <streaming_source_ip>10.0.0.16</streaming_source_ip>
  <coherency_group></coherency_group>
  <shared_lo>false</shared_lo>
  <capabilities>
    <rx_channels>1</rx_channels>
    <tx_channels>0</tx_channels>
    <freq_min_hz>24000000</freq_min_hz>         <!-- 24 MHz (R820T2) -->
    <freq_max_hz>1766000000</freq_max_hz>       <!-- 1.766 GHz -->
    <bandwidth_max_hz>8000000</bandwidth_max_hz>
    <sample_rate_max_sps>3200000</sample_rate_max_sps>  <!-- 3.2 MSPS practical max -->
    <rx_gain_min_db>0</rx_gain_min_db>
    <rx_gain_max_db>49</rx_gain_max_db>
    <tx_atten_min_db>0</tx_atten_min_db>
    <tx_atten_max_db>0</tx_atten_max_db>
  </capabilities>
</device>
```

**RTL-SDR V4 (R828D) variant** — extends lower end to ~500 kHz:
```xml
    <freq_min_hz>500000</freq_min_hz>
    <freq_max_hz>1766000000</freq_max_hz>
```

**Notes:**
- USB 2.0; sustained sample rates above 2.4 MSPS may drop samples on busy systems.
- 8-bit ADC (CS8); limited dynamic range (~48 dBFS practical).
- Not suitable for coherent multi-board applications; use as single receivers only.
- Frequency stability: ~25 ppm unless modified with a TCXO.

---

## Mixed Device Pool

Multiple different board types can coexist in `devices.xml`. The scheduler selects
the best device for each request by matching capabilities:

```xml
<devices>
  <!-- High-performance coherent pair for DF -->
  <device id="pluto-0"> ... AD9361 ... <shared_lo>true</shared_lo> </device>
  <device id="pluto-1"> ... AD9361 ... <shared_lo>true</shared_lo> </device>

  <!-- Wide-frequency single receiver -->
  <device id="hackrf-0"> ... HackRF ... </device>

  <!-- Budget narrowband receivers -->
  <device id="rtlsdr-0"> ... RTL-SDR at 192.168.10.106 ... </device>
  <device id="rtlsdr-1"> ... RTL-SDR at 192.168.10.107 ... </device>
</devices>
```

**Routing behavior with a mixed pool:**

| Request | Routed to |
|---------|-----------|
| CF=50 MHz (below AD9361 min) | hackrf-0 or rtlsdr-0/1 |
| CF=2.4 GHz, SR=50 MSPS | pluto-0 or pluto-1 only |
| CF=915 MHz, rx_count=4, coherency_group=refclk-group-0 | pluto-0 AND pluto-1 |
| CF=915 MHz, rx_count=1 (no group) | least-loaded device that fits |

---

## Discovering Your Hardware's Capabilities

SoapySDR's probe utility reports the actual hardware ranges:

```bash
# Probe all connected devices
SoapySDRUtil --probe

# Probe a specific remote device
SoapySDRUtil --probe="driver=remote,remote=192.168.10.100:55132"

# Output includes:
#   Driver/Hardware key
#   numRxChannels, numTxChannels
#   Frequency range(s)
#   Bandwidth range(s)
#   Sample rate range(s)
#   Gain range(s)
```

Map probe output to `devices.xml` fields:

| Probe field | XML element |
|-------------|-------------|
| `numRxChannels` | `<rx_channels>` |
| `numTxChannels` | `<tx_channels>` |
| Frequency range minimum | `<freq_min_hz>` |
| Frequency range maximum | `<freq_max_hz>` |
| Bandwidth range maximum | `<bandwidth_max_hz>` |
| Sample rate range maximum | `<sample_rate_max_sps>` |
| RX gain range minimum | `<rx_gain_min_db>` |
| RX gain range maximum | `<rx_gain_max_db>` |

---

## IQ Packet Size vs MTU

Default `iq_packet_samples=1024` → **8224-byte packets**. This requires jumbo frames
(MTU ≥ 9000) on all network switches in the path between the controller pod and DSP pods.

For standard 1500-byte Ethernet MTU set in `devices.xml` policy:

```xml
<iq_packet_samples>183</iq_packet_samples>
```

183 samples × 8 bytes/sample + 32 bytes header = 1496 bytes ≤ 1500 MTU.

**Trade-off**: smaller packets = more packets per second = higher interrupt rate.
At 61.44 MSPS with 183 samples/packet: ~335,000 pkt/s per channel.
At 61.44 MSPS with 1024 samples/packet: ~60,000 pkt/s per channel.

Use jumbo frames in production when possible.
