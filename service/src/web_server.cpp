// One select() loop for:
//   * Modbus TCP (port 502 by default)
//   * HTTP on one port (8080 by default):
//       GET  /                  -> web UI (files from web_dir)
//       GET  /api/state         -> JSON state       GET /api/list -> roasters & beans
//       POST /api/command       -> JSON command API (body = command object)
//       POST /mcp               -> Model Context Protocol (Streamable HTTP, JSON replies; see docs/MCP.md)
//       GET  <any path> + Upgrade: websocket -> WebSocket
//   * WebSocket messages are JSON text. Two dialects share every socket:
//       {"cmd": ...}          native API (web UI, tools); {"cmd":"subscribe"} streams state
//       {"command": ...}      Artisan's WebSocket device (getData / setControlParams / ...)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>

#include "internal.hpp"
#include "roastsim/audio.hpp"
#include "roastsim/mcp.hpp"
#include "sha1.hpp"

namespace fs = std::filesystem;

namespace roastsim {

namespace {

enum class ConnKind { Modbus, Http, WebSocket };

struct Conn {
    platform::socket_t s = platform::kInvalidSocket;
    ConnKind kind = ConnKind::Http;
    std::string peer;
    std::string in, out;
    std::string fragment;          // WebSocket continuation buffer
    bool closeAfterWrite = false;
    bool dead = false;
    bool subscribed = false;       // UI wants periodic state
    bool artisan = false;          // speaks Artisan's dialect
    bool canSavePresets = false;
    double lastPush = 0;
    std::uint64_t eventSeq = 0;
    std::uint64_t historyNext = 0;
    // Streamed sound (binary PCM16 mono frames), rendered by this connection's own synth.
    std::unique_ptr<AudioSynth> audio;
    double audioStart = 0;
    std::uint64_t audioSent = 0;
    std::vector<float> audioBuf;
    // An MCP tool call still waiting (advance_time in real time); its HTTP reply is sent when it resolves.
    McpServer::Pending mcpPending;
};

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool trustedPresetOrigin(const std::map<std::string, std::string>& headers, const std::string& bindAddress) {
    const auto origin = headers.find("origin");
    if (origin == headers.end()) return true;  // native API clients have no browser origin
    const auto host = headers.find("host");
    if (host == headers.end() || host->second.empty()) return false;
    const std::string authority = lower(host->second);
    const std::string from = lower(origin->second);
    if (from != "http://" + authority && from != "https://" + authority) return false;
    if (bindAddress != "localhost" && bindAddress.compare(0, 4, "127.") != 0) return true;
    const std::string hostname = authority.substr(0, authority.find(':'));
    return hostname == "localhost" || hostname == (bindAddress == "localhost" ? "127.0.0.1" : bindAddress);
}

// MCP over HTTP must reject browser pages from other sites (DNS rebinding):
// a request without Origin is a native client; with one, it must be this
// server's own origin or a page on the local machine (e.g. the MCP Inspector).
bool trustedMcpOrigin(const std::map<std::string, std::string>& headers) {
    const auto origin = headers.find("origin");
    if (origin == headers.end() || origin->second == "null") return origin == headers.end();
    std::string from = lower(origin->second);
    const auto host = headers.find("host");
    if (host != headers.end() && (from == "http://" + lower(host->second) || from == "https://" + lower(host->second)))
        return true;
    std::size_t scheme = from.find("://");
    if (scheme == std::string::npos) return false;
    std::string authority = from.substr(scheme + 3);
    if (authority.empty()) return false;
    std::string hostname = authority[0] == '[' ? authority.substr(0, authority.find(']') + 1)
                                               : authority.substr(0, authority.find(':'));
    return hostname == "localhost" || hostname == "127.0.0.1" || hostname == "[::1]";
}

json::Value presetOriginError() {
    json::Value response = json::Value::object();
    response["ok"] = false;
    response["error"] = "Open the RoastSim control panel to save a preset.";
    return response;
}

std::string mimeFor(const std::string& path) {
    std::string ext = lower(fs::path(path).extension().string());
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".wav") return "audio/wav";
    return "application/octet-stream";
}

std::string httpResponse(int code, const std::string& status, const std::string& type, const std::string& body) {
    std::string r = "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n";
    r += "Content-Type: " + type + "\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "Cache-Control: no-cache\r\n";
    r += "Access-Control-Allow-Origin: *\r\n";
    r += "Connection: close\r\n\r\n";
    r += body;
    return r;
}

std::string wsFrame(const std::string& payload, std::uint8_t opcode = 1) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | opcode));
    std::size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>(n));
    } else if (n < 65536) {
        f.push_back(126);
        f.push_back(static_cast<char>((n >> 8) & 0xFF));
        f.push_back(static_cast<char>(n & 0xFF));
    } else {
        f.push_back(127);
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((static_cast<std::uint64_t>(n) >> (i * 8)) & 0xFF));
    }
    f += payload;
    return f;
}

}  // namespace

// ---------------------------------------------------------------- impl
struct NetServer::Impl {
    Host& host;
    std::map<platform::socket_t, std::unique_ptr<Conn>> conns;
    McpServer mcp;
    explicit Impl(Host& h) : host(h), mcp(h) {}

    void handleHttp(Conn& c);
    void handleMcp(Conn& c, const std::string& method, const std::map<std::string, std::string>& headers,
                   const std::string& body);
    void handleWsData(Conn& c);
    void handleWsMessage(Conn& c, const std::string& text);
    void handleArtisan(Conn& c, const json::Value& msg);
    void periodic(Conn& c, double now);
    json::Value artisanData();
};

json::Value NetServer::Impl::artisanData() {
    Snapshot s = host.snapshot();
    json::Value d = json::Value::object();
    auto t = [&](double c) {
        double v = host.toLoggerUnits(c);
        return json::Value(std::round(v * 100.0) / 100.0);
    };
    for (const auto& p : s.probes) d[p.id] = t(p.value);
    d["ambient"] = t(s.env.ambient_C);
    double rf = host.units() == 'F' ? 1.8 : 1.0;
    d["RoR_BT"] = std::round(s.ror_bt * rf * 100.0) / 100.0;
    d["RoR_ET"] = std::round(s.ror_et * rf * 100.0) / 100.0;
    for (int i = 0; i < kControlCount; ++i)
        if (s.has_control[i]) d[controlName(static_cast<Control>(i))] = std::round(s.controls[i] * 10.0) / 10.0;
    d["state"] = roastPhaseName(s.phase);
    d["roast_time"] = s.t_roast >= 0 ? std::round(s.t_roast * 10.0) / 10.0 : -1.0;
    if (s.beans.present) {
        d["bean_true"] = t(s.beans.T_mean);
        d["agtron"] = std::round(s.beans.agtron_ground * 10.0) / 10.0;
        d["moisture"] = std::round(s.beans.moisture_pct * 100.0) / 100.0;
    }
    d["heater_W"] = std::round(s.tele.heater_W);
    d["smoke"] = std::round(s.smoke * 1000.0) / 10.0;
    return d;
}

void NetServer::Impl::handleArtisan(Conn& c, const json::Value& msg) {
    const ArtisanWsConfig& w = host.config().ws;
    c.artisan = true;
    host.noteLoggerActivity("artisan-websocket");
    const std::string command = msg.get(w.command_node, "");
    json::Value reply = json::Value::object();
    bool hasId = msg.has(w.id_node);
    if (hasId) reply[w.id_node] = msg[w.id_node];
    if (msg.has(w.machine_node)) reply[w.machine_node] = msg[w.machine_node];

    if (command == w.request_data) {
        reply[w.data_node] = artisanData();
    } else if (command == "setControlParams" || command == "set") {
        const json::Value& params = msg["params"];
        for (const auto& k : params.keys()) {
            Control ctl;
            if (controlFromName(k, ctl)) host.setControl(ctl, params[k].num(), "artisan-ws");
        }
        reply[w.data_node] = json::Value::object();
    } else if (!command.empty()) {
        // Anything else maps onto the native API: {"command":"charge","params":{"batch_g":120}}
        json::Value cmd = msg["params"].isObject() ? msg["params"] : json::Value::object();
        std::string name = command;
        if (name == "startRoasting") name = "charge";
        if (name == "endRoasting") name = "drop";
        cmd["cmd"] = name;
        json::Value r = host.command(cmd, "artisan-ws");
        reply[w.data_node] = r;
    }
    if (hasId) c.out += wsFrame(reply.dump());
}

void NetServer::Impl::handleWsMessage(Conn& c, const std::string& text) {
    // Some clients batch several JSON objects separated by newlines.
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        start = nl == std::string::npos ? text.size() : nl + 1;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        std::string err;
        json::Value msg = json::Value::parse(line, &err);
        if (!err.empty() || !msg.isObject()) {
            json::Value e = json::Value::object();
            e["type"] = "error";
            e["error"] = "bad JSON: " + err;
            c.out += wsFrame(e.dump());
            continue;
        }
        if (msg.has(host.config().ws.command_node) && !msg.has("cmd")) {
            handleArtisan(c, msg);
            continue;
        }
        std::string cmd = msg.get("cmd", "");
        json::Value reply;
        if (cmd == "preset_save" && !c.canSavePresets) {
            reply = presetOriginError();
        } else if (cmd == "audio") {
            // {"cmd":"audio","value":true,"rate":24000} starts the sound stream; false stops it.
            reply = json::Value::object();
            if (msg.getBool("value", true)) {
                int rate = static_cast<int>(clampd(msg.get("rate", 24000.0), 8000.0, 48000.0));
                c.audio = std::make_unique<AudioSynth>(host, rate);
                c.audioStart = nowSeconds();
                c.audioSent = 0;
                reply["rate"] = rate;
            } else {
                c.audio.reset();
            }
            reply["ok"] = true;
        } else if (cmd == "audio_mix") {
            if (c.audio) {
                AudioMix m = c.audio->mix();
                m.master = static_cast<float>(msg.get("master", m.master));
                m.machine = static_cast<float>(msg.get("machine", m.machine));
                m.cracks = static_cast<float>(msg.get("cracks", m.cracks));
                c.audio->setMix(m);
            }
            reply = json::Value::object();
            reply["ok"] = true;
        } else if (cmd == "subscribe") {
            c.subscribed = msg.getBool("value", true);
            c.eventSeq = static_cast<std::uint64_t>(msg.get("event_seq", 0.0));
            c.historyNext = static_cast<std::uint64_t>(msg.get("history_from", 0.0));
            reply = json::Value::object();
            reply["ok"] = true;
        } else {
            reply = host.command(msg, msg.get("origin", "web"));
        }
        reply["type"] = reply.get("type", "reply");
        if (msg.has("req")) reply["req"] = msg["req"];
        reply["cmd"] = cmd;
        c.out += wsFrame(reply.dump());
    }
}

void NetServer::Impl::handleWsData(Conn& c) {
    for (;;) {
        if (c.in.size() < 2) return;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(c.in.data());
        bool fin = (p[0] & 0x80) != 0;
        std::uint8_t op = p[0] & 0x0F;
        bool masked = (p[1] & 0x80) != 0;
        std::uint64_t len = p[1] & 0x7F;
        std::size_t hdr = 2;
        if (len == 126) {
            if (c.in.size() < 4) return;
            len = (std::uint64_t(p[2]) << 8) | p[3];
            hdr = 4;
        } else if (len == 127) {
            if (c.in.size() < 10) return;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
            hdr = 10;
        }
        if (len > (16u << 20)) { c.dead = true; return; }
        std::size_t maskOff = hdr;
        if (masked) hdr += 4;
        if (c.in.size() < hdr + len) return;
        std::string payload = c.in.substr(hdr, static_cast<std::size_t>(len));
        if (masked)
            for (std::size_t i = 0; i < payload.size(); ++i) payload[i] ^= c.in[maskOff + (i % 4)];
        c.in.erase(0, hdr + static_cast<std::size_t>(len));

        switch (op) {
            case 0x0:  // continuation
            case 0x1:  // text
            case 0x2:  // binary (treated as text JSON)
                c.fragment += payload;
                if (fin) {
                    std::string msg;
                    msg.swap(c.fragment);
                    handleWsMessage(c, msg);
                }
                break;
            case 0x8:  // close
                c.out += wsFrame(payload.substr(0, 2), 0x8);
                c.closeAfterWrite = true;
                return;
            case 0x9:  // ping
                c.out += wsFrame(payload, 0xA);
                break;
            default: break;  // pong etc.
        }
    }
}

void NetServer::Impl::handleHttp(Conn& c) {
    std::size_t hdrEnd = c.in.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) {
        if (c.in.size() > 65536) c.dead = true;
        return;
    }
    std::string head = c.in.substr(0, hdrEnd);
    std::map<std::string, std::string> headers;
    std::size_t lineEnd = head.find("\r\n");
    std::string reqLine = head.substr(0, lineEnd);
    std::size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        std::size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) e = head.size();
        std::string line = head.substr(pos, e - pos);
        std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string v = line.substr(colon + 1);
            v.erase(0, v.find_first_not_of(" \t"));
            headers[lower(line.substr(0, colon))] = v;
        }
        pos = e + 2;
    }
    std::size_t contentLen = headers.count("content-length") ? std::strtoul(headers["content-length"].c_str(), nullptr, 10) : 0;
    if (contentLen > (4u << 20)) { c.dead = true; return; }
    if (c.in.size() < hdrEnd + 4 + contentLen) return;
    std::string body = c.in.substr(hdrEnd + 4, contentLen);
    c.in.erase(0, hdrEnd + 4 + contentLen);

    std::string method = reqLine.substr(0, reqLine.find(' '));
    std::size_t p1 = reqLine.find(' ');
    std::size_t p2 = reqLine.find(' ', p1 + 1);
    std::string target = p1 == std::string::npos ? "/" : reqLine.substr(p1 + 1, p2 - p1 - 1);
    std::string path = target.substr(0, target.find('?'));

    // WebSocket upgrade on any path (Artisan defaults to "/WebSocket", the UI uses "/ws").
    if (lower(headers["upgrade"]) == "websocket" && headers.count("sec-websocket-key")) {
        std::string accept = detail::base64(detail::sha1(headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
        c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        // No Sec-WebSocket-Extensions in the reply: permessage-deflate is declined.
        c.kind = ConnKind::WebSocket;
        c.canSavePresets = trustedPresetOrigin(headers, host.config().bind_address);
        c.eventSeq = host.snapshot().last_event_seq;  // only push events from now on
        if (!c.in.empty()) handleWsData(c);
        return;
    }

    if (method == "OPTIONS") {
        c.out += "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\n"
                 "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                 "Access-Control-Allow-Headers: Content-Type, Accept, Mcp-Protocol-Version, Mcp-Session-Id\r\n"
                 "Content-Length: 0\r\nConnection: close\r\n\r\n";
    } else if (path == "/mcp") {
        handleMcp(c, method, headers, body);
        if (c.mcpPending) return;  // the reply is sent from periodic() when the wait ends
    } else if (path == "/api/state") {
        c.out += httpResponse(200, "OK", "application/json", host.stateJson(true).dump());
    } else if (path == "/api/list") {
        json::Value cmd = json::Value::object();
        cmd["cmd"] = "list";
        c.out += httpResponse(200, "OK", "application/json", host.command(cmd, "http").dump());
    } else if (path == "/api/command" && method == "POST") {
        std::string err;
        json::Value cmd = json::Value::parse(body, &err);
        json::Value r = json::Value::object();
        if (!err.empty()) {
            r["ok"] = false;
            r["error"] = "bad JSON: " + err;
        } else if (cmd.get("cmd", "") == "preset_save" && !trustedPresetOrigin(headers, host.config().bind_address))
            r = presetOriginError();
        else r = host.command(cmd, cmd.get("origin", "http"));
        c.out += httpResponse(200, "OK", "application/json", r.dump());
    } else if (method == "GET" || method == "HEAD") {
        std::string rel = path == "/" ? "index.html" : path.substr(1);
        if (rel.find("..") != std::string::npos || rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos) {
            c.out += httpResponse(400, "Bad Request", "text/plain", "bad path");
        } else {
            std::string file = (fs::u8path(host.config().web_dir) / fs::u8path(rel)).u8string();
            std::string data;
            if (readTextFile(file, data)) {
                c.out += httpResponse(200, "OK", mimeFor(file), method == "HEAD" ? std::string() : data);
            } else {
                c.out += httpResponse(404, "Not Found", "text/plain", "not found: " + path);
            }
        }
    } else {
        c.out += httpResponse(405, "Method Not Allowed", "text/plain", "method not allowed");
    }
    c.closeAfterWrite = true;
}

void NetServer::Impl::handleMcp(Conn& c, const std::string& method,
                                const std::map<std::string, std::string>& headers, const std::string& body) {
    if (!trustedMcpOrigin(headers)) {
        c.out += httpResponse(403, "Forbidden", "text/plain", "MCP requests from this origin are not allowed");
        return;
    }
    if (method != "POST") {
        // No server-to-client SSE stream and no sessions: POST only.
        c.out += "HTTP/1.1 405 Method Not Allowed\r\nAllow: POST, OPTIONS\r\nAccess-Control-Allow-Origin: *\r\n"
                 "Content-Length: 0\r\nConnection: close\r\n\r\n";
        return;
    }
    std::string err;
    json::Value msg = json::Value::parse(body, &err);
    if (!err.empty()) {
        c.out += httpResponse(400, "Bad Request", "application/json", McpServer::parseError(err).dump());
        return;
    }
    McpServer::Result r = mcp.handle(msg);
    if (r.pending) c.mcpPending = std::move(r.pending);
    else if (r.reply.isNull()) c.out += httpResponse(202, "Accepted", "application/json", "");
    else c.out += httpResponse(200, "OK", "application/json", r.reply.dump());
}

void NetServer::Impl::periodic(Conn& c, double now) {
    if (c.mcpPending) {
        json::Value reply;
        if (c.mcpPending(reply)) {
            c.mcpPending = nullptr;
            c.out += httpResponse(200, "OK", "application/json", reply.dump());
            c.closeAfterWrite = true;
        }
        return;
    }
    if (c.kind != ConnKind::WebSocket) return;
    if (c.audio) {
        // Keep ~150 ms rendered ahead of real time; the browser schedules the chunks.
        const int rate = c.audio->sampleRate();
        double target = (now - c.audioStart + 0.15) * rate;
        if (target - static_cast<double>(c.audioSent) > rate)  // stalled for a second: skip ahead
            c.audioSent = static_cast<std::uint64_t>(target) - static_cast<std::uint64_t>(rate / 4);
        int frames = static_cast<int>(target - static_cast<double>(c.audioSent));
        if (frames >= rate / 50 && c.out.size() < 256 * 1024) {
            c.audioBuf.resize(static_cast<std::size_t>(frames));
            c.audio->render(c.audioBuf.data(), frames);
            std::string pcm(static_cast<std::size_t>(frames) * 2, '\0');
            for (int i = 0; i < frames; ++i) {
                float v = std::max(-1.0f, std::min(1.0f, c.audioBuf[static_cast<std::size_t>(i)]));
                auto s = static_cast<std::int16_t>(v * 32767.0f);
                pcm[static_cast<std::size_t>(i) * 2] = static_cast<char>(s & 0xFF);
                pcm[static_cast<std::size_t>(i) * 2 + 1] = static_cast<char>((s >> 8) & 0xFF);
            }
            c.out += wsFrame(pcm, 0x2);
            c.audioSent += static_cast<std::uint64_t>(frames);
        }
    }
    // Events: UI subscribers get them all; Artisan gets CHARGE/DROP and crack marks.
    if (c.subscribed || c.artisan) {
        auto evs = host.eventsSince(c.eventSeq);
        const ArtisanWsConfig& w = host.config().ws;
        for (const auto& e : evs) {
            c.eventSeq = e.seq;
            if (c.subscribed) {
                json::Value v = json::Value::object();
                v["type"] = "event";
                v["seq"] = static_cast<double>(e.seq);
                v["event"] = e.type;
                v["detail"] = e.detail;
                v["origin"] = e.origin;
                v["t"] = e.t_session;
                v["t_roast"] = e.t_roast;
                v["bt"] = e.bt;
                v["power"] = e.powered;
                c.out += wsFrame(v.dump());
            }
            if (c.artisan && e.origin != "artisan-ws") {
                json::Value v = json::Value::object();
                if (w.push_charge_drop && e.type == "CHARGE") v[w.push_node] = w.charge_message;
                else if (w.push_charge_drop && e.type == "DROP") v[w.push_node] = w.drop_message;
                else if (w.push_crack_events) {
                    std::string node;
                    if (e.type == "DRY") node = w.dry_event;
                    else if (e.type == "FCs") node = w.fcs_event;
                    else if (e.type == "FCe") node = w.fce_event;
                    else if (e.type == "SCs") node = w.scs_event;
                    else if (e.type == "SCe") node = w.sce_event;
                    if (!node.empty()) {
                        v[w.push_node] = w.add_event_message;
                        json::Value d = json::Value::object();
                        d[w.event_node] = node;
                        v[w.data_node] = d;
                    }
                }
                if (v.isObject() && v.size() > 0) c.out += wsFrame(v.dump());
            }
        }
    }
    if (c.subscribed && now - c.lastPush >= 0.25) {
        c.lastPush = now;
        json::Value st = host.stateJson(false);
        std::uint64_t next = 0;
        auto hist = host.historySince(c.historyNext, &next, 2000);
        if (!hist.empty()) {
            json::Value arr = json::Value::array();
            for (const auto& h : hist) {
                json::Value a = json::Value::array();
                a.push(std::round(h.t * 10.0f) / 10.0); a.push(std::round(h.t_roast * 10.0f) / 10.0);
                a.push(std::round(h.bt * 100.0f) / 100.0); a.push(std::round(h.et * 100.0f) / 100.0);
                a.push(std::round(h.ror_bt * 100.0f) / 100.0); a.push(std::round(h.ror_et * 100.0f) / 100.0);
                a.push(std::round(h.bean_true * 100.0f) / 100.0); a.push(std::round(h.heat * 10.0f) / 10.0);
                a.push(std::round(h.fan * 10.0f) / 10.0); a.push(std::round(h.drum * 10.0f) / 10.0);
                arr.push(a);
            }
            st["history"] = arr;
            st["history_from"] = static_cast<double>(c.historyNext);
            c.historyNext = next;
        }
        c.out += wsFrame(st.dump());
    }
}

// ---------------------------------------------------------------- server

bool NetServer::start(std::string* err, std::vector<std::string>& messages) {
    const HostConfig& cfg = host_.config();
    std::string e;
    if (cfg.modbus_enabled) {
        modbusListener_ = platform::listenTcp(cfg.bind_address, cfg.modbus_port, &e);
        modbusStatus_ = modbusListener_ != platform::kInvalidSocket
                            ? "listening on " + cfg.bind_address + ":" + std::to_string(cfg.modbus_port)
                            : "failed: " + e;
        messages.push_back("Modbus TCP: " + modbusStatus_);
    }
    if (cfg.http_enabled) {
        httpListener_ = platform::listenTcp(cfg.bind_address, cfg.http_port, &e);
        httpStatus_ = httpListener_ != platform::kInvalidSocket
                          ? "listening on " + cfg.bind_address + ":" + std::to_string(cfg.http_port)
                          : "failed: " + e;
        messages.push_back("Web UI / WebSocket: " + httpStatus_);
    }
    if (modbusListener_ == platform::kInvalidSocket && httpListener_ == platform::kInvalidSocket) {
        if (err) *err = "no network listener could be started";
        return false;
    }
    stop_ = false;
    thread_ = std::thread([this] { loop(); });
    return true;
}

void NetServer::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    platform::closeSocket(modbusListener_);
    platform::closeSocket(httpListener_);
    modbusListener_ = httpListener_ = platform::kInvalidSocket;
}

json::Value NetServer::info() const {
    std::lock_guard<std::mutex> lk(infoMu_);
    json::Value v = json::Value::object();
    v["modbus"] = modbusStatus_;
    v["http"] = httpStatus_;
    v["modbus_clients"] = modbusClients_;
    v["http_clients"] = httpClients_;
    v["ws_ui_clients"] = wsUiClients_;
    v["ws_artisan_clients"] = wsArtisanClients_;
    return v;
}

void NetServer::loop() {
    Impl impl(host_);
    std::vector<platform::PollItem> items;
    char buf[16384];
    while (!stop_) {
        items.clear();
        if (modbusListener_ != platform::kInvalidSocket) items.push_back({modbusListener_});
        if (httpListener_ != platform::kInvalidSocket) items.push_back({httpListener_});
        for (auto& kv : impl.conns) {
            platform::PollItem it;
            it.s = kv.first;
            it.wantWrite = !kv.second->out.empty();
            items.push_back(it);
        }
        platform::pollSockets(items, 50);
        double now = nowSeconds();

        for (auto& it : items) {
            if (it.s == modbusListener_ || it.s == httpListener_) {
                if (!it.readable) continue;
                std::string peer;
                platform::socket_t c = platform::acceptClient(it.s, &peer);
                if (c == platform::kInvalidSocket) continue;
                auto conn = std::make_unique<Conn>();
                conn->s = c;
                conn->peer = peer;
                conn->kind = it.s == modbusListener_ ? ConnKind::Modbus : ConnKind::Http;
                impl.conns[c] = std::move(conn);
                continue;
            }
            auto found = impl.conns.find(it.s);
            if (found == impl.conns.end()) continue;
            Conn& c = *found->second;
            if (it.readable) {
                long n = platform::recvSome(c.s, buf, sizeof buf);
                if (n > 0) {
                    c.in.append(buf, static_cast<std::size_t>(n));
                    if (c.kind == ConnKind::Modbus) {
                        if (!modbusProcess(host_, c.in, c.out)) c.dead = true;
                    } else if (c.kind == ConnKind::Http) {
                        impl.handleHttp(c);
                    } else {
                        impl.handleWsData(c);
                    }
                } else if (n == 0 || n == -2) {
                    c.dead = true;
                }
            }
        }
        // Periodic pushes, then flush.
        int nModbus = 0, nHttp = 0, nUi = 0, nArt = 0;
        for (auto& kv : impl.conns) {
            Conn& c = *kv.second;
            if (c.dead) continue;
            impl.periodic(c, now);
            if (!c.out.empty()) {
                long n = platform::sendSome(c.s, c.out.data(), c.out.size());
                if (n > 0) c.out.erase(0, static_cast<std::size_t>(n));
                else if (n < 0) c.dead = true;
                if (c.out.size() > (8u << 20)) c.dead = true;  // client not reading
            }
            if (c.closeAfterWrite && c.out.empty()) c.dead = true;
            if (c.kind == ConnKind::Modbus) ++nModbus;
            else if (c.kind == ConnKind::Http) ++nHttp;
            else if (c.artisan) ++nArt;
            else ++nUi;
            if (c.artisan) host_.noteLoggerActivity("artisan-websocket");
        }
        for (auto iter = impl.conns.begin(); iter != impl.conns.end();) {
            if (iter->second->dead) {
                platform::closeSocket(iter->first);
                iter = impl.conns.erase(iter);
            } else {
                ++iter;
            }
        }
        std::lock_guard<std::mutex> lk(infoMu_);
        modbusClients_ = nModbus;
        httpClients_ = nHttp;
        wsUiClients_ = nUi;
        wsArtisanClients_ = nArt;
    }
    for (auto& kv : impl.conns) platform::closeSocket(kv.first);
}

}  // namespace roastsim
