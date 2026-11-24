// Host: owns one Simulation, paces it in real time on its own thread, and
// exposes it to every front end through one JSON command API:
//
//   native shells (C API) ─┐
//   web UI (WebSocket)    ─┼──► Host::command(json) ──► Simulation (mutex)
//   roastsimd console     ─┤
//   Artisan (Modbus/WS/TC4)┘    plus thin protocol adapters for Artisan
//
// The future 3D view reads Host::visual() snapshots (copy under the lock), so
// it never blocks or perturbs the physics thread for long.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "roastsim/catalog.hpp"
#include "roastsim/json.hpp"
#include "roastsim/simulation.hpp"

namespace roastsim {

// Node names/messages for Artisan's WebSocket device. Defaults equal Artisan's
// own defaults, so only host/port need changing in Artisan.
struct ArtisanWsConfig {
    std::string id_node = "id";
    std::string machine_node = "roasterID";
    std::string command_node = "command";
    std::string data_node = "data";
    std::string push_node = "pushMessage";
    std::string request_data = "getData";
    std::string charge_message = "startRoasting";
    std::string drop_message = "endRoasting";
    std::string add_event_message = "addEvent";
    std::string event_node = "event";
    std::string dry_event = "colorChangeEvent";
    std::string fcs_event = "firstCrackBeginningEvent";
    std::string fce_event = "firstCrackEndEvent";
    std::string scs_event = "secondCrackBeginningEvent";
    std::string sce_event = "secondCrackEndEvent";
    bool push_charge_drop = true;   // tell Artisan when CHARGE/DROP happen elsewhere
    bool push_crack_events = true;  // auto-mark DRY/FC/SC detected by the physics
};

struct HostConfig {
    std::string data_dir;        // empty: auto-detect "data"
    std::string web_dir;         // empty: auto-detect "web"
    std::string presets_dir;     // empty: the user's RoastSim data directory
    std::string bind_address = "127.0.0.1";  // "0.0.0.0" to serve the LAN
    bool http_enabled = true;
    int http_port = 8080;        // web UI + JSON API + WebSocket (Artisan and UI)
    bool modbus_enabled = true;
    int modbus_port = 502;
    std::string tc4_port;        // "" off; "COM20" (Windows, via com0com); "pty" or "/dev/tty..." (POSIX)
    int tc4_baud = 115200;
    std::string roaster = "popper_modded";
    std::string bean = "colombia_washed";
    double batch_g = 0;          // batch for the next charge; 0 (or out of the roaster's range) = its default
    json::Value env;             // starting environment (ambient_C, humidity_rh, ...); keys left out keep the defaults
    double time_scale = 1.0;
    bool lock_realtime_with_logger = true;  // force 1x while Artisan is connected
    std::uint64_t seed = 0;      // 0 = random per run
    int parcels = 200;
    // Units shown by every UI and (temperature) sent to loggers. One shared setting
    // so RoastSim and Artisan always agree. Internally everything stays C / g / m.
    char units = 'C';            // temperature: C or F
    std::string weight_units = "g";   // g | kg | oz | lb
    std::string altitude_units = "m"; // m | ft
    // Where settings changed at run time (units, roaster, bean, batch, environment,
    // speed, front-end "ui" settings) are saved (roastsim.json); "" = don't save.
    // Front ends get the right file without setting it: rs_host_create() and
    // roastsimd use platform::defaultConfigPath() unless told otherwise.
    std::string config_path;
    std::string config_error;    // set by fromFile when the file doesn't parse (not saved)
    double logger_timeout_s = 5.0;
    ArtisanWsConfig ws;

    static HostConfig fromJson(const json::Value& v, const HostConfig& base);
    // Lays the settings file at `path` over `base` and sets config_path to it.
    // A missing file is fine (it is created on the first save). One that doesn't
    // parse is skipped and reported in config_error, and is never overwritten.
    static HostConfig fromFile(const std::string& path, const HostConfig& base);
    json::Value toJson() const;
};

class NetServer;
class Tc4Serial;

class Host {
public:
    explicit Host(HostConfig cfg);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // Loads the catalog and roaster, starts the sim thread and enabled servers.
    // Servers that fail to start are reported in `err`/serverInfo but don't abort.
    bool start(std::string* err);
    void stop();

    // The one command API (thread-safe). See docs/API.md.
    json::Value command(const json::Value& cmd, const std::string& origin);

    // Fast typed accessors for UIs and protocol adapters (thread-safe).
    Snapshot snapshot() const;
    VisualState visual() const;
    std::vector<SimEvent> eventsSince(std::uint64_t seq) const;
    std::vector<HistorySample> historySince(std::uint64_t index, std::uint64_t* nextIndex, std::size_t max = 100000) const;
    std::uint64_t historyEnd() const;    // index one past the newest sample
    std::uint64_t historyStart() const;  // index of the oldest sample still held (moves past all on a cold reset)
    std::vector<CrackEvent> cracksSince(std::uint64_t seq, std::size_t max = 4096) const;
    std::uint64_t crackSeq() const;
    RoasterDef roasterDef() const;  // copy of the loaded roaster definition
    BeanSpec beanSpec() const;      // copy of the loaded bean
    bool setControl(Control c, double pct, const std::string& origin);
    json::Value stateJson(bool full) const;

    // Temperature in the units configured for loggers.
    double toLoggerUnits(double celsius) const;
    double fromLoggerUnits(double t) const;
    char units() const { return tempUnit_.load(); }
    void setUnits(char u) { tempUnit_ = (u == 'F' || u == 'f') ? 'F' : 'C'; }  // temperature only, not saved
    std::string weightUnits() const;
    std::string altitudeUnits() const;
    json::Value unitsJson() const;
    // Changes any of the three (empty / 0 = keep) and saves them to config_path.
    // Returns false with a message for an unknown unit.
    bool changeUnits(char temp, const std::string& weight, const std::string& altitude, bool save, std::string* err,
                     bool* saved = nullptr);
    // Merges `patch` into the file at config_path, keeping every other key already
    // there (objects merge key by key). Returns false when there is no config_path or the file
    // could not be written.
    bool saveSettings(const json::Value& patch);

    void noteLoggerActivity(const std::string& who);
    bool loggerConnected() const;
    double timeScale() const;
    bool paused() const;

    const HostConfig& config() const { return cfg_; }
    json::Value serverInfo() const;
    std::vector<std::string> startupMessages() const;

private:
    void simLoop();
    void loadUserPresets();
    json::Value presetCommand(const json::Value& cmd);
    json::Value describeServers() const;

    HostConfig cfg_;
    Catalog catalog_;
    mutable std::mutex mu_;
    std::set<std::string> userRoasters_, userBeans_;
    std::uint64_t catalogRevision_ = 0;
    std::uint64_t roasterRevision_ = 0;
    Simulation sim_;
    double batchG_ = 0;           // batch size for the next charge
    double timeScale_ = 1.0;
    bool paused_ = false;

    std::thread simThread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};

    std::mutex configMu_;         // serializes writes to config_path
    std::atomic<char> tempUnit_{'C'};
    mutable std::mutex unitsMu_;
    std::string weightUnit_ = "g", altitudeUnit_ = "m";

    mutable std::mutex loggerMu_;
    std::chrono::steady_clock::time_point lastLogger_{};
    std::string loggerName_;

    std::unique_ptr<NetServer> net_;
    std::unique_ptr<Tc4Serial> tc4_;
    std::vector<std::string> messages_;
};

}  // namespace roastsim
