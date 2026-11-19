// TC4 / aArtisanQ serial emulation (what Artisan's "TC4" device type talks to).
//
// Artisan sends newline-terminated commands; the firmware replies with CRLF
// lines. Physical thermocouples are mapped as TC1 = ET, TC2 = BT, TC3/TC4 =
// the roaster's 3rd/4th probes, so the usual "CHAN;1200" gives ET on chan1 and
// BT on chan2, which is what Artisan expects.
//
//   CHAN;ijkl   -> "# Active channels set to ijkl"
//   UNITS;C|F   -> "# Units changed to C"
//   FILT;a,b,c,d-> "# Filter levels set"
//   READ        -> "ambient,chan1,chan2,chan3,chan4[,heater,fan,SV]"
//   OT1;duty  OT2;duty  IO3;duty  DCFAN;duty   (no reply)
//   PID;ON|OFF  PID;SV;v  PID;T;p;i;d  PID;CHAN;i  PID;CT;ms   (no reply)
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <vector>

#include "internal.hpp"

namespace roastsim {

namespace {

std::vector<std::string> splitArgs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : line) {
        if (ch == ',' || ch == ';' || ch == ' ' || ch == '=' || ch == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string fmt(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

}  // namespace

bool Tc4Serial::start(const std::string& port, int baud, std::string* err) {
    if (!port_.open(port, baud, err)) {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = "failed: " + (err ? *err : std::string("open failed"));
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = "TC4 emulation on " + port_.description() + " @" + std::to_string(baud);
    }
    stop_ = false;
    thread_ = std::thread([this] { loop(); });
    return true;
}

void Tc4Serial::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    port_.close();
}

std::string Tc4Serial::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

std::string Tc4Serial::handleLine(const std::string& rawLine) {
    std::string line = rawLine;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    auto args = splitArgs(line);
    if (args.empty()) return "";
    std::string cmd = upper(args[0]).substr(0, 5);  // first five characters are significant
    const std::string origin = "tc4";
    host_.noteLoggerActivity("tc4-serial");
    const bool stock = !host_.snapshot().has_control[static_cast<int>(Control::Heat)];

    if (cmd == "CHAN") {
        std::string m = args.size() > 1 ? args[1] : "1200";
        for (int i = 0; i < 4; ++i) chanMap_[i] = (i < static_cast<int>(m.size()) && std::isdigit(static_cast<unsigned char>(m[static_cast<std::size_t>(i)])))
                                                      ? m[static_cast<std::size_t>(i)] - '0'
                                                      : 0;
        return "# Active channels set to " + m;
    }
    if (cmd == "UNITS") {
        char u = args.size() > 1 && !args[1].empty() ? static_cast<char>(std::toupper(static_cast<unsigned char>(args[1][0]))) : 'C';
        host_.setUnits(u);
        return std::string("# Units changed to ") + (u == 'F' ? "F" : "C");
    }
    if (cmd == "FILT") return "# Filter levels set";
    if (cmd == "READ") {
        Snapshot s = host_.snapshot();
        // physical TCs: 1 = ET, 2 = BT, 3/4 = other probes in definition order
        double phys[5] = {0, -1, -1, -1, -1};
        int extra = 3;
        for (const auto& p : s.probes) {
            if (p.id == "ET") phys[1] = p.value;
            else if (p.id == "BT") phys[2] = p.value;
            else if (extra <= 4) phys[extra++] = p.value;
        }
        std::string out = fmt(host_.toLoggerUnits(s.env.ambient_C));
        for (int i = 0; i < 4; ++i) {
            int pc = chanMap_[i];
            double v = (pc >= 1 && pc <= 4) ? host_.toLoggerUnits(phys[pc]) : 0.0;
            out += "," + fmt(v);
        }
        if (pidOn_) {
            out += "," + fmt(s.controls[static_cast<int>(stock ? Control::Voltage : Control::Heat)]);
            out += "," + fmt(s.controls[static_cast<int>(Control::Fan)]);
            out += "," + fmt(pidSv_);
        }
        return out;
    }
    double val = args.size() > 1 ? std::atof(args[1].c_str()) : 0.0;
    if (cmd == "OT1") {
        if (!pidOn_) host_.setControl(stock ? Control::Voltage : Control::Heat, val, origin);
        return "";
    }
    if (cmd == "OT2" || cmd == "IO3" || cmd == "DCFAN") {
        // Fan on roasters that have one; drum speed on those that don't.
        if (!host_.setControl(Control::Fan, val, origin)) host_.setControl(Control::Drum, val, origin);
        return "";
    }
    if (cmd == "PID") {
        std::string sub = args.size() > 1 ? upper(args[1]) : "";
        if (sub == "ON") {
            pidOn_ = true;
            pidI_ = 0;
            lastPidT_ = -1;
        } else if (sub == "OFF") {
            pidOn_ = false;
            host_.setControl(stock ? Control::Voltage : Control::Heat, 0, origin);
        } else if (sub == "SV" && args.size() > 2) {
            pidSv_ = std::atof(args[2].c_str());
        } else if (sub == "T" && args.size() > 4) {
            kp_ = std::atof(args[2].c_str());
            ki_ = std::atof(args[3].c_str());
            kd_ = std::atof(args[4].c_str());
        } else if (sub == "CHAN" && args.size() > 2) {
            pidChan_ = std::atoi(args[2].c_str());
        }
        return "";
    }
    return "";  // DWRITE, AWRITE and unknown commands: no reply
}

void Tc4Serial::loop() {
    std::string buf;
    char tmp[512];
    auto lastPid = std::chrono::steady_clock::now();
    while (!stop_) {
        long n = port_.read(tmp, sizeof tmp, 100);
        if (n > 0) {
            buf.append(tmp, static_cast<std::size_t>(n));
            std::size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                std::string reply = handleLine(line);
                if (!reply.empty()) {
                    reply += "\r\n";
                    port_.write(reply.data(), reply.size());
                }
            }
            if (buf.size() > 4096) buf.clear();
        } else if (n < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        // Firmware PID on OT1, using the logical channel's temperature.
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - lastPid).count();
        if (pidOn_ && dt >= 1.0) {
            lastPid = now;
            Snapshot s = host_.snapshot();
            double phys[5] = {0, s.et, s.bt, 0, 0};
            int pc = (pidChan_ >= 1 && pidChan_ <= 4) ? chanMap_[pidChan_ - 1] : 2;
            double pv = host_.toLoggerUnits((pc >= 1 && pc <= 4) ? phys[pc] : s.bt);
            double err = pidSv_ - pv;
            pidI_ = clampd(pidI_ + ki_ * err * dt, 0.0, 100.0);
            double d = lastPidT_ < 0 ? 0 : (pv - lastPidT_) / dt;
            lastPidT_ = pv;
            double out = clampd(kp_ * err + pidI_ - kd_ * d, 0.0, 100.0);
            bool stock = !s.has_control[static_cast<int>(Control::Heat)];
            host_.setControl(stock ? Control::Voltage : Control::Heat, out, "tc4-pid");
        }
    }
}

}  // namespace roastsim
