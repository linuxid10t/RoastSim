// Modbus TCP slave. Register map (0-based addresses, any unit/slave id 1-247):
//
// Read (FC3 holding or FC4 input; identical contents), int16 unless noted:
//   0 BT x10            1 ET x10            2 probe #3 x10      3 probe #4 x10
//   4 ambient x10       5 BT RoR x10/min    6 ET RoR x10/min
//   7 heat %            8 fan %             9 drum %           10 stir %
//  11 voltage %        12 cooling (0/100)  13 phase (0 idle 1 roasting 2 cooling 3 done)
//  14 roast time s     15 batch g          16 event flags      17 alarm bits
//  18 true bean temp x10 (no probe lag - a "cheat" channel)    19 Agtron (ground) x10
//  20 heater W (uint16) 21 smoke %          22 moisture % x100  23 weight loss % x100
//  Temperatures in the logger units (C default, F if configured). Open probe = -10 (-1.0).
//
// Write (FC6 single / FC16 multiple holding registers), also readable:
// 100 heat %  101 fan %  102 drum %  103 stir %  104 voltage %  105 cooling (0/100)
// 110 command: 1 CHARGE, 2 DROP, 3 COOL END, 4 empty, 5 cold reset, 6 repair,
//              10 DRY, 11 FCs, 12 FCe, 13 SCs, 14 SCe (manual marks)
// 111 batch size g     112 preheat target (logger units) - writing starts a preheat
//
// Coils (FC1 read / FC5, FC15 write): 0 charge, 1 drop, 2 cooling tray on/off
// Discrete inputs (FC2): 0 roasting, 1 FC started, 2 SC started, 3 beans in tray
#include <cmath>
#include <cstdint>

#include "internal.hpp"

namespace roastsim {

namespace {

constexpr int kMaxAddr = 200;

std::uint16_t u16(const std::string& s, std::size_t i) {
    return static_cast<std::uint16_t>((static_cast<unsigned char>(s[i]) << 8) | static_cast<unsigned char>(s[i + 1]));
}

void put16(std::string& s, std::uint16_t v) {
    s.push_back(static_cast<char>(v >> 8));
    s.push_back(static_cast<char>(v & 0xFF));
}

std::int16_t s16(double v) {
    double r = std::round(v);
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    return static_cast<std::int16_t>(r);
}

struct RegisterImage {
    std::uint16_t reg[kMaxAddr] = {};
    bool coil[8] = {};
    bool discrete[8] = {};
};

RegisterImage buildImage(Host& host) {
    RegisterImage img;
    Snapshot s = host.snapshot();
    auto temp = [&](double c) { return static_cast<std::uint16_t>(s16(host.toLoggerUnits(c) * 10.0)); };
    auto rate = [&](double r) { return static_cast<std::uint16_t>(s16((host.units() == 'F' ? r * 1.8 : r) * 10.0)); };
    // probes: BT, ET, then the remaining ones in definition order
    std::vector<double> extra;
    for (const auto& p : s.probes) {
        if (p.id == "BT") img.reg[0] = temp(p.value);
        else if (p.id == "ET") img.reg[1] = temp(p.value);
        else extra.push_back(p.value);
    }
    if (extra.size() > 0) img.reg[2] = temp(extra[0]);
    if (extra.size() > 1) img.reg[3] = temp(extra[1]);
    img.reg[4] = temp(s.env.ambient_C);
    img.reg[5] = rate(s.ror_bt);
    img.reg[6] = rate(s.ror_et);
    for (int i = 0; i < 6; ++i) {
        std::uint16_t v = static_cast<std::uint16_t>(s16(s.controls[i]));
        img.reg[7 + i] = v;
        img.reg[100 + i] = v;
    }
    img.reg[13] = static_cast<std::uint16_t>(s.phase);
    img.reg[14] = static_cast<std::uint16_t>(s.t_roast > 0 ? s16(s.t_roast) : 0);
    img.reg[15] = static_cast<std::uint16_t>(s16(s.batch_g));
    std::uint16_t flags = 0;
    for (const auto& e : host.eventsSince(0)) {
        if (s.t_roast < 0 || e.t_roast < 0) continue;
        if (e.type == "CHARGE") flags |= 1;
        else if (e.type == "DRY") flags |= 2;
        else if (e.type == "FCs") flags |= 4;
        else if (e.type == "FCe") flags |= 8;
        else if (e.type == "SCs") flags |= 16;
        else if (e.type == "SCe") flags |= 32;
        else if (e.type == "DROP") flags |= 64;
        else if (e.type == "COOL") flags |= 128;
    }
    img.reg[16] = flags;
    img.reg[17] = static_cast<std::uint16_t>(s.alarms & 0xFFFF);
    img.reg[18] = s.beans.present ? temp(s.beans.T_mean) : 0;
    img.reg[19] = static_cast<std::uint16_t>(s.beans.present ? s16(s.beans.agtron_ground * 10.0) : 0);
    img.reg[20] = static_cast<std::uint16_t>(std::min(65535.0, std::max(0.0, std::round(s.tele.heater_W))));
    img.reg[21] = static_cast<std::uint16_t>(s16(s.smoke * 100.0));
    img.reg[22] = static_cast<std::uint16_t>(s16(s.beans.moisture_pct * 100.0));
    img.reg[23] = static_cast<std::uint16_t>(s16(s.beans.weight_loss_pct * 100.0));
    json::Value st = host.stateJson(false);
    img.reg[111] = static_cast<std::uint16_t>(s16(st.get("next_batch_g", 0.0)));
    img.coil[0] = s.phase == RoastPhase::Roasting;
    img.coil[1] = s.phase == RoastPhase::Cooling || s.phase == RoastPhase::Done;
    img.coil[2] = s.controls[static_cast<int>(Control::Cooling)] >= 50;
    img.discrete[0] = s.phase == RoastPhase::Roasting;
    img.discrete[1] = (flags & 4) != 0;
    img.discrete[2] = (flags & 16) != 0;
    img.discrete[3] = s.bean_location == BeanLocation::Tray;
    return img;
}

// Returns false (and an exception code) for illegal writes.
bool writeRegister(Host& host, int addr, std::uint16_t raw, std::uint8_t& exc) {
    const std::string origin = "modbus";
    auto signedv = static_cast<std::int16_t>(raw);
    if (addr >= 100 && addr <= 105) {
        Control c = static_cast<Control>(addr - 100);
        host.setControl(c, static_cast<double>(signedv), origin);  // silently ignored if absent
        return true;
    }
    if (addr == 110) {
        json::Value cmd = json::Value::object();
        switch (raw) {
            case 0: return true;
            case 1: cmd["cmd"] = "charge"; break;
            case 2: cmd["cmd"] = "drop"; break;
            case 3: cmd["cmd"] = "cool_end"; break;
            case 4: cmd["cmd"] = "empty"; break;
            case 5: cmd["cmd"] = "reset"; break;
            case 6: cmd["cmd"] = "repair"; break;
            case 10: cmd["cmd"] = "mark"; cmd["event"] = "DRY"; break;
            case 11: cmd["cmd"] = "mark"; cmd["event"] = "FCs"; break;
            case 12: cmd["cmd"] = "mark"; cmd["event"] = "FCe"; break;
            case 13: cmd["cmd"] = "mark"; cmd["event"] = "SCs"; break;
            case 14: cmd["cmd"] = "mark"; cmd["event"] = "SCe"; break;
            default: exc = 3; return false;
        }
        host.command(cmd, origin);
        return true;
    }
    if (addr == 111) {
        json::Value cmd = json::Value::object();
        cmd["cmd"] = "batch";
        cmd["batch_g"] = static_cast<double>(raw);
        host.command(cmd, origin);
        return true;
    }
    if (addr == 112) {
        json::Value cmd = json::Value::object();
        cmd["cmd"] = "preheat";
        cmd["target"] = static_cast<double>(signedv);
        host.command(cmd, origin);
        return true;
    }
    exc = 2;  // illegal data address
    return false;
}

void exception(std::string& pdu, std::uint8_t fc, std::uint8_t code) {
    pdu.clear();
    pdu.push_back(static_cast<char>(fc | 0x80));
    pdu.push_back(static_cast<char>(code));
}

std::string handlePdu(Host& host, const std::string& req) {
    std::string out;
    if (req.empty()) return out;
    std::uint8_t fc = static_cast<std::uint8_t>(req[0]);
    switch (fc) {
        case 1:
        case 2: {  // read coils / discrete inputs
            if (req.size() < 5) { exception(out, fc, 3); break; }
            int addr = u16(req, 1), qty = u16(req, 3);
            if (qty < 1 || qty > 2000) { exception(out, fc, 3); break; }
            if (addr + qty > 8) { exception(out, fc, 2); break; }
            RegisterImage img = buildImage(host);
            int nbytes = (qty + 7) / 8;
            out.push_back(static_cast<char>(fc));
            out.push_back(static_cast<char>(nbytes));
            for (int b = 0; b < nbytes; ++b) {
                std::uint8_t byte = 0;
                for (int i = 0; i < 8; ++i) {
                    int idx = b * 8 + i;
                    if (idx >= qty) break;
                    bool v = fc == 1 ? img.coil[addr + idx] : img.discrete[addr + idx];
                    if (v) byte |= static_cast<std::uint8_t>(1u << i);
                }
                out.push_back(static_cast<char>(byte));
            }
            break;
        }
        case 3:
        case 4: {  // read holding / input registers
            if (req.size() < 5) { exception(out, fc, 3); break; }
            int addr = u16(req, 1), qty = u16(req, 3);
            if (qty < 1 || qty > 125) { exception(out, fc, 3); break; }
            if (addr + qty > kMaxAddr) { exception(out, fc, 2); break; }
            RegisterImage img = buildImage(host);
            out.push_back(static_cast<char>(fc));
            out.push_back(static_cast<char>(qty * 2));
            for (int i = 0; i < qty; ++i) put16(out, img.reg[addr + i]);
            break;
        }
        case 5: {  // write single coil
            if (req.size() < 5) { exception(out, fc, 3); break; }
            int addr = u16(req, 1);
            std::uint16_t v = u16(req, 3);
            if (v != 0xFF00 && v != 0x0000) { exception(out, fc, 3); break; }
            bool on = v == 0xFF00;
            json::Value cmd = json::Value::object();
            if (addr == 0 && on) cmd["cmd"] = "charge";
            else if (addr == 1 && on) cmd["cmd"] = "drop";
            else if (addr == 2) host.setControl(Control::Cooling, on ? 100 : 0, "modbus");
            else if (addr > 2) { exception(out, fc, 2); break; }
            if (cmd.has("cmd")) host.command(cmd, "modbus");
            out = req.substr(0, 5);
            break;
        }
        case 15: {  // write multiple coils
            if (req.size() < 6) { exception(out, fc, 3); break; }
            int addr = u16(req, 1), qty = u16(req, 3);
            if (addr + qty > 8) { exception(out, fc, 2); break; }
            for (int i = 0; i < qty; ++i) {
                std::size_t byteIdx = 6 + static_cast<std::size_t>(i / 8);
                if (byteIdx >= req.size()) break;
                bool on = (static_cast<unsigned char>(req[byteIdx]) >> (i % 8)) & 1;
                int a = addr + i;
                json::Value cmd = json::Value::object();
                if (a == 0 && on) cmd["cmd"] = "charge";
                else if (a == 1 && on) cmd["cmd"] = "drop";
                else if (a == 2) host.setControl(Control::Cooling, on ? 100 : 0, "modbus");
                if (cmd.has("cmd")) host.command(cmd, "modbus");
            }
            out = req.substr(0, 5);
            break;
        }
        case 6: {  // write single register
            if (req.size() < 5) { exception(out, fc, 3); break; }
            std::uint8_t exc = 0;
            if (!writeRegister(host, u16(req, 1), u16(req, 3), exc)) { exception(out, fc, exc); break; }
            out = req.substr(0, 5);
            break;
        }
        case 16: {  // write multiple registers
            if (req.size() < 6) { exception(out, fc, 3); break; }
            int addr = u16(req, 1), qty = u16(req, 3);
            if (qty < 1 || qty > 123 || req.size() < 6 + static_cast<std::size_t>(qty) * 2) { exception(out, fc, 3); break; }
            std::uint8_t exc = 0;
            bool okAll = true;
            for (int i = 0; i < qty; ++i) {
                if (!writeRegister(host, addr + i, u16(req, 6 + static_cast<std::size_t>(i) * 2), exc)) {
                    okAll = false;
                    break;
                }
            }
            if (!okAll) { exception(out, fc, exc); break; }
            out = req.substr(0, 5);
            break;
        }
        case 22: {  // mask write register
            if (req.size() < 7) { exception(out, fc, 3); break; }
            int addr = u16(req, 1);
            std::uint16_t andM = u16(req, 3), orM = u16(req, 5);
            if (addr >= kMaxAddr) { exception(out, fc, 2); break; }
            RegisterImage img = buildImage(host);
            std::uint16_t cur = img.reg[addr];
            std::uint16_t nv = static_cast<std::uint16_t>((cur & andM) | (orM & ~andM));
            std::uint8_t exc = 0;
            if (!writeRegister(host, addr, nv, exc)) { exception(out, fc, exc); break; }
            out = req.substr(0, 7);
            break;
        }
        default: exception(out, fc, 1);  // illegal function
    }
    return out;
}

}  // namespace

bool modbusProcess(Host& host, std::string& in, std::string& out) {
    while (in.size() >= 7) {
        std::uint16_t tid = u16(in, 0);
        std::uint16_t pid = u16(in, 2);
        std::uint16_t len = u16(in, 4);
        if (pid != 0 || len < 2 || len > 260) return false;
        if (in.size() < 6u + len) break;
        std::uint8_t unit = static_cast<std::uint8_t>(in[6]);
        std::string pdu = in.substr(7, len - 1u);
        in.erase(0, 6u + len);
        host.noteLoggerActivity("modbus");
        std::string resp = handlePdu(host, pdu);
        if (resp.empty()) continue;
        put16(out, tid);
        put16(out, 0);
        put16(out, static_cast<std::uint16_t>(resp.size() + 1));
        out.push_back(static_cast<char>(unit));
        out += resp;
    }
    return true;
}

}  // namespace roastsim
