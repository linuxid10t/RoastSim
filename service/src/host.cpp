#include "roastsim/host.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "internal.hpp"
#include "roastsim/platform.hpp"
#include "roastsim/scenario.hpp"

namespace roastsim {

// ---------------------------------------------------------------- config
HostConfig HostConfig::fromJson(const json::Value& v, const HostConfig& base) {
    HostConfig c = base;
    if (!v.isObject()) return c;
    c.data_dir = v.get("data_dir", c.data_dir.c_str());
    c.web_dir = v.get("web_dir", c.web_dir.c_str());
    c.presets_dir = v.get("presets_dir", c.presets_dir.c_str());
    c.bind_address = v.get("bind_address", c.bind_address.c_str());
    c.http_enabled = v.getBool("http_enabled", c.http_enabled);
    c.http_port = static_cast<int>(v.get("http_port", static_cast<double>(c.http_port)));
    c.modbus_enabled = v.getBool("modbus_enabled", c.modbus_enabled);
    c.modbus_port = static_cast<int>(v.get("modbus_port", static_cast<double>(c.modbus_port)));
    c.tc4_port = v.get("tc4_port", c.tc4_port.c_str());
    c.tc4_baud = static_cast<int>(v.get("tc4_baud", static_cast<double>(c.tc4_baud)));
    c.roaster = v.get("roaster", c.roaster.c_str());
    c.bean = v.get("bean", c.bean.c_str());
    c.time_scale = v.get("time_scale", c.time_scale);
    c.lock_realtime_with_logger = v.getBool("lock_realtime_with_logger", c.lock_realtime_with_logger);
    c.seed = static_cast<std::uint64_t>(v.get("seed", static_cast<double>(c.seed)));
    c.parcels = static_cast<int>(v.get("parcels", static_cast<double>(c.parcels)));
    std::string u = v.get("units", std::string(1, c.units).c_str());
    c.units = (!u.empty() && (u[0] == 'F' || u[0] == 'f')) ? 'F' : 'C';
    c.weight_units = v.get("weight_units", c.weight_units.c_str());
    c.altitude_units = v.get("altitude_units", c.altitude_units.c_str());
    c.config_path = v.get("config_path", c.config_path.c_str());
    c.logger_timeout_s = v.get("logger_timeout_s", c.logger_timeout_s);
    const json::Value& w = v["artisan_ws"];
    if (w.isObject()) {
#define RS_W(n) c.ws.n = w.get(#n, c.ws.n.c_str())
        RS_W(id_node); RS_W(machine_node); RS_W(command_node); RS_W(data_node); RS_W(push_node);
        RS_W(request_data); RS_W(charge_message); RS_W(drop_message); RS_W(add_event_message);
        RS_W(event_node); RS_W(dry_event); RS_W(fcs_event); RS_W(fce_event); RS_W(scs_event); RS_W(sce_event);
#undef RS_W
        c.ws.push_charge_drop = w.getBool("push_charge_drop", c.ws.push_charge_drop);
        c.ws.push_crack_events = w.getBool("push_crack_events", c.ws.push_crack_events);
    }
    return c;
}

json::Value HostConfig::toJson() const {
    json::Value v = json::Value::object();
    v["data_dir"] = data_dir;
    v["web_dir"] = web_dir;
    v["presets_dir"] = presets_dir;
    v["bind_address"] = bind_address;
    v["http_enabled"] = http_enabled;
    v["http_port"] = http_port;
    v["modbus_enabled"] = modbus_enabled;
    v["modbus_port"] = modbus_port;
    v["tc4_port"] = tc4_port;
    v["tc4_baud"] = tc4_baud;
    v["roaster"] = roaster;
    v["bean"] = bean;
    v["time_scale"] = time_scale;
    v["lock_realtime_with_logger"] = lock_realtime_with_logger;
    v["seed"] = static_cast<double>(seed);
    v["parcels"] = parcels;
    v["units"] = std::string(1, units);
    v["weight_units"] = weight_units;
    v["altitude_units"] = altitude_units;
    json::Value w = json::Value::object();
    w["id_node"] = ws.id_node;
    w["machine_node"] = ws.machine_node;
    w["command_node"] = ws.command_node;
    w["data_node"] = ws.data_node;
    w["push_node"] = ws.push_node;
    w["request_data"] = ws.request_data;
    w["charge_message"] = ws.charge_message;
    w["drop_message"] = ws.drop_message;
    w["add_event_message"] = ws.add_event_message;
    w["event_node"] = ws.event_node;
    w["push_charge_drop"] = ws.push_charge_drop;
    w["push_crack_events"] = ws.push_crack_events;
    v["artisan_ws"] = w;
    return v;
}

// ---------------------------------------------------------------- host
Host::Host(HostConfig cfg) : cfg_(std::move(cfg)) {
    timeScale_ = cfg_.time_scale > 0 ? cfg_.time_scale : 1.0;
    std::string ignored;
    changeUnits(cfg_.units, cfg_.weight_units, cfg_.altitude_units, false, &ignored);
}

Host::~Host() { stop(); }

bool Host::start(std::string* err) {
    if (running_) return true;
    if (cfg_.data_dir.empty()) cfg_.data_dir = platform::findResourceDir("data");
    if (cfg_.web_dir.empty()) cfg_.web_dir = platform::findResourceDir("web");
    if (cfg_.presets_dir.empty()) cfg_.presets_dir = platform::userPresetDir();
    catalog_ = Catalog{};
    if (!catalog_.loadDir(cfg_.data_dir)) {
        if (err) {
            *err = "cannot load roaster/bean definitions from '" + cfg_.data_dir + "'";
            for (const auto& e : catalog_.errors()) *err += "\n  " + e;
        }
        return false;
    }
    for (const auto& e : catalog_.errors()) messages_.push_back("warning: " + e);
    loadUserPresets();

    const RoasterDef* r = catalog_.roaster(cfg_.roaster);
    if (!r) {
        messages_.push_back("roaster '" + cfg_.roaster + "' not found, using '" + catalog_.roasters().front().id + "'");
        r = &catalog_.roasters().front();
    }
    const BeanSpec* b = catalog_.bean(cfg_.bean);
    if (!b) b = &catalog_.beans().front();
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::uint64_t seed = cfg_.seed ? cfg_.seed
                                       : static_cast<std::uint64_t>(
                                             std::chrono::steady_clock::now().time_since_epoch().count());
        sim_.setSeed(seed);
        sim_.setParcelCount(cfg_.parcels);
        sim_.load(*r, *b);
        ++roasterRevision_;
        batchG_ = r->batch.default_g;
    }

    stop_ = false;
    running_ = true;
    simThread_ = std::thread([this] { simLoop(); });

    std::string serr;
    if (cfg_.http_enabled || cfg_.modbus_enabled) {
        if (!platform::netInit()) messages_.push_back("network init failed");
        net_ = std::make_unique<NetServer>(*this);
        if (!net_->start(&serr, messages_)) messages_.push_back(serr);
    }
    if (!cfg_.tc4_port.empty()) {
        tc4_ = std::make_unique<Tc4Serial>(*this);
        if (!tc4_->start(cfg_.tc4_port, cfg_.tc4_baud, &serr)) messages_.push_back("TC4 serial: " + serr);
        else messages_.push_back("TC4 serial: " + tc4_->status());
    }
    return true;
}

void Host::stop() {
    if (!running_) return;
    stop_ = true;
    if (tc4_) tc4_->stop();
    if (net_) net_->stop();
    if (simThread_.joinable()) simThread_.join();
    tc4_.reset();
    if (net_) {
        net_.reset();
        platform::netShutdown();
    }
    running_ = false;
}

void Host::simLoop() {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    while (!stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto now = clock::now();
        double wall = std::chrono::duration<double>(now - last).count();
        last = now;
        wall = std::min(wall, 0.25);  // after a stall, don't try to catch up wildly
        bool logger = loggerConnected();
        std::lock_guard<std::mutex> lk(mu_);
        if (paused_) continue;
        double scale = (logger && cfg_.lock_realtime_with_logger) ? 1.0 : timeScale_;
        sim_.step(std::min(wall * scale, 60.0));
    }
}

Snapshot Host::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sim_.snapshot();
}

VisualState Host::visual() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sim_.visual();
}

std::vector<SimEvent> Host::eventsSince(std::uint64_t seq) const {
    std::lock_guard<std::mutex> lk(mu_);
    return sim_.eventsSince(seq);
}

std::vector<HistorySample> Host::historySince(std::uint64_t index, std::uint64_t* nextIndex, std::size_t max) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto& h = sim_.history();
    std::uint64_t base = sim_.historyBase();
    std::uint64_t end = base + h.size();
    std::uint64_t from = std::max(index, base);
    std::vector<HistorySample> out;
    for (std::uint64_t i = from; i < end && out.size() < max; ++i) out.push_back(h[static_cast<std::size_t>(i - base)]);
    if (nextIndex) *nextIndex = from + out.size();
    return out;
}

std::vector<CrackEvent> Host::cracksSince(std::uint64_t seq, std::size_t max) const {
    std::lock_guard<std::mutex> lk(mu_);
    return sim_.cracksSince(seq, max);
}

std::uint64_t Host::crackSeq() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sim_.crackSeq();
}

RoasterDef Host::roasterDef() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sim_.roaster();
}

BeanSpec Host::beanSpec() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sim_.bean();
}

std::uint64_t Host::historyEnd() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sim_.historyBase() + sim_.history().size();
}

bool Host::setControl(Control c, double pct, const std::string& origin) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!sim_.roaster().hasControl(c)) return false;
    sim_.setControl(c, pct);
    return true;
}

double Host::toLoggerUnits(double c) const {
    if (c == -1.0) return -1.0;  // open probe marker passes through
    return units() == 'F' ? c * 9.0 / 5.0 + 32.0 : c;
}

std::string Host::weightUnits() const {
    std::lock_guard<std::mutex> lk(unitsMu_);
    return weightUnit_;
}

std::string Host::altitudeUnits() const {
    std::lock_guard<std::mutex> lk(unitsMu_);
    return altitudeUnit_;
}

json::Value Host::unitsJson() const {
    json::Value v = json::Value::object();
    v["temperature"] = std::string(1, units());
    std::lock_guard<std::mutex> lk(unitsMu_);
    v["weight"] = weightUnit_;
    v["altitude"] = altitudeUnit_;
    return v;
}

bool Host::changeUnits(char temp, const std::string& weight, const std::string& altitude, bool save, std::string* err,
                       bool* saved) {
    auto lower = [](std::string s) {
        for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    std::string w = lower(weight), a = lower(altitude);
    if (w == "lbs" || w == "pound" || w == "pounds") w = "lb";
    if (w == "grams" || w == "gram") w = "g";
    if (a == "feet" || a == "foot") a = "ft";
    if (a == "meters" || a == "metres") a = "m";
    if (temp && temp != 'C' && temp != 'c' && temp != 'F' && temp != 'f') {
        if (err) *err = "temperature units must be C or F";
        return false;
    }
    if (!w.empty() && w != "g" && w != "kg" && w != "oz" && w != "lb") {
        if (err) *err = "weight units must be g, kg, oz or lb";
        return false;
    }
    if (!a.empty() && a != "m" && a != "ft") {
        if (err) *err = "altitude units must be m or ft";
        return false;
    }
    if (temp) setUnits(temp);
    {
        std::lock_guard<std::mutex> lk(unitsMu_);
        if (!w.empty()) weightUnit_ = w;
        if (!a.empty()) altitudeUnit_ = a;
    }
    if (saved) *saved = false;
    if (save && !cfg_.config_path.empty()) {
        // Update only the unit keys, keeping everything else the user put in the file.
        std::string text, perr;
        json::Value file = json::Value::object();
        if (readTextFile(cfg_.config_path, text)) {
            json::Value parsed = json::Value::parse(text, &perr);
            if (perr.empty() && parsed.isObject()) file = parsed;
        }
        json::Value u = unitsJson();
        file["units"] = u["temperature"];
        file["weight_units"] = u["weight"];
        file["altitude_units"] = u["altitude"];
        std::ofstream f(std::filesystem::u8path(cfg_.config_path), std::ios::binary | std::ios::trunc);
        if (f) {
            f << file.dump(2) << "\n";
            if (saved) *saved = static_cast<bool>(f);
        }
    }
    return true;
}

double Host::fromLoggerUnits(double t) const { return units() == 'F' ? (t - 32.0) * 5.0 / 9.0 : t; }

void Host::noteLoggerActivity(const std::string& who) {
    std::lock_guard<std::mutex> lk(loggerMu_);
    lastLogger_ = std::chrono::steady_clock::now();
    loggerName_ = who;
}

bool Host::loggerConnected() const {
    std::lock_guard<std::mutex> lk(loggerMu_);
    if (loggerName_.empty()) return false;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - lastLogger_).count() < cfg_.logger_timeout_s;
}

double Host::timeScale() const {
    std::lock_guard<std::mutex> lk(mu_);
    return timeScale_;
}

bool Host::paused() const {
    std::lock_guard<std::mutex> lk(mu_);
    return paused_;
}

json::Value Host::serverInfo() const {
    json::Value v = json::Value::object();
    v["bind_address"] = cfg_.bind_address;
    if (net_) v["net"] = net_->info();
    v["tc4"] = tc4_ ? tc4_->status() : std::string("off");
    v["units"] = unitsJson();
    json::Value lg = json::Value::object();
    lg["connected"] = loggerConnected();
    {
        std::lock_guard<std::mutex> lk(loggerMu_);
        lg["via"] = loggerName_;
    }
    v["logger"] = lg;
    json::Value msgs = json::Value::array();
    for (const auto& m : messages_) msgs.push(m);
    v["messages"] = msgs;
    v["data_dir"] = cfg_.data_dir;
    v["web_dir"] = cfg_.web_dir;
    return v;
}

std::vector<std::string> Host::startupMessages() const { return messages_; }

}  // namespace roastsim
