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
#include "roastsim/mcp.hpp"
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
        "  --presets DIR        custom preset storage directory\n"
        "  --script FILE        run a scripted roast as fast as possible, print the report, exit\n"
        "  --mcp                MCP server on stdin/stdout for AI assistants (see docs/MCP.md): drives the\n"
        "                       RoastSim already running on --http-port if there is one, else runs its own\n"
        "  --mcp-connect H:P    MCP on stdio, always driving the RoastSim at host H, HTTP port P\n"
        "  --mcp-embedded       MCP on stdio, always running its own simulator\n"
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
        "  speed <x> | pause | resume | power [on|off]\n"
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
    } else if (w == "power") {
        cmd["cmd"] = "power";
        std::string v;
        if (ss >> v) {
            for (auto& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (v == "on" || v == "1") cmd["value"] = true;
            else if (v == "off" || v == "0") cmd["value"] = false;
        }
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

// ------------------------------------------------------------ MCP over stdio

// One HTTP/1.1 POST on a fresh connection (the RoastSim web server closes
// every connection after its reply). Returns the status code, or 0 when the
// server could not be reached.
int httpPost(const std::string& host, int port, const std::string& path, const std::string& body, int timeoutMs,
             std::string& reply, std::string& err) {
    platform::socket_t s = platform::connectTcp(host, port, &err);
    if (s == platform::kInvalidSocket) return 0;
    std::string out = "POST " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                      "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    std::string in;
    char buf[16384];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool closed = false;
    while (!closed && std::chrono::steady_clock::now() < deadline) {
        std::vector<platform::PollItem> items(1);
        items[0].s = s;
        items[0].wantWrite = !out.empty();
        platform::pollSockets(items, 100);
        if (items[0].writable && !out.empty()) {
            long n = platform::sendSome(s, out.data(), out.size());
            if (n < 0) break;
            out.erase(0, static_cast<std::size_t>(n));
        }
        if (items[0].readable) {
            long n = platform::recvSome(s, buf, sizeof buf);
            if (n > 0) in.append(buf, static_cast<std::size_t>(n));
            else if (n == 0 || n == -2) closed = true;
        }
    }
    platform::closeSocket(s);
    std::size_t hdrEnd = in.find("\r\n\r\n");
    if (in.compare(0, 5, "HTTP/") != 0 || hdrEnd == std::string::npos) {
        err = closed ? "bad HTTP reply" : "no reply in time";
        return 0;
    }
    reply = in.substr(hdrEnd + 4);
    return std::atoi(in.c_str() + in.find(' ') + 1);
}

void mcpWrite(const json::Value& v) {
    std::string line = v.dump() + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

json::Value mcpError(const json::Value& id, const std::string& msg) {
    json::Value v = json::Value::object();
    v["jsonrpc"] = "2.0";
    v["id"] = id;
    v["error"]["code"] = -32603;
    v["error"]["message"] = msg;
    return v;
}

// Reads JSON-RPC messages, one per line, until stdin closes.
template <typename Handle>
void mcpLoop(Handle handle) {
    std::string line;
    while (!g_quit && std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        std::string err;
        json::Value msg = json::Value::parse(line, &err);
        if (!err.empty()) mcpWrite(McpServer::parseError(err));
        else handle(msg, line);
    }
}

int runMcp(HostConfig cfg, const std::string& connect, bool embeddedOnly) {
    platform::setBinaryStdio();
    // Proxy mode: is a RoastSim (the app, the web server or another roastsimd) already serving?
    std::string host = cfg.bind_address == "0.0.0.0" || cfg.bind_address.empty() ? "127.0.0.1" : cfg.bind_address;
    int port = cfg.http_port;
    if (!connect.empty()) {
        std::size_t colon = connect.rfind(':');
        host = colon == std::string::npos ? connect : connect.substr(0, colon);
        if (colon != std::string::npos) port = std::atoi(connect.c_str() + colon + 1);
    }
    if (!embeddedOnly) {
        platform::netInit();
        std::string reply, err;
        int status = httpPost(host, port, "/mcp", R"({"jsonrpc":"2.0","id":0,"method":"ping"})", 5000, reply, err);
        if (status == 200 && reply.find("\"result\"") != std::string::npos) {
            std::fprintf(stderr, "roastsimd: MCP bridge to the RoastSim at http://%s:%d/mcp\n", host.c_str(), port);
            mcpLoop([&](const json::Value& msg, const std::string& line) {
                std::string body, e;
                int code = httpPost(host, port, "/mcp", line, 120000, body, e);
                bool request = msg.isObject() && msg.has("id") && !msg.get("method", "").empty();
                if (code == 200 || (code >= 400 && body.find("\"jsonrpc\"") != std::string::npos)) {
                    if (!body.empty()) {
                        std::fwrite(body.data(), 1, body.size(), stdout);
                        std::fwrite("\n", 1, 1, stdout);
                        std::fflush(stdout);
                    }
                } else if (request && code != 202) {
                    mcpWrite(mcpError(msg["id"], code == 0 ? "RoastSim at " + host + ":" + std::to_string(port) +
                                                                 " is not reachable (" + e + "). Is it still running?"
                                                           : "RoastSim replied HTTP " + std::to_string(code)));
                }
            });
            return 0;
        }
        if (!connect.empty()) {
            std::fprintf(stderr, "roastsimd: no RoastSim MCP endpoint at http://%s:%d/mcp (%s)\n", host.c_str(), port,
                         status ? ("HTTP " + std::to_string(status)).c_str() : err.c_str());
            return 1;
        }
    }

    // Embedded mode: our own simulator, with the usual servers so a person can
    // watch in the browser and Artisan can log. Nothing but MCP goes to stdout.
    Host sim(cfg);
    std::string err;
    if (!sim.start(&err)) {
        std::fprintf(stderr, "roastsimd: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "roastsimd: MCP server on stdio with its own simulator\n");
    for (const auto& m : sim.startupMessages()) std::fprintf(stderr, "  %s\n", m.c_str());
    McpServer mcp(sim);
    mcpLoop([&](const json::Value& msg, const std::string&) {
        McpServer::Result r = mcp.handle(msg);
        if (r.pending) {
            json::Value reply;
            while (!r.pending(reply)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            mcpWrite(reply);
        } else if (!r.reply.isNull()) {
            mcpWrite(r.reply);
        }
    });
    sim.stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    HostConfig cfg;
    std::string script;
    bool list = false, quiet = false, sound = false;
    bool mcp = false, mcpEmbedded = false;
    std::string mcpConnect;
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
        else if (a == "--presets") cfg.presets_dir = next();
        else if (a == "--script") script = next();
        else if (a == "--mcp") mcp = true;
        else if (a == "--mcp-connect") { mcp = true; mcpConnect = next(); }
        else if (a == "--mcp-embedded") { mcp = true; mcpEmbedded = true; }
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
    if (mcp) return runMcp(cfg, mcpConnect, mcpEmbedded);

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
