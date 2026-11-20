// Service tests: a live Host on loopback, spoken to the way Artisan does.
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "../service/src/internal.hpp"
#include "../service/src/sha1.hpp"
#include "roastsim/audio.hpp"
#include "roastsim/host.hpp"
#include "roastsim/platform.hpp"
#include "roastsim/roastsim.h"
#include "test_util.hpp"

using namespace roastsim;
using namespace std::chrono_literals;

namespace {

int basePort() {
    static int p = 20000 + static_cast<int>(std::chrono::steady_clock::now().time_since_epoch().count() % 20000);
    return p;
}

std::unique_ptr<Host> startHost(const char* roaster = "popper_modded") {
    HostConfig cfg;
    cfg.data_dir = rstest::dataDir();
    cfg.web_dir = rstest::dataDir() + "/../web";
    cfg.modbus_port = basePort();
    cfg.http_port = basePort() + 1;
    cfg.roaster = roaster;
    cfg.seed = 7;
    auto h = std::make_unique<Host>(cfg);
    std::string err;
    if (!h->start(&err)) std::printf("    host start failed: %s\n", err.c_str());
    std::this_thread::sleep_for(100ms);
    return h;
}

// Reads until `n` bytes are buffered or the timeout passes.
std::string recvAtLeast(platform::socket_t s, std::size_t n, int timeoutMs = 2000) {
    std::string buf;
    char tmp[4096];
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (buf.size() < n && std::chrono::steady_clock::now() < end) {
        std::vector<platform::PollItem> it{{s}};
        platform::pollSockets(it, 50);
        long r = platform::recvSome(s, tmp, sizeof tmp);
        if (r > 0) buf.append(tmp, static_cast<std::size_t>(r));
        else if (r == 0 || r == -2) break;
    }
    return buf;
}

void sendAll(platform::socket_t s, const std::string& d) {
    std::size_t off = 0;
    while (off < d.size()) {
        long n = platform::sendSome(s, d.data() + off, d.size() - off);
        if (n < 0) return;
        off += static_cast<std::size_t>(n);
        if (n == 0) std::this_thread::sleep_for(1ms);
    }
}

std::string mbap(std::uint16_t tid, std::uint8_t unit, const std::string& pdu) {
    std::string f;
    f.push_back(static_cast<char>(tid >> 8));
    f.push_back(static_cast<char>(tid & 0xFF));
    f.push_back(0);
    f.push_back(0);
    std::uint16_t len = static_cast<std::uint16_t>(pdu.size() + 1);
    f.push_back(static_cast<char>(len >> 8));
    f.push_back(static_cast<char>(len & 0xFF));
    f.push_back(static_cast<char>(unit));
    return f + pdu;
}

std::string pdu(std::initializer_list<int> bytes) {
    std::string s;
    for (int b : bytes) s.push_back(static_cast<char>(b));
    return s;
}

int reg(const std::string& resp, int i) {  // signed int16 register i from an FC3/FC4 response
    std::size_t o = 9 + static_cast<std::size_t>(i) * 2;
    return static_cast<std::int16_t>((static_cast<unsigned char>(resp[o]) << 8) | static_cast<unsigned char>(resp[o + 1]));
}

// --- WebSocket client helpers
std::string wsMasked(const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x81));
    std::size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>(0x80 | n));
    } else {
        f.push_back(static_cast<char>(0x80 | 126));
        f.push_back(static_cast<char>(n >> 8));
        f.push_back(static_cast<char>(n & 0xFF));
    }
    const char mask[4] = {0x12, 0x34, 0x56, 0x78};
    f.append(mask, 4);
    for (std::size_t i = 0; i < n; ++i) f.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    return f;
}

// Pulls complete text frames out of buf.
std::vector<std::string> wsFrames(std::string& buf) {
    std::vector<std::string> out;
    for (;;) {
        if (buf.size() < 2) break;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(buf.data());
        std::size_t len = p[1] & 0x7F, hdr = 2;
        if (len == 126) {
            if (buf.size() < 4) break;
            len = (std::size_t(p[2]) << 8) | p[3];
            hdr = 4;
        } else if (len == 127) {
            if (buf.size() < 10) break;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
            hdr = 10;
        }
        if (buf.size() < hdr + len) break;
        out.push_back(buf.substr(hdr, len));
        buf.erase(0, hdr + len);
    }
    return out;
}

platform::socket_t wsConnect(int port, const std::string& path) {
    std::string err;
    platform::socket_t s = platform::connectTcp("127.0.0.1", port, &err);
    if (s == platform::kInvalidSocket) return s;
    sendAll(s, "GET /" + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
               "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n");
    std::string resp = recvAtLeast(s, 20);
    while (resp.find("\r\n\r\n") == std::string::npos) {
        std::string more = recvAtLeast(s, 1, 500);
        if (more.empty()) break;
        resp += more;
    }
    CHECK(resp.find("101 Switching Protocols") != std::string::npos);
    // RFC 6455 sample key -> known accept value
    CHECK(resp.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    CHECK(resp.find("permessage-deflate") == std::string::npos);  // compression declined
    return s;
}

// Waits for a frame whose JSON satisfies pred.
json::Value wsWait(platform::socket_t s, std::string& buf, const std::function<bool(const json::Value&)>& pred,
                   int timeoutMs = 3000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        buf += recvAtLeast(s, 1, 100);
        for (const auto& f : wsFrames(buf)) {
            json::Value v = json::Value::parse(f);
            if (pred(v)) return v;
        }
    }
    return json::Value();
}

}  // namespace

TEST(sha1_base64_rfc6455) {
    CHECK(detail::base64(detail::sha1("dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11")) ==
          "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CHECK(detail::base64("ab") == "YWI=");
}

TEST(modbus_read_write) {
    auto host = startHost();
    std::string err;
    platform::socket_t s = platform::connectTcp("127.0.0.1", basePort(), &err);
    CHECK(s != platform::kInvalidSocket);
    if (s == platform::kInvalidSocket) return;
    // FC3 read 0..23 (what Artisan's optimizer would batch)
    sendAll(s, mbap(1, 1, pdu({3, 0, 0, 0, 24})));
    std::string r = recvAtLeast(s, 9 + 48);
    CHECK(r.size() == 9 + 48);
    CHECK(r[7] == 3 && static_cast<unsigned char>(r[8]) == 48);
    CHECK_RANGE(reg(r, 0) / 10.0, 10, 40);  // BT at ambient
    CHECK_RANGE(reg(r, 4) / 10.0, 21, 23);  // ambient register
    // FC6 write heat = 75 to register 100, then read it back with FC4 at register 7
    sendAll(s, mbap(2, 1, pdu({6, 0, 100, 0, 75})));
    std::string w = recvAtLeast(s, 12);
    CHECK(w.size() == 12 && w.substr(7) == pdu({6, 0, 100, 0, 75}));
    sendAll(s, mbap(3, 1, pdu({4, 0, 7, 0, 1})));
    std::string rb = recvAtLeast(s, 11);
    CHECK(rb.size() == 11 && reg(rb, 0) == 75);
    // FC16 write fan=60 at 101 and command CHARGE via register 110 in two requests
    sendAll(s, mbap(4, 1, pdu({16, 0, 101, 0, 1, 2, 0, 60})));
    CHECK(recvAtLeast(s, 12).size() == 12);
    sendAll(s, mbap(5, 1, pdu({6, 0, 110, 0, 1})));
    CHECK(recvAtLeast(s, 12).size() == 12);
    CHECK(host->snapshot().phase == RoastPhase::Roasting);
    CHECK(host->snapshot().controls[static_cast<int>(Control::Fan)] == 60);
    // Illegal address -> exception 0x83 code 2
    sendAll(s, mbap(6, 1, pdu({3, 0, 250, 0, 2})));
    std::string ex = recvAtLeast(s, 9);
    CHECK(ex.size() == 9 && static_cast<unsigned char>(ex[7]) == 0x83 && ex[8] == 2);
    // Two requests in one TCP segment are both answered
    sendAll(s, mbap(7, 1, pdu({3, 0, 0, 0, 2})) + mbap(8, 1, pdu({3, 0, 1, 0, 1})));
    CHECK(recvAtLeast(s, 13 + 11).size() == 24);
    CHECK(host->loggerConnected());
    platform::closeSocket(s);
    host->stop();
}

TEST(artisan_websocket_dialect) {
    auto host = startHost();
    {   // The roaster must be switched on before its heater does anything.
        json::Value pw = json::Value::object();
        pw["cmd"] = "power";
        pw["value"] = true;
        host->command(pw, "test");
    }
    platform::socket_t s = wsConnect(basePort() + 1, "WebSocket");
    if (s == platform::kInvalidSocket) return;
    std::string buf;
    // Exactly what Artisan sends each sample.
    sendAll(s, wsMasked(R"({"command":"getData","id":4711,"roasterID":0})"));
    json::Value r = wsWait(s, buf, [](const json::Value& v) { return v.get("id", 0.0) == 4711; });
    CHECK(r["data"].has("BT"));
    CHECK(r["data"].has("ET"));
    CHECK(r.get("roasterID", -1.0) == 0);
    // Slider command
    sendAll(s, wsMasked(R"({"command":"setControlParams","params":{"burner":80,"fan":70},"id":12,"roasterID":0})"));
    wsWait(s, buf, [](const json::Value& v) { return v.get("id", 0.0) == 12; });
    CHECK(host->snapshot().controls[static_cast<int>(Control::Heat)] == 80);
    CHECK(host->snapshot().controls[static_cast<int>(Control::Fan)] == 70);
    // CHARGE from another front end -> push message to Artisan, without an id
    json::Value c = json::Value::object();
    c["cmd"] = "charge";
    host->command(c, "ui");
    json::Value push = wsWait(s, buf, [](const json::Value& v) { return v.has("pushMessage"); });
    CHECK(push.get("pushMessage", "") == "startRoasting");
    CHECK(!push.has("id"));
    // Physics-detected DRY -> addEvent with Artisan's default event node name
    json::Value step = json::Value::object();
    step["cmd"] = "step";
    step["seconds"] = 240;
    host->command(step, "test");
    json::Value ev = wsWait(s, buf, [](const json::Value& v) { return v.get("pushMessage", "") == "addEvent"; }, 4000);
    CHECK(ev["data"].get("event", "") == "colorChangeEvent");
    // Artisan-triggered DROP is not echoed back
    sendAll(s, wsMasked(R"({"command":"endRoasting","id":13,"roasterID":0})"));
    wsWait(s, buf, [](const json::Value& v) { return v.get("id", 0.0) == 13; });
    CHECK(host->snapshot().phase == RoastPhase::Cooling);
    json::Value echo = wsWait(s, buf, [](const json::Value& v) { return v.get("pushMessage", "") == "endRoasting"; }, 600);
    CHECK(echo.isNull());
    platform::closeSocket(s);
    host->stop();
}

TEST(ui_websocket_and_http) {
    auto host = startHost("drum_home_1kg");
    platform::socket_t s = wsConnect(basePort() + 1, "ws");
    if (s == platform::kInvalidSocket) return;
    std::string buf;
    sendAll(s, wsMasked(R"({"cmd":"subscribe","history_from":0,"req":1})"));
    json::Value st = wsWait(s, buf, [](const json::Value& v) { return v.get("type", "") == "state"; });
    CHECK(st["roaster"].get("id", "") == "drum_home_1kg");
    sendAll(s, wsMasked(R"({"cmd":"list","req":2})"));
    json::Value l = wsWait(s, buf, [](const json::Value& v) { return v.get("req", 0.0) == 2; });
    CHECK(l["roasters"].size() >= 8);
    sendAll(s, wsMasked(R"({"cmd":"nonsense","req":3})"));
    json::Value bad = wsWait(s, buf, [](const json::Value& v) { return v.get("req", 0.0) == 3; });
    CHECK(!bad.getBool("ok", true));
    platform::closeSocket(s);

    // Plain HTTP API
    std::string err;
    platform::socket_t h = platform::connectTcp("127.0.0.1", basePort() + 1, &err);
    std::string body = R"({"cmd":"set","control":"drum","value":40})";
    sendAll(h, "POST /api/command HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: " +
                   std::to_string(body.size()) + "\r\n\r\n" + body);
    std::string resp = recvAtLeast(h, 1000, 1500);
    CHECK(resp.find("200 OK") != std::string::npos && resp.find("\"ok\":true") != std::string::npos);
    CHECK(host->snapshot().controls[static_cast<int>(Control::Drum)] == 40);
    platform::closeSocket(h);
    h = platform::connectTcp("127.0.0.1", basePort() + 1, &err);
    sendAll(h, "GET /../secret HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(recvAtLeast(h, 1000, 1000).find("400 Bad Request") != std::string::npos);
    platform::closeSocket(h);
    host->stop();
}

TEST(tc4_protocol) {
    auto host = startHost();
    Tc4Serial tc4(*host);
    CHECK(tc4.handleLine("CHAN;1200\n") == "# Active channels set to 1200");
    CHECK(tc4.handleLine("UNITS;C") == "# Units changed to C");
    CHECK(tc4.handleLine("FILT;70,70,70,70")[0] == '#');
    std::string r = tc4.handleLine("READ");
    int commas = 0;
    for (char ch : r) commas += ch == ',';
    CHECK(commas == 4);  // ambient + 4 channels
    CHECK(tc4.handleLine("OT1;65").empty());
    CHECK(tc4.handleLine("OT2;55").empty());
    CHECK(host->snapshot().controls[static_cast<int>(Control::Heat)] == 65);
    CHECK(host->snapshot().controls[static_cast<int>(Control::Fan)] == 55);
    tc4.handleLine("PID;SV;200");
    tc4.handleLine("PID;ON");
    std::string withPid = tc4.handleLine("READ");
    commas = 0;
    for (char ch : withPid) commas += ch == ',';
    CHECK(commas == 7);  // + heater, fan, SV
    tc4.handleLine("PID;OFF");
    host->setUnits('F');
    std::string f = tc4.handleLine("READ");
    CHECK_RANGE(std::atof(f.c_str()), 68, 75);  // 22 C ambient in F
    host->setUnits('C');
    host->stop();
}

TEST(sound_follows_the_machine) {
    HostConfig cfg;
    cfg.data_dir = rstest::dataDir();
    cfg.http_enabled = cfg.modbus_enabled = false;
    cfg.roaster = "popper_modded";
    cfg.seed = 5;
    Host host(cfg);
    std::string err;
    CHECK(host.start(&err));
    json::Value p = json::Value::object();
    p["cmd"] = "pause";
    p["value"] = true;
    host.command(p, "test");
    json::Value pw = json::Value::object();
    pw["cmd"] = "power";
    pw["value"] = true;
    host.command(pw, "test");  // blower and heater only run with the switch on
    AudioSynth synth(host, 22050);
    std::vector<float> buf(22050);
    auto rms = [&](double simSeconds) {
        json::Value s = json::Value::object();
        s["cmd"] = "step";
        s["seconds"] = simSeconds;
        host.command(s, "test");
        synth.render(buf.data(), static_cast<int>(buf.size()));
        double sum = 0, peak = 0;
        for (float v : buf) {
            sum += v * v;
            peak = std::max(peak, static_cast<double>(std::fabs(v)));
        }
        CHECK(peak <= 1.0);
        return std::sqrt(sum / buf.size());
    };
    host.setControl(Control::Heat, 0, "test");
    host.setControl(Control::Fan, 0, "test");
    rms(3);
    double off = rms(1);
    host.setControl(Control::Fan, 90, "test");
    rms(3);
    double fan = rms(1);
    CHECK(off < 0.005);   // machine off: silence
    CHECK(fan > 0.03);    // blower running: clearly audible
    AudioMix m;
    m.master = 0;
    synth.setMix(m);
    CHECK(rms(1) < 1e-6);
    host.stop();
}

TEST(c_api) {
    char err[256] = {0};
    std::string cfg = "{\"data_dir\":\"" + rstest::dataDir() + "\",\"http_enabled\":false,\"modbus_enabled\":false,"
                      "\"roaster\":\"stir_pan\",\"seed\":3}";
    rs_host* h = rs_host_create(cfg.c_str(), err, sizeof err);
    CHECK(h != nullptr);
    if (!h) return;
    CHECK(rs_host_start(h, err, sizeof err) == 1);
    rs_state st{};
    st.struct_size = sizeof st;
    CHECK(rs_host_get_state(h, &st) == 1);
    CHECK((st.control_mask & (1u << RS_CTL_STIR)) != 0);
    CHECK((st.control_mask & (1u << RS_CTL_DRUM)) == 0);
    CHECK(rs_host_set_control(h, RS_CTL_HEAT, 90) == 1);
    CHECK(rs_host_set_control(h, RS_CTL_DRUM, 50) == 0);  // stir pan has no drum
    rs_state st0{};
    st0.struct_size = sizeof st0;
    CHECK(rs_host_get_state(h, &st0) == 1);
    CHECK(st0.powered == 0);  // machines boot switched off
    char out[4096];
    size_t n = rs_host_command(h, R"({"cmd":"power","value":true})", "test", out, sizeof out);
    CHECK(n > 0 && std::strstr(out, "\"ok\":true"));
    n = rs_host_command(h, R"({"cmd":"charge","batch_g":200})", "test", out, sizeof out);
    CHECK(n > 0 && std::strstr(out, "\"ok\":true"));
    rs_host_command(h, R"({"cmd":"step","seconds":120})", "test", out, sizeof out);
    rs_visual_info vi{};
    vi.struct_size = sizeof vi;
    rs_visual_bean beans[300];
    int nb = rs_host_get_visual(h, &vi, beans, 300);
    CHECK(nb == 200 && vi.bean_count == 200);
    CHECK(vi.kind == RS_KIND_STIR_PAN && vi.bean_location == 1);
    CHECK(beans[0].size_mm > 5 && beans[0].temp_c > 50);
    rs_sample samples[400];
    uint64_t next = 0;
    int got = rs_host_get_history(h, 0, samples, 400, &next);
    CHECK(got > 100 && next > 0);
    // Truncation reports the full length
    char tiny[8];
    size_t full = rs_host_command(h, R"({"cmd":"list"})", "test", tiny, sizeof tiny);
    CHECK(full > sizeof tiny && std::strlen(tiny) == sizeof tiny - 1);
    rs_host_stop(h);
    rs_host_destroy(h);
}
