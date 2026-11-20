// roastsimd: headless virtual roaster. Runs the simulation in real time and
// serves Artisan (Modbus TCP, WebSocket, TC4 serial) plus the web UI. Works the
// same on Windows, Linux and Haiku; the console accepts simple commands.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "roastsim/audio.hpp"
#include "roastsim/catalog.hpp"
#include "roastsim/host.hpp"
#include "roastsim/platform.hpp"
#include "roastsim/scenario.hpp"

using namespace roastsim;

namespace {

std::atomic<bool> g_quit{false};
void onSignal(int) { g_quit = true; }

void usage() {
    std::printf(
        "roastsimd - virtual coffee roaster for Artisan and friends\n\n"
        "usage: roastsimd [options]\n"
        "  --config FILE        JSON host config (see docs/API.md)\n"
        "  --roaster ID         roaster preset (see --list)\n"
        "  --bean ID            green coffee preset\n"
        "  --bind ADDR          listen address (default 127.0.0.1)\n"
        "  --lan                listen on all interfaces (same as --bind 0.0.0.0)\n"
        "  --modbus-port N      Modbus TCP port (default 502), --no-modbus to disable\n"
        "  --http-port N        web UI + WebSocket port (default 8080), --no-http to disable\n"
        "  --tc4 PORT           TC4 serial emulation: COM20 (Windows+com0com), pty or /dev/tty* (POSIX)\n"
        "  --baud N             TC4 baud rate (default 115200)\n"
        "  --units C|F          temperature units (UIs and loggers; also: console 'units')\n"
        "  --weight-units U     g, kg, oz or lb      --altitude-units m|ft\n"
        "  --time-scale X       simulation speed when no logger is connected (default 1)\n"
        "  --seed N             random seed (default: random)\n"
        "  --data DIR, --web DIR  resource directories\n"
        "  --script FILE        run a scripted roast as fast as possible, print the report, exit\n"
        "  --list               list roaster and bean presets\n"
        "  --quiet              no periodic status line\n"
        "  --sound              play the roaster sound locally (Windows; elsewhere use the web UI)\n");
}

void consoleHelp() {
    std::printf(
        "commands:\n"
        "  state | report | info | list | events\n"
        "  set <control> <pct>        controls: heat fan drum stir voltage cooling\n"
        "  charge [grams] | drop | cool | mark <DRY|FCs|FCe|SCs|SCe>\n"
        "  preheat <C> | empty | reset | repair\n"
        "  load <roaster> [bean]      bean <id>\n"
        "  env <key> <value>          ambient_C line_voltage altitude_m gas_pressure humidity_rh\n"
        "  fault <probe> <open|short|spikes|drift|stuck|noisy|none>\n"
        "  speed <x> | pause | resume\n"
        "  units [C|F] [g|kg|oz|lb] [m|ft]   shared UI + Artisan units (saved)\n"
        "  json {\"cmd\": ...}         raw API command\n"
        "  quit\n");
}

json::Value parseConsole(const std::string& line, bool& handled) {
    std::istringstream ss(line);
    std::string w;
    ss >> w;
    json::Value cmd = json::Value::object();
    handled = true;
    if (w == "json") {
        std::string rest;
        std::getline(ss, rest);
        return json::Value::parse(rest);
    }
    if (w == "state" || w == "report" || w == "list" || w == "events" || w == "drop" || w == "empty" ||
        w == "reset" || w == "repair") {
        cmd["cmd"] = w;
    } else if (w == "info") {
        cmd["cmd"] = "server";
    } else if (w == "cool") {
        cmd["cmd"] = "cool_end";
    } else if (w == "set") {
        std::string c;
        double v = 0;
        ss >> c >> v;
        cmd["cmd"] = "set";
        cmd["control"] = c;
        cmd["value"] = v;
    } else if (w == "charge") {
        cmd["cmd"] = "charge";
        double g = 0;
        if (ss >> g) cmd["batch_g"] = g;
    } else if (w == "mark") {
        std::string e;
        ss >> e;
        cmd["cmd"] = "mark";
        cmd["event"] = e;
    } else if (w == "preheat") {
        double t = 0;
        ss >> t;
        cmd["cmd"] = "preheat";
        cmd["target_C"] = t;
    } else if (w == "load") {
        std::string r, b;
        ss >> r >> b;
        cmd["cmd"] = "load";
        cmd["roaster"] = r;
        if (!b.empty()) cmd["bean"] = b;
    } else if (w == "bean") {
        std::string b;
        ss >> b;
        cmd["cmd"] = "bean";
        cmd["id"] = b;
    } else if (w == "env") {
        std::string k;
        double v = 0;
        ss >> k >> v;
        cmd["cmd"] = "env";
        cmd[k] = v;
    } else if (w == "fault") {
        std::string p, t;
        ss >> p >> t;
        cmd["cmd"] = "fault";
        cmd["probe"] = p;
        cmd["type"] = t;
    } else if (w == "speed") {
        double v = 1;
        ss >> v;
        cmd["cmd"] = "time_scale";
        cmd["value"] = v;
    } else if (w == "pause" || w == "resume") {
        cmd["cmd"] = "pause";
        cmd["value"] = w == "pause";
    } else if (w == "units") {
        // units F lb ft  (any order, any subset)
        cmd["cmd"] = "units";
        std::string u;
        while (ss >> u) {
            std::string l = u;
            for (auto& ch : l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (l == "c" || l == "f") cmd["temperature"] = l;
            else if (l == "m" || l == "ft") cmd["altitude"] = l;
            else cmd["weight"] = l;
        }
    } else {
        handled = false;
    }
    return cmd;
}

std::string mmss(double s) {
    if (s < 0) return "--:--";
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
    return b;
}

int runScript(const std::string& path, const std::string& dataDir) {
    std::string text, err;
    if (!readTextFile(path, text)) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return 2;
    }
    json::Value script = json::Value::parse(text, &err);
    if (!err.empty()) {
        std::fprintf(stderr, "script: %s\n", err.c_str());
        return 2;
    }
    Catalog cat;
    if (!cat.loadDir(dataDir)) {
        std::fprintf(stderr, "cannot load %s\n", dataDir.c_str());
        return 2;
    }
    ScenarioResult res;
    if (!runScenario(script, cat, res)) {
        std::fprintf(stderr, "script failed: %s\n", res.error.c_str());
        return 1;
    }
    for (const auto& e : res.events)
        std::printf("%s  %-6s %s BT %.1f\n", mmss(e.t_roast).c_str(), e.type.c_str(), e.detail.c_str(), e.bt);
    std::printf("%s\n", res.report.toJson().dump(2).c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    HostConfig cfg;
    std::string script;
    bool list = false, quiet = false, sound = false;
    // Like the Win32 app, read roastsim.json next to the program unless --config
    // names another file; unit changes made at runtime are saved back to it.
    bool explicitConfig = false;
    for (int i = 1; i < argc; ++i) explicitConfig |= std::string(argv[i]) == "--config";
    cfg.config_path = platform::executableDir() + "/roastsim.json";
    if (!explicitConfig) {
        std::string text, err;
        if (readTextFile(cfg.config_path, text)) {
            json::Value v = json::Value::parse(text, &err);
            if (err.empty()) cfg = HostConfig::fromJson(v, cfg);
            else std::fprintf(stderr, "warning: %s: %s (ignored)\n", cfg.config_path.c_str(), err.c_str());
        }
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--config") {
            std::string text, err;
            std::string f = next();
            if (!readTextFile(f, text)) { std::fprintf(stderr, "cannot read %s\n", f.c_str()); return 2; }
            json::Value v = json::Value::parse(text, &err);
            if (!err.empty()) { std::fprintf(stderr, "%s: %s\n", f.c_str(), err.c_str()); return 2; }
            cfg = HostConfig::fromJson(v, cfg);
            cfg.config_path = f;
        }
        else if (a == "--weight-units") cfg.weight_units = next();
        else if (a == "--altitude-units") cfg.altitude_units = next();
        else if (a == "--roaster") cfg.roaster = next();
        else if (a == "--bean") cfg.bean = next();
        else if (a == "--bind") cfg.bind_address = next();
        else if (a == "--lan") cfg.bind_address = "0.0.0.0";
        else if (a == "--modbus-port") cfg.modbus_port = std::atoi(next().c_str());
        else if (a == "--no-modbus") cfg.modbus_enabled = false;
        else if (a == "--http-port") cfg.http_port = std::atoi(next().c_str());
        else if (a == "--no-http") cfg.http_enabled = false;
        else if (a == "--tc4") cfg.tc4_port = next();
        else if (a == "--baud") cfg.tc4_baud = std::atoi(next().c_str());
        else if (a == "--units") { std::string u = next(); cfg.units = (!u.empty() && (u[0] == 'F' || u[0] == 'f')) ? 'F' : 'C'; }
        else if (a == "--time-scale") cfg.time_scale = std::atof(next().c_str());
        else if (a == "--seed") cfg.seed = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--data") cfg.data_dir = next();
        else if (a == "--web") cfg.web_dir = next();
        else if (a == "--script") script = next();
        else if (a == "--list") list = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "--sound") sound = true;
        else { std::fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return 2; }
    }
    if (cfg.data_dir.empty()) cfg.data_dir = platform::findResourceDir("data");

    if (!script.empty()) return runScript(script, cfg.data_dir);
    if (list) {
        Catalog cat;
        cat.loadDir(cfg.data_dir);
        std::printf("Roasters:\n");
        for (const auto& r : cat.roasters()) std::printf("  %-22s %s\n", r.id.c_str(), r.name.c_str());
        std::printf("Beans:\n");
        for (const auto& b : cat.beans()) std::printf("  %-22s %s\n", b.id.c_str(), b.name.c_str());
        return 0;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Host host(cfg);
    std::string err;
    if (!host.start(&err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    Snapshot s0 = host.snapshot();
    std::printf("RoastSim: %s / %s\n", s0.roaster_name.c_str(), s0.bean_name.c_str());
    for (const auto& m : host.startupMessages()) std::printf("  %s\n", m.c_str());
    if (cfg.http_enabled)
        std::printf("  Open http://%s:%d/ for the control panel\n",
                    cfg.bind_address == "0.0.0.0" ? "localhost" : cfg.bind_address.c_str(), cfg.http_port);
    AudioSynth synth(host, 44100);
    platform::AudioOutput speaker;
    if (sound) {
        std::string aerr;
        auto fill = [](void* u, float* out, int n) { static_cast<AudioSynth*>(u)->render(out, n); };
        if (speaker.start(44100, fill, &synth, &aerr)) std::printf("  Sound: playing on the default output device\n");
        else std::printf("  Sound: %s\n", aerr.c_str());
    }
    std::printf("Type 'help' for commands, Ctrl+C to quit.\n");

    // Console reader thread (stdin may be closed when run as a service).
    std::thread console([&] {
        std::string line;
        while (!g_quit && std::getline(std::cin, line)) {
            if (line.empty()) continue;
            if (line == "quit" || line == "exit") { g_quit = true; break; }
            if (line == "help" || line == "?") { consoleHelp(); continue; }
            bool handled = false;
            json::Value cmd = parseConsole(line, handled);
            if (!handled || !cmd.isObject()) { std::printf("unknown command (try help)\n"); continue; }
            json::Value r = host.command(cmd, "console");
            std::printf("%s\n", r.dump(2).c_str());
        }
    });
    console.detach();

    auto lastStatus = std::chrono::steady_clock::now();
    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (quiet) continue;
        auto now = std::chrono::steady_clock::now();
        if (now - lastStatus < std::chrono::seconds(5)) continue;
        lastStatus = now;
        Snapshot s = host.snapshot();
        std::printf("[%s] %-8s BT %6.1f  ET %6.1f  RoR %5.1f  %s%s\n", mmss(s.t_roast).c_str(), roastPhaseName(s.phase),
                    s.bt, s.et, s.ror_bt, host.loggerConnected() ? "logger connected" : "no logger",
                    s.beans.present ? ("  Agtron " + json::formatNumber(s.beans.agtron_ground, 1)).c_str() : "");
        std::fflush(stdout);
    }
    std::printf("stopping...\n");
    speaker.stop();
    host.stop();
    return 0;
}
