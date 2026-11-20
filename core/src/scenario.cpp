#include "roastsim/scenario.hpp"

#include <cmath>

namespace roastsim {

void applyEnvironmentJson(const json::Value& v, Environment& env) {
    if (!v.isObject()) return;
    env.ambient_C = v.get("ambient_C", env.ambient_C);
    env.humidity_rh = v.get("humidity_rh", env.humidity_rh);
    env.line_voltage = v.get("line_voltage", env.line_voltage);
    env.source_ohms = v.get("source_ohms", env.source_ohms);
    env.altitude_m = v.get("altitude_m", env.altitude_m);
    env.gas_pressure = v.get("gas_pressure", env.gas_pressure);
}

namespace {

struct Action {
    json::Value spec;
    bool fired = false;
    double eventSeen = -1;  // session time the awaited event happened
};

void apply(Simulation& sim, const json::Value& a, std::string& err) {
    const json::Value& set = a["set"];
    for (const auto& key : set.keys()) {
        Control c;
        if (controlFromName(key, c)) sim.setControl(c, set[key].num());
        else err = "unknown control '" + key + "'";
    }
    if (a.getBool("charge", false)) {
        std::string e;
        if (!sim.charge(a.get("batch_g", sim.roaster().batch.default_g), "script", &e)) err = e;
    }
    if (a.getBool("drop", false)) sim.drop("script");
    if (a.has("power")) sim.setPower(a.getBool("power", true), "script");
    if (a.has("mark")) sim.markEvent(a.get("mark", ""), "script");
    if (a.has("env")) {
        Environment env = sim.environment();
        applyEnvironmentJson(a["env"], env);
        sim.setEnvironment(env);
    }
    if (a.has("fault")) {
        ProbeFault f;
        if (probeFaultFromName(a["fault"].get("type", "none"), f))
            sim.setProbeFault(a["fault"].get("probe", "BT"), f, a["fault"].get("param", 0.0));
    }
}

}  // namespace

bool runScenario(const json::Value& script, const Catalog& catalog, ScenarioResult& out,
                 const std::function<void(const Simulation&)>& onSecond) {
    out = ScenarioResult{};
    const RoasterDef* r = catalog.roaster(script.get("roaster", ""));
    if (!r) {
        out.error = "unknown roaster '" + script.get("roaster", "") + "'";
        return false;
    }
    const BeanSpec* b = catalog.bean(script.get("bean", catalog.beans().empty() ? "" : catalog.beans()[0].id.c_str()));
    if (!b) {
        out.error = "unknown bean '" + script.get("bean", "") + "'";
        return false;
    }
    Simulation sim;
    sim.setSeed(static_cast<std::uint64_t>(script.get("seed", 1.0)));
    sim.setParcelCount(static_cast<int>(script.get("parcels", 200.0)));
    Environment env;
    env.line_voltage = r->nominalLineVoltage();
    applyEnvironmentJson(script["env"], env);
    sim.load(*r, *b, env);
    // Scripts drive a working roaster: switch it on unless the script says otherwise
    // (a "power" action can switch it off mid-roast to model a power cut).
    if (script.getBool("power", true)) sim.setPower(true, "script");

    const double batch = script.get("batch_g", r->batch.default_g);
    if (script.get("preheat_C", 0.0) > 0) {
        // Set the non-heat controls of the first action first so preheat uses them.
        const json::Value& acts = script["actions"];
        if (acts.size() > 0 && acts.at(0).get("at", -1.0) == 0.0) {
            std::string e;
            json::Value first = json::Value::object();
            first["set"] = acts.at(0)["set"];
            first["set"].erase("heat");
            apply(sim, first, e);
        }
        if (sim.preheat(script.get("preheat_C", 0.0)) < 0) {
            out.error = "preheat failed";
            return false;
        }
    }

    std::vector<Action> actions;
    for (const auto& a : script["actions"].items()) actions.push_back({a, false, -1});
    const double maxS = script.get("max_s", 3600.0);
    const double t0 = sim.sessionTime();
    double nextSecond = t0;
    std::uint64_t seenSeq = 0;

    while (sim.sessionTime() - t0 < maxS) {
        double ts = sim.sessionTime() - t0;
        double tr = sim.roastTime();
        double bt = sim.snapshot().bt;
        for (const auto& e : sim.eventsSince(seenSeq)) {
            seenSeq = e.seq;
            for (auto& a : actions)
                if (!a.fired && a.eventSeen < 0 && a.spec.get("event", "") == e.type) a.eventSeen = e.t_session;
        }
        for (auto& a : actions) {
            if (a.fired) continue;
            const json::Value& s = a.spec;
            bool go = false;
            if (s.has("at")) go = ts >= s.get("at", 0.0);
            else if (s.has("roast_at")) go = tr >= 0 && tr >= s.get("roast_at", 0.0);
            else if (s.has("bt_ge")) go = sim.phase() == RoastPhase::Roasting && bt >= s.get("bt_ge", 0.0);
            else if (s.has("event")) go = a.eventSeen >= 0 && sim.sessionTime() >= a.eventSeen + s.get("delay", 0.0);
            if (!go) continue;
            a.fired = true;
            json::Value spec = s;
            if (spec.getBool("charge", false) && !spec.has("batch_g")) spec["batch_g"] = batch;
            std::string err;
            apply(sim, spec, err);
            if (!err.empty()) {
                out.error = err;
                return false;
            }
        }
        sim.step(Simulation::kStep);
        if (onSecond && sim.sessionTime() >= nextSecond) {
            nextSecond += 1.0;
            onSecond(sim);
        }
        if (sim.phase() == RoastPhase::Done) break;
    }
    out.history.assign(sim.history().begin(), sim.history().end());
    out.events = sim.events();
    out.report = sim.report();
    out.sim_seconds = sim.sessionTime() - t0;
    out.ok = true;
    return true;
}

}  // namespace roastsim
