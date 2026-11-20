// roastsim_sound: render the roaster's sound offline to a WAV file.
//
//   roastsim_sound --roaster popper_modded --bean kenya_washed --from fc --seconds 30 --out kenya_fc.wav
//
// --from  idle    machine running, no beans
//         charge  from the moment beans go in
//         fc      10 s before first crack      sc   10 s before second crack
//         <n>     n seconds after charge
// --heat P (default 100 fluid bed / stir pan, 85 drum), --batch G, --rate HZ, --stats (per-second levels)
// The simulation is stepped in lock-step with the audio, so the file is exactly
// what a listener would hear in real time, independent of CPU speed.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "roastsim/audio.hpp"
#include "roastsim/host.hpp"
#include "roastsim/platform.hpp"

using namespace roastsim;

namespace {

struct Opts {
    std::string roaster = "popper_modded", bean = "colombia_washed", from = "fc", out = "roast.wav", data;
    double seconds = 30, heat = -1, batch = -1;
    int rate = 44100;
    bool stats = false;
    float machine = 1.0f, cracks = 1.0f;
};

json::Value cmd(const char* name) {
    json::Value v = json::Value::object();
    v["cmd"] = name;
    return v;
}

std::unique_ptr<Host> makeHost(const Opts& o) {
    HostConfig cfg;
    cfg.data_dir = o.data;
    cfg.http_enabled = cfg.modbus_enabled = false;
    cfg.roaster = o.roaster;
    cfg.bean = o.bean;
    cfg.seed = 11;
    auto h = std::make_unique<Host>(cfg);
    std::string err;
    if (!h->start(&err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return nullptr;
    }
    json::Value p = cmd("pause");
    p["value"] = true;
    h->command(p, "sound");
    json::Value l = cmd("load");
    l["roaster"] = o.roaster;
    l["bean"] = o.bean;
    json::Value r = h->command(l, "sound");
    if (!r.getBool("ok", false)) {
        std::fprintf(stderr, "%s\n", r.get("error", "load failed").c_str());
        return nullptr;
    }
    json::Value pw = cmd("power");
    pw["value"] = true;
    h->command(pw, "sound");
    RoasterDef def = h->roasterDef();
    bool drum = def.kind == RoasterKind::Drum;
    json::Value set = cmd("set");
    json::Value c = json::Value::object();
    double heat = o.heat >= 0 ? o.heat : (drum ? 85.0 : 100.0);
    c[def.hasControl(Control::Heat) ? "heat" : "voltage"] = heat;
    set["controls"] = c;
    h->command(set, "sound");
    if (drum) {
        json::Value ph = cmd("preheat");
        ph["target_C"] = 200;
        h->command(ph, "sound");
    }
    return h;
}

void step(Host& h, double s) {
    json::Value v = cmd("step");
    v["seconds"] = s;
    h.command(v, "sound");
}

void charge(Host& h, const Opts& o) {
    json::Value v = cmd("charge");
    if (o.batch > 0) v["batch_g"] = o.batch;
    json::Value r = h.command(v, "sound");
    if (!r.getBool("ok", false)) std::fprintf(stderr, "charge: %s\n", r.get("error", "").c_str());
}

double findEvent(const Opts& o, const char* type) {
    auto h = makeHost(o);
    if (!h) return -1;
    charge(*h, o);
    for (int i = 0; i < 2400 * 2; ++i) {
        step(*h, 0.5);
        for (const auto& e : h->eventsSince(0))
            if (e.type == type) return e.t_roast;
    }
    return -1;
}

void writeWav(const std::string& path, const std::vector<float>& s, int rate) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    std::uint32_t bytes = static_cast<std::uint32_t>(s.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(1);
    u32(static_cast<std::uint32_t>(rate)); u32(static_cast<std::uint32_t>(rate * 2)); u16(2); u16(16);
    f.write("data", 4); u32(bytes);
    for (float v : s) {
        v = v > 1 ? 1 : (v < -1 ? -1 : v);
        u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(v * 32767.0f)));
    }
}

}  // namespace

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--roaster") o.roaster = next();
        else if (a == "--bean") o.bean = next();
        else if (a == "--from") o.from = next();
        else if (a == "--seconds") o.seconds = std::atof(next().c_str());
        else if (a == "--out") o.out = next();
        else if (a == "--heat") o.heat = std::atof(next().c_str());
        else if (a == "--batch") o.batch = std::atof(next().c_str());
        else if (a == "--rate") o.rate = std::atoi(next().c_str());
        else if (a == "--data") o.data = next();
        else if (a == "--stats") o.stats = true;
        else if (a == "--machine") o.machine = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--cracks") o.cracks = static_cast<float>(std::atof(next().c_str()));
        else {
            std::printf("usage: roastsim_sound [--roaster ID] [--bean ID] [--from idle|charge|fc|sc|<s>] [--seconds N]\n"
                        "                      [--heat P] [--batch G] [--rate HZ] [--out file.wav] [--stats]\n");
            return a == "-h" || a == "--help" ? 0 : 2;
        }
    }
    if (o.data.empty()) o.data = platform::findResourceDir("data");

    double startAt = 0;
    bool idle = o.from == "idle";
    if (o.from == "fc" || o.from == "sc") {
        double t = findEvent(o, o.from == "fc" ? "FCs" : "SCs");
        if (t < 0) {
            std::fprintf(stderr, "no %s within 40 minutes at this heat\n", o.from.c_str());
            return 1;
        }
        startAt = std::max(0.0, t - 10.0);
        std::printf("%s at %.0f s; rendering from %.0f s\n", o.from == "fc" ? "First crack" : "Second crack", t, startAt);
    } else if (o.from != "charge" && !idle) {
        startAt = std::atof(o.from.c_str());
    }

    auto h = makeHost(o);
    if (!h) return 1;
    if (!idle) {
        charge(*h, o);
        for (double t = 0; t < startAt; t += 0.5) step(*h, 0.5);
    } else {
        step(*h, 5);
    }

    AudioSynth synth(*h, o.rate);
    {
        AudioMix m;
        m.machine = o.machine;
        m.cracks = o.cracks;
        synth.setMix(m);
    }
    const int block = 512;
    std::vector<float> all, buf(block);
    std::uint64_t crackStart = h->crackSeq();
    double secAcc = 0, sumSq = 0, peak = 0;
    int second = 0;
    if (o.stats) std::printf("  sec   BT     RMS    peak   cracks\n");
    std::uint64_t lastCracks = crackStart;
    while (all.size() < static_cast<std::size_t>(o.seconds * o.rate)) {
        step(*h, static_cast<double>(block) / o.rate);
        synth.render(buf.data(), block);
        for (float v : buf) {
            all.push_back(v);
            sumSq += v * v;
            peak = std::max(peak, static_cast<double>(std::fabs(v)));
        }
        secAcc += static_cast<double>(block) / o.rate;
        if (secAcc >= 1.0) {
            if (o.stats) {
                std::uint64_t now = h->crackSeq();
                std::printf("%5d %6.1f %6.3f %6.3f %6llu\n", second, h->snapshot().bt, std::sqrt(sumSq / (secAcc * o.rate)),
                            peak, static_cast<unsigned long long>(now - lastCracks));
                lastCracks = now;
            }
            secAcc -= 1.0;
            sumSq = peak = 0;
            ++second;
        }
    }
    writeWav(o.out, all, o.rate);
    std::printf("Wrote %s (%.1f s, %llu cracks)\n", o.out.c_str(), o.seconds,
                static_cast<unsigned long long>(h->crackSeq() - crackStart));
    h->stop();
    return 0;
}
