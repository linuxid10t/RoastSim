// roastsim_script: run scripted roasts headlessly (no networking, core only).
//
//   roastsim_script <script.json> [--data DIR] [--csv out.csv] [--every N]
//   roastsim_script --inline '<json>' ...
//
// Prints a progress table every N simulated seconds and the roast report.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "roastsim/catalog.hpp"
#include "roastsim/scenario.hpp"

using namespace roastsim;

static std::string mmss(double s) {
    if (s < 0) return "  -  ";
    char b[16];
    std::snprintf(b, sizeof b, "%2d:%02d", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
    return b;
}

int main(int argc, char** argv) {
    std::string scriptPath, inlineJson, dataDir = "data", csvPath;
    int every = 30;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) dataDir = argv[++i];
        else if (a == "--csv" && i + 1 < argc) csvPath = argv[++i];
        else if (a == "--every" && i + 1 < argc) every = std::atoi(argv[++i]);
        else if (a == "--inline" && i + 1 < argc) inlineJson = argv[++i];
        else if (a == "-h" || a == "--help") {
            std::printf("usage: roastsim_script <script.json> [--data DIR] [--csv out.csv] [--every N]\n");
            return 0;
        } else scriptPath = a;
    }
    std::string text = inlineJson;
    if (text.empty()) {
        if (scriptPath.empty() || !readTextFile(scriptPath, text)) {
            std::fprintf(stderr, "cannot read script '%s'\n", scriptPath.c_str());
            return 2;
        }
    }
    std::string err;
    json::Value script = json::Value::parse(text, &err);
    if (!err.empty()) {
        std::fprintf(stderr, "script JSON error: %s\n", err.c_str());
        return 2;
    }
    Catalog cat;
    if (!cat.loadDir(dataDir)) {
        std::fprintf(stderr, "cannot load data dir '%s'\n", dataDir.c_str());
        for (auto& e : cat.errors()) std::fprintf(stderr, "  %s\n", e.c_str());
        return 2;
    }
    for (auto& e : cat.errors()) std::fprintf(stderr, "warning: %s\n", e.c_str());

    // bean = true mean bean temperature (no probe lag); inlet = air entering the chamber;
    // wall = drum shell / pan / chamber wall; power = heat source output in W.
    std::printf("  time   BT     ET    RoR   bean   moist  agtron  heat  fan  inlet   wall  power  state\n");
    int counter = 0;
    ScenarioResult res;
    bool ok = runScenario(script, cat, res, [&](const Simulation& sim) {
        if (++counter % every) return;
        Snapshot s = sim.snapshot();
        std::printf("%s %6.1f %6.1f %5.1f %6.1f %5.2f%% %6.1f %5.0f %4.0f %6.1f %6.1f %6.0f  %s %s\n",
                    mmss(s.t_roast).c_str(), s.bt, s.et, s.ror_bt, s.beans.T_mean, s.beans.moisture_pct,
                    s.beans.agtron_ground,
                    s.actuators[static_cast<int>(sim.roaster().hasControl(Control::Heat) ? Control::Heat : Control::Voltage)],
                    s.actuators[static_cast<int>(Control::Fan)], s.tele.inlet_C, s.tele.wall_C, s.tele.heater_W,
                    roastPhaseName(s.phase), s.tele.bed_state);
    });
    if (!ok) {
        std::fprintf(stderr, "scenario failed: %s\n", res.error.c_str());
        return 1;
    }
    std::printf("\nEvents:\n");
    for (const auto& e : res.events)
        std::printf("  %s  %-6s %-16s BT %.1f  (%s)\n", mmss(e.t_roast).c_str(), e.type.c_str(), e.detail.c_str(), e.bt,
                    e.origin.c_str());
    std::printf("\nReport:\n%s\n", res.report.toJson().dump(2).c_str());

    if (!csvPath.empty()) {
        std::ofstream f(csvPath);
        f << "t,t_roast,bt,et,ror_bt,ror_et,bean_true,heat,fan,drum\n";
        for (const auto& h : res.history)
            f << h.t << ',' << h.t_roast << ',' << h.bt << ',' << h.et << ',' << h.ror_bt << ',' << h.ror_et << ','
              << h.bean_true << ',' << h.heat << ',' << h.fan << ',' << h.drum << '\n';
    }
    return 0;
}
