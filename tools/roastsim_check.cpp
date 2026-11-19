// roastsim_check: validate roaster/bean presets and run standard reference roasts.
//
//   roastsim_check [--data DIR]                      validate every preset file
//   roastsim_check --roaster ID [--bean ID] [--batch G] [--heat PCT] [--preheat C]
//                                                    reference roast on one roaster
//   roastsim_check --bean ID                         bean on the reference roasters, vs colombia_washed
//   roastsim_check --all                             reference roast for every roaster
//   add --json for machine-readable output
//
// The reference roast is deliberately simple and machine-agnostic: controls at
// their defaults, heat fixed (100 % fluid bed / stir pan, 85 % drums), drums
// preheated, default batch, drop when development reaches 20 % DTR. It is a
// sanity check that a preset behaves like a coffee roaster, not a profile.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "roastsim/catalog.hpp"
#include "roastsim/scenario.hpp"

using namespace roastsim;
namespace fs = std::filesystem;

namespace {

struct Options {
    std::string data = "data";
    std::string roaster, bean;
    double batch = -1, heat = -1, preheat = -1;
    bool all = false, json = false;
};

std::string mmss(double s) {
    if (s < 0) return "--:--";
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
    return b;
}

// ---------------------------------------------------------------- validation
int validateFiles(const std::string& dir, json::Value& out) {
    int problems = 0;
    json::Value issues = json::Value::array();
    auto issue = [&](const std::string& file, const std::string& level, const std::string& msg) {
        if (level == "error") ++problems;
        json::Value v = json::Value::object();
        v["file"] = file;
        v["level"] = level;
        v["message"] = msg;
        issues.push(v);
    };
    for (const char* sub : {"roasters", "beans"}) {
        std::map<std::string, std::string> seen;
        std::error_code ec;
        fs::path p = fs::u8path(dir) / sub;
        if (!fs::is_directory(p, ec)) {
            issue(p.u8string(), "error", "directory missing");
            continue;
        }
        for (const auto& e : fs::directory_iterator(p, ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".json") continue;
            std::string name = std::string(sub) + "/" + e.path().filename().u8string();
            std::string text, err;
            readTextFile(e.path().u8string(), text);
            json::Value v = json::Value::parse(text, &err);
            if (!err.empty()) {
                issue(name, "error", "JSON syntax: " + err);
                continue;
            }
            std::string id = v.get("id", "");
            if (id.empty()) {
                issue(name, "error", "missing \"id\"");
                continue;
            }
            if (seen.count(id)) issue(name, "error", "duplicate id '" + id + "' (also in " + seen[id] + ")");
            seen[id] = name;
            if (e.path().stem().u8string() != id) issue(name, "warning", "file name should be " + id + ".json");
            Catalog c;
            if (std::string(sub) == "roasters") {
                if (!c.addRoasterJson(text, &err)) issue(name, "error", err);
                // Unknown keys are silently ignored by the loader: flag likely typos.
                static const char* top[] = {"id", "name", "description", "kind", "batch", "controls", "probes",
                                            "fluid_bed", "drum", "stir_pan", "cooling", "charge_bean_temp_C", "visual"};
                for (const auto& k : v.keys()) {
                    bool known = false;
                    for (const char* t : top) known |= k == t;
                    if (!known) issue(name, "warning", "unknown top-level key '" + k + "' (ignored)");
                }
                std::string kind = v.get("kind", "");
                const char* block = kind == "fluid_bed" ? "fluid_bed" : kind == "drum" ? "drum" : kind == "stir_pan" ? "stir_pan" : "";
                if (*block && !v.has(block)) issue(name, "warning", std::string("no \"") + block + "\" block: every machine parameter is a default");
                for (const char* other : {"fluid_bed", "drum", "stir_pan"})
                    if (v.has(other) && std::string(other) != block)
                        issue(name, "warning", std::string("\"") + other + "\" block is ignored for kind '" + kind + "'");
            } else {
                if (!c.addBeanJson(text, &err)) issue(name, "error", err);
                static const char* keys[] = {"id", "name", "origin", "process", "notes", "moisture_pct", "moisture_wb",
                                             "moisture_sd", "density", "bean_mass_mg", "size_rel_sd", "fc_temp", "fc_sd",
                                             "sc_temp", "sc_sd", "chaff_frac", "kinetics_scale", "bulk_density", "crack_loudness",
                                             "green_rgb", "kinetics"};
                for (const auto& k : v.keys()) {
                    bool known = false;
                    for (const char* t : keys) known |= k == t;
                    if (!known) issue(name, "warning", "unknown key '" + k + "' (ignored)");
                }
                double m = v.get("moisture_pct", 11.0);
                if (m < 7 || m > 14) issue(name, "warning", "moisture_pct outside the usual 8-13 % for green coffee");
                double d = v.get("density", 700.0);
                if (d < 550 || d > 820) issue(name, "warning", "density outside the usual 600-780 kg/m3");
                double fc = v.get("fc_temp", 196.0);
                if (fc < 185 || fc > 205) issue(name, "warning", "fc_temp outside the usual 190-202 C");
            }
        }
    }
    out["issues"] = issues;
    out["errors"] = problems;
    return problems;
}

// ---------------------------------------------------------------- reference roast
struct RefResult {
    bool ok = false;
    std::string error;
    RoastReport report;
    std::vector<std::string> alarms;
    std::vector<std::string> warnings;
    double fcBtProbe = 0;
};

json::Value baseScript(const RoasterDef& r, const std::string& bean, const Options& o) {
    json::Value s = json::Value::object();
    s["roaster"] = r.id;
    s["bean"] = bean;
    s["seed"] = 1;
    s["max_s"] = 2400;
    double batch = o.batch > 0 ? o.batch : r.batch.default_g;
    s["batch_g"] = batch;
    bool drum = r.kind == RoasterKind::Drum;
    double heat = o.heat >= 0 ? o.heat : (drum ? 85.0 : 100.0);
    double preheat = o.preheat >= 0 ? o.preheat : (drum ? 200.0 : 0.0);
    if (preheat > 0) s["preheat_C"] = preheat;
    json::Value set = json::Value::object();
    for (const auto& c : r.controls) set[controlName(c.id)] = c.def;
    if (r.hasControl(Control::Heat)) set["heat"] = heat;
    else if (r.hasControl(Control::Voltage)) set["voltage"] = std::min(heat, r.control(Control::Voltage)->max);
    json::Value acts = json::Value::array();
    json::Value a0 = json::Value::object();
    a0["at"] = 0;
    a0["set"] = set;
    acts.push(a0);
    json::Value ch = json::Value::object();
    ch["at"] = preheat > 0 ? 1 : 30;  // fluid beds/pans: 30 s warm-up
    ch["charge"] = true;
    acts.push(ch);
    s["actions"] = acts;
    return s;
}

RefResult referenceRoast(const Catalog& cat, const RoasterDef& r, const std::string& bean, const Options& o) {
    RefResult res;
    // Pass 1: find first crack.
    json::Value s = baseScript(r, bean, o);
    json::Value probe = json::Value::object();
    probe["event"] = "FCs";
    probe["delay"] = 1;
    probe["drop"] = true;
    s["actions"].push(probe);
    ScenarioResult sr;
    if (!runScenario(s, cat, sr)) {
        res.error = sr.error;
        return res;
    }
    double fcs = sr.report.fcs_s;
    // Pass 2: drop at 20 % DTR (dev = fcs / 4), or at 20 min if FC never came.
    json::Value s2 = baseScript(r, bean, o);
    json::Value drop = json::Value::object();
    if (fcs > 0) {
        drop["event"] = "FCs";
        drop["delay"] = fcs / 4.0;
    } else {
        drop["roast_at"] = 1200;
    }
    drop["drop"] = true;
    s2["actions"].push(drop);
    if (!runScenario(s2, cat, sr)) {
        res.error = sr.error;
        return res;
    }
    res.ok = true;
    res.report = sr.report;
    res.fcBtProbe = sr.report.fcs_bt;
    for (const auto& e : sr.events)
        if (e.type == "ALARM") {
            bool dup = false;
            for (const auto& a : res.alarms) dup |= a == e.detail;
            if (!dup) res.alarms.push_back(e.detail);
        }
    const RoastReport& rp = res.report;
    auto warn = [&](const std::string& w) { res.warnings.push_back(w); };
    if (rp.fcs_s < 0) warn("no first crack within 20 min: machine underpowered for this batch, or heat transfer too low");
    else if (rp.fcs_s < 180) warn("first crack before 3:00: too aggressive (check heater power / heat transfer)");
    else if (rp.fcs_s > 960) warn("first crack after 16:00: very slow (check losses, heater power, airflow)");
    if (rp.tp_s < 0) warn("no turning point detected (BT probe never dipped after charge?)");
    else if (rp.tp_s > 150) warn("turning point after 2:30 (BT probe time constant too long?)");
    if (rp.weight_loss_pct < 9 || rp.weight_loss_pct > 18) warn("weight loss outside 9-18 %");
    if (rp.moisture_pct > 4.5) warn("final moisture above 4.5 %: drying too slow");
    if (rp.agtron_sd > 10) warn("colour spread above 10 Agtron points: poor mixing / high unevenness");
    if (rp.scorch_pct > 5) warn("scorching: contact surfaces too hot or poor agitation");
    if (rp.tipping_pct > 5) warn("tipping: too much convective/radiant flux while beans are wet");
    if (rp.ejected_pct > 0.5) warn("beans ejected: fan too strong for this batch (fluidize_flow_per_kg / fan_max_flow)");
    for (const auto& a : res.alarms)
        if (a == "fuse_blown" || a == "overheat" || a == "bed_stalled" || a == "centrifuging" || a == "fire_risk")
            warn("alarm during reference roast: " + a);
    return res;
}

json::Value resultJson(const std::string& roaster, const std::string& bean, const RefResult& r) {
    json::Value v = json::Value::object();
    v["roaster"] = roaster;
    v["bean"] = bean;
    v["ok"] = r.ok;
    if (!r.ok) {
        v["error"] = r.error;
        return v;
    }
    v["report"] = r.report.toJson();
    json::Value a = json::Value::array();
    for (const auto& x : r.alarms) a.push(x);
    v["alarms"] = a;
    json::Value w = json::Value::array();
    for (const auto& x : r.warnings) w.push(x);
    v["warnings"] = w;
    v["verdict"] = r.warnings.empty() ? "plausible" : "check warnings";
    return v;
}

void printHeader() {
    std::printf("%-22s %-20s %6s %6s %6s %6s %7s %6s %6s %6s %5s  %s\n", "roaster", "bean", "TP", "DRY", "FCs", "DROP",
                "FC BT", "loss%", "moist", "agtron", "sd", "verdict");
}

void printRow(const std::string& roaster, const std::string& bean, const RefResult& r) {
    if (!r.ok) {
        std::printf("%-22s %-20s ERROR %s\n", roaster.c_str(), bean.c_str(), r.error.c_str());
        return;
    }
    const RoastReport& p = r.report;
    std::printf("%-22s %-20s %6s %6s %6s %6s %7.1f %6.1f %6.2f %6.1f %5.1f  %s\n", roaster.c_str(), bean.c_str(),
                mmss(p.tp_s).c_str(), mmss(p.dry_s).c_str(), mmss(p.fcs_s).c_str(), mmss(p.drop_s).c_str(), p.fcs_bt,
                p.weight_loss_pct, p.moisture_pct, p.agtron_ground, p.agtron_sd,
                r.warnings.empty() ? "plausible" : "CHECK");
    for (const auto& w : r.warnings) std::printf("    ! %s\n", w.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--data") o.data = next();
        else if (a == "--roaster") o.roaster = next();
        else if (a == "--bean") o.bean = next();
        else if (a == "--batch") o.batch = std::atof(next().c_str());
        else if (a == "--heat") o.heat = std::atof(next().c_str());
        else if (a == "--preheat") o.preheat = std::atof(next().c_str());
        else if (a == "--all") o.all = true;
        else if (a == "--json") o.json = true;
        else if (a == "-h" || a == "--help") {
            std::printf("usage: roastsim_check [--data DIR] [--roaster ID] [--bean ID] [--all] [--batch G] [--heat PCT] "
                        "[--preheat C] [--json]\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }

    json::Value out = json::Value::object();
    int errors = validateFiles(o.data, out);
    if (!o.json) {
        std::printf("Validation of %s: %d error(s)\n", o.data.c_str(), errors);
        for (const auto& is : out["issues"].items())
            std::printf("  %-7s %s: %s\n", is.get("level", "").c_str(), is.get("file", "").c_str(),
                        is.get("message", "").c_str());
    }
    Catalog cat;
    cat.loadDir(o.data);
    bool runAny = o.all || !o.roaster.empty() || !o.bean.empty();
    if (errors > 0 || !runAny) {
        if (o.json) std::printf("%s\n", out.dump(2).c_str());
        return errors > 0 ? 1 : 0;
    }

    json::Value runs = json::Value::array();
    int checks = 0;
    if (!o.json) {
        std::printf("\nReference roasts (defaults, fixed heat, drop at 20%% DTR):\n");
        printHeader();
    }
    auto runOne = [&](const RoasterDef& r, const std::string& bean) {
        RefResult res = referenceRoast(cat, r, bean, o);
        if (!res.ok || !res.warnings.empty()) ++checks;
        if (o.json) runs.push(resultJson(r.id, bean, res));
        else printRow(r.id, bean, res);
    };

    if (!o.roaster.empty()) {
        const RoasterDef* r = cat.roaster(o.roaster);
        if (!r) {
            std::fprintf(stderr, "unknown roaster '%s'\n", o.roaster.c_str());
            return 2;
        }
        std::string bean = o.bean.empty() ? "colombia_washed" : o.bean;
        if (!cat.bean(bean)) {
            std::fprintf(stderr, "unknown bean '%s'\n", bean.c_str());
            return 2;
        }
        runOne(*r, bean);
    } else if (!o.bean.empty()) {
        if (!cat.bean(o.bean)) {
            std::fprintf(stderr, "unknown bean '%s'\n", o.bean.c_str());
            return 2;
        }
        // The bean against the reference bean on three different machine families.
        for (const char* rid : {"popper_modded", "drum_home_1kg", "stir_pan"}) {
            const RoasterDef* r = cat.roaster(rid);
            if (!r) continue;
            runOne(*r, "colombia_washed");
            runOne(*r, o.bean);
        }
    }
    if (o.all) {
        for (const auto& r : cat.roasters()) runOne(r, o.bean.empty() ? "colombia_washed" : o.bean);
    }
    if (o.json) {
        out["runs"] = runs;
        std::printf("%s\n", out.dump(2).c_str());
    } else {
        std::printf("\n%s\n", checks == 0 ? "All reference roasts plausible." : "Some runs need a look (see '!' lines).");
    }
    return checks == 0 ? 0 : 3;
}
