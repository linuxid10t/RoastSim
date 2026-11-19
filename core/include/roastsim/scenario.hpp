// Scripted roasts: run a whole roast as fast as the CPU allows. Used by the
// test-suite, for calibration, and by `roastsimd --script`.
//
// {
//   "roaster": "popper_modded", "bean": "colombia_washed", "batch_g": 100,
//   "seed": 1, "preheat_C": 0, "max_s": 1800, "parcels": 200,
//   "env": { "ambient_C": 22, "line_voltage": 120 },
//   "actions": [
//     { "at": 0, "set": { "heat": 100, "fan": 70 } },   // seconds since script start
//     { "at": 30, "charge": true },
//     { "bt_ge": 170, "set": { "fan": 60 } },            // when the BT probe reads >= 170
//     { "event": "FCs", "delay": 75, "drop": true },     // 75 s after first crack
//     { "roast_at": 600, "set": { "heat": 50 } },        // seconds since CHARGE
//     { "event": "DROP", "delay": 0, "set": { "heat": 0 } }
//   ]
// }
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "roastsim/catalog.hpp"
#include "roastsim/simulation.hpp"

namespace roastsim {

struct ScenarioResult {
    bool ok = false;
    std::string error;
    std::vector<HistorySample> history;  // 1 Hz
    std::vector<SimEvent> events;
    RoastReport report;
    double sim_seconds = 0;
};

// Applies one environment JSON object onto `env` (only the keys present).
void applyEnvironmentJson(const json::Value& v, Environment& env);

bool runScenario(const json::Value& script, const Catalog& catalog, ScenarioResult& out,
                 const std::function<void(const Simulation&)>& onSecond = {});

}  // namespace roastsim
