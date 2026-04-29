#include "SweepConfig.hpp"
#include <tinyxml2.h>
#include <fmt/format.h>
#include <sstream>

namespace acq {

using namespace tinyxml2;

namespace {

const char* textOrDefault(XMLElement* el, const char* def = "") {
    return el && el->GetText() ? el->GetText() : def;
}
XMLElement* need(XMLElement* parent, const char* name) {
    auto* el = parent ? parent->FirstChildElement(name) : nullptr;
    if (!el) throw std::runtime_error(fmt::format("Missing XML element <{}>", name));
    return el;
}
XMLElement* opt(XMLElement* parent, const char* name) {
    return parent ? parent->FirstChildElement(name) : nullptr;
}

} // namespace

std::string DbConfig::connection_string() const {
    return fmt::format("host={} port={} dbname={} user={} password={}",
        host, port, name, user, password);
}

SweepConfig SweepConfig::from_file(const std::string& path) {
    XMLDocument doc;
    if (doc.LoadFile(path.c_str()) != XML_SUCCESS)
        throw std::runtime_error(fmt::format("Cannot open config: {} — {}", path, doc.ErrorStr()));

    auto* root = doc.FirstChildElement("sdr_acquisition");
    if (!root) throw std::runtime_error("Root element <sdr_acquisition> not found");

    SweepConfig cfg;

    if (auto* el = opt(root, "scanner_id"))
        cfg.scanner_id = el->GetText() ? el->GetText() : cfg.scanner_id;

    // ── AMQP ────────────────────────────────────────────────────────────────
    if (auto* amqp = opt(root, "amqp")) {
        cfg.amqp.url              = textOrDefault(opt(amqp, "url"), cfg.amqp.url.c_str());
        cfg.amqp.username         = textOrDefault(opt(amqp, "username"));
        cfg.amqp.password         = textOrDefault(opt(amqp, "password"));
        cfg.amqp.detection_topic  = textOrDefault(opt(amqp, "detection_topic"),
                                                   cfg.amqp.detection_topic.c_str());
        if (auto* el = opt(amqp, "reconnect_interval_sec"))
            el->QueryIntText(&cfg.amqp.reconnect_interval_sec);
    }

    // ── Database ─────────────────────────────────────────────────────────────
    if (auto* db = opt(root, "database")) {
        cfg.db.host     = textOrDefault(opt(db, "host"), cfg.db.host.c_str());
        if (auto* el = opt(db, "port")) el->QueryIntText(&cfg.db.port);
        cfg.db.name     = textOrDefault(opt(db, "name"), cfg.db.name.c_str());
        cfg.db.user     = textOrDefault(opt(db, "user"));
        cfg.db.password = textOrDefault(opt(db, "password"));
    }

    // ── Device ───────────────────────────────────────────────────────────────
    auto* dev = need(root, "device");
    cfg.device.driver = textOrDefault(opt(dev, "driver"), cfg.device.driver.c_str());
    cfg.device.uri    = textOrDefault(opt(dev, "uri"));
    if (auto* el = opt(dev, "rx_channels"))    el->QueryIntText(&cfg.device.rx_channels);
    if (auto* el = opt(dev, "sample_rate_sps"))el->QueryDoubleText(&cfg.device.sample_rate);
    if (auto* el = opt(dev, "rx_gain_db"))     el->QueryDoubleText(&cfg.device.rx_gain_db);
    if (auto* el = opt(dev, "shared_lo"))      cfg.device.shared_lo = std::string(el->GetText() ? el->GetText() : "false") == "true";

    cfg.device.rx_channels = std::max(1, cfg.device.rx_channels);

    // ── Sweep ────────────────────────────────────────────────────────────────
    auto* sweep = need(root, "sweep");
    if (auto* el = opt(sweep, "start_hz"))          el->QueryUnsigned64Text(&cfg.sweep.start_hz);
    if (auto* el = opt(sweep, "stop_hz"))           el->QueryUnsigned64Text(&cfg.sweep.stop_hz);
    if (auto* el = opt(sweep, "dwell_samples"))     el->QueryIntText(&cfg.sweep.dwell_samples);
    if (auto* el = opt(sweep, "fft_size"))          el->QueryIntText(&cfg.sweep.fft_size);
    if (auto* el = opt(sweep, "usable_bw_fraction"))el->QueryDoubleText(&cfg.sweep.usable_bw_fraction);
    if (auto* el = opt(sweep, "threshold_db"))      el->QueryDoubleText(&cfg.sweep.threshold_db);
    if (auto* el = opt(sweep, "min_signal_bw_hz"))  el->QueryUnsigned64Text(reinterpret_cast<uint64_t*>(&cfg.sweep.min_signal_bw_hz));
    if (auto* el = opt(sweep, "settle_samples"))    el->QueryIntText(&cfg.sweep.settle_samples);

    if (cfg.sweep.stop_hz <= cfg.sweep.start_hz)
        throw std::runtime_error("sweep stop_hz must be > start_hz");
    if (cfg.sweep.fft_size < 64)
        throw std::runtime_error("fft_size must be >= 64");
    if (cfg.sweep.dwell_samples < cfg.sweep.fft_size)
        cfg.sweep.dwell_samples = cfg.sweep.fft_size;

    return cfg;
}

} // namespace acq
