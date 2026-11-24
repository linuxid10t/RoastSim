// The JSON command API shared by every front end. Documented in docs/API.md.
#include <cmath>

#include "internal.hpp"
#include "roastsim/scenario.hpp"

namespace roastsim {

namespace {

json::Value r1(double v, int d = 1) {
    if (!std::isfinite(v)) return json::Value();
    double p = std::pow(10.0, d);
    return json::Value(std::round(v * p) / p);
}

json::Value ok() {
    json::Value v = json::Value::object();
    v["ok"] = true;
    return v;
}

json::Value fail(const std::string& msg) {
    json::Value v = json::Value::object();
    v["ok"] = false;
    v["error"] = msg;
    return v;
}

json::Value alarmList(std::uint32_t a) {
    static const struct { std::uint32_t bit; const char* name; } names[] = {
        {AlarmThermostatOpen, "thermostat_open"}, {AlarmFuseBlown, "fuse_blown"},
        {AlarmBeansEjecting, "beans_ejecting"},   {AlarmBedStalled, "bed_stalled"},
        {AlarmScorching, "scorching"},            {AlarmSmokeHeavy, "smoke_heavy"},
        {AlarmFireRisk, "fire_risk"},             {AlarmOverheat, "overheat"},
        {AlarmCentrifuging, "centrifuging"},      {AlarmProbeFault, "probe_fault"},
    };
    json::Value v = json::Value::array();
    for (const auto& n : names)
        if (a & n.bit) v.push(n.name);
    return v;
}

json::Value envJson(const Environment& e) {
    json::Value v = json::Value::object();
    v["ambient_C"] = r1(e.ambient_C);
    v["humidity_rh"] = r1(e.humidity_rh);
    v["line_voltage"] = r1(e.line_voltage);
    v["source_ohms"] = r1(e.source_ohms, 3);
    v["altitude_m"] = r1(e.altitude_m, 0);
    v["gas_pressure"] = r1(e.gas_pressure, 2);
    return v;
}

json::Value eventJson(const SimEvent& e) {
    json::Value v = json::Value::object();
    v["seq"] = static_cast<double>(e.seq);
    v["type"] = e.type;
    if (!e.detail.empty()) v["detail"] = e.detail;
    v["origin"] = e.origin;
    v["t"] = r1(e.t_session, 2);
    v["t_roast"] = r1(e.t_roast, 2);
    v["bt"] = r1(e.bt);
    v["power"] = e.powered;
    return v;
}

json::Value sampleJson(const HistorySample& h) {
    // Compact array form: [t, t_roast, bt, et, ror_bt, ror_et, bean_true, heat, fan, drum]
    json::Value a = json::Value::array();
    a.push(r1(h.t, 1));
    a.push(r1(h.t_roast, 1));
    a.push(r1(h.bt, 2));
    a.push(r1(h.et, 2));
    a.push(r1(h.ror_bt, 2));
    a.push(r1(h.ror_et, 2));
    a.push(r1(h.bean_true, 2));
    a.push(r1(h.heat, 1));
    a.push(r1(h.fan, 1));
    a.push(r1(h.drum, 1));
    return a;
}

}  // namespace

json::Value Host::stateJson(bool full) const {
    Snapshot s;
    json::Value def;
    double batch, scale, nominalVoltage;
    std::uint64_t catalogRevision, roasterRevision;
    bool paused, customRoaster;
    std::uint64_t historyStart;
    {
        std::lock_guard<std::mutex> lk(mu_);
        s = sim_.snapshot();
        historyStart = sim_.historyBase();
        if (full) def = sim_.roaster().describe();
        nominalVoltage = sim_.roaster().nominalLineVoltage();
        catalogRevision = catalogRevision_;
        roasterRevision = roasterRevision_;
        customRoaster = userRoasters_.count(s.roaster_id) != 0;
        batch = batchG_;
        scale = timeScale_;
        paused = paused_;
    }
    json::Value v = json::Value::object();
    v["type"] = "state";
    v["catalog_revision"] = static_cast<double>(catalogRevision);
    v["t"] = r1(s.t_session, 2);
    v["t_roast"] = r1(s.t_roast, 2);
    v["phase"] = roastPhaseName(s.phase);
    v["bean_location"] = s.bean_location == BeanLocation::Machine ? "machine"
                         : s.bean_location == BeanLocation::Tray  ? "tray"
                                                                  : "none";
    json::Value ro = json::Value::object();
    ro["id"] = s.roaster_id;
    ro["name"] = s.roaster_name;
    ro["kind"] = roasterKindName(s.kind);
    ro["nominal_V"] = nominalVoltage;
    ro["revision"] = static_cast<double>(roasterRevision);
    ro["custom"] = customRoaster;
    v["roaster"] = ro;
    json::Value be = json::Value::object();
    be["id"] = s.bean_id;
    be["name"] = s.bean_name;
    v["bean"] = be;
    if (full) v["roaster_def"] = def;
    v["bt"] = r1(s.bt, 2);
    v["et"] = r1(s.et, 2);
    v["ror_bt"] = r1(s.ror_bt, 2);
    v["ror_et"] = r1(s.ror_et, 2);
    v["eta_dry_s"] = s.eta_dry_s < 0 ? s.eta_dry_s : r1(s.eta_dry_s, 0);  // -1 n/a, -2 stalling
    v["eta_fc_s"] = s.eta_fc_s < 0 ? s.eta_fc_s : r1(s.eta_fc_s, 0);
    json::Value probes = json::Value::array();
    for (const auto& p : s.probes) {
        json::Value o = json::Value::object();
        o["id"] = p.id;
        o["location"] = probeLocationName(p.loc);
        o["value"] = r1(p.value, 2);
        o["fault"] = p.fault;
        probes.push(o);
    }
    v["probes"] = probes;
    json::Value ctl = json::Value::object();
    for (int i = 0; i < kControlCount; ++i) {
        if (!s.has_control[i]) continue;
        json::Value o = json::Value::object();
        o["value"] = r1(s.controls[i]);
        o["actual"] = r1(s.actuators[i]);
        ctl[controlName(static_cast<Control>(i))] = o;
    }
    v["controls"] = ctl;
    json::Value m = json::Value::object();
    m["heater_W"] = r1(s.tele.heater_W, 0);
    m["voltage_V"] = r1(s.tele.voltage_V);
    m["airflow_g_s"] = r1(s.tele.airflow_kg_s * 1000.0, 2);
    m["inlet_C"] = r1(s.tele.inlet_C);
    m["exhaust_C"] = r1(s.tele.exhaust_C);
    m["air_C"] = r1(s.tele.air_C);
    m["wall_C"] = r1(s.tele.wall_C);
    m["bean_env_C"] = s.tele.bean_env_C < 0 ? -1.0 : r1(s.tele.bean_env_C);
    m["element_C"] = r1(s.tele.element_C);
    m["agitation"] = r1(s.tele.agitation, 2);
    m["mix_quality"] = r1(s.tele.mix_quality, 2);
    m["fill"] = r1(s.tele.fill, 3);
    m["drum_rpm"] = r1(s.tele.drum_rpm);
    m["stir_rpm"] = r1(s.tele.stir_rpm);
    m["bed_state"] = s.tele.bed_state;
    m["bt_immersion"] = r1(s.tele.bt_immersion, 2);
    v["machine"] = m;
    json::Value b = json::Value::object();
    b["present"] = s.beans.present;
    if (s.beans.present) {
        b["mass_g"] = r1(s.beans.mass_g);
        b["weight_loss_pct"] = r1(s.beans.weight_loss_pct, 2);
        b["moisture_pct"] = r1(s.beans.moisture_pct, 2);
        b["T_surface"] = r1(s.beans.T_surface);
        b["T_core"] = r1(s.beans.T_core);
        b["T_mean"] = r1(s.beans.T_mean);
        b["T_min"] = r1(s.beans.T_min);
        b["T_max"] = r1(s.beans.T_max);
        b["agtron_whole"] = r1(s.beans.agtron_whole);
        b["agtron_ground"] = r1(s.beans.agtron_ground);
        b["agtron_sd"] = r1(s.beans.agtron_sd);
        b["fc_frac"] = r1(s.beans.fc_frac, 3);
        b["sc_frac"] = r1(s.beans.sc_frac, 3);
        b["oil"] = r1(s.beans.oil, 2);
        b["scorch_frac"] = r1(s.beans.scorch_frac, 3);
        b["tipping_frac"] = r1(s.beans.tipping_frac, 3);
        b["expansion"] = r1(s.beans.expansion, 3);
        json::Value rgb = json::Value::array();
        for (double c : s.beans.rgb) rgb.push(r1(c, 3));
        b["rgb"] = rgb;
        b["level"] = roastLevelName(s.beans.agtron_ground);
    }
    v["beans"] = b;
    v["batch_g"] = r1(s.bean_location == BeanLocation::None ? batch : s.batch_g);
    v["next_batch_g"] = r1(batch);
    v["smoke"] = r1(s.smoke, 3);
    v["chaff_g_s"] = r1(s.chaff_rate_g_s, 3);
    v["pops_fc"] = static_cast<double>(s.pops_fc);
    v["pops_sc"] = static_cast<double>(s.pops_sc);
    v["alarms"] = alarmList(s.alarms);
    v["env"] = envJson(s.env);
    v["time_scale"] = scale;
    v["paused"] = paused;
    v["power"] = s.powered;
    v["logger_connected"] = loggerConnected();
    v["event_seq"] = static_cast<double>(s.last_event_seq);
    // Index of the oldest history sample the simulator still holds. A cold
    // reset or roaster load moves it past every earlier sample: front ends
    // that keep their own copy drop anything below it.
    v["history_start"] = static_cast<double>(historyStart);
    v["units"] = unitsJson();
    return v;
}

json::Value Host::command(const json::Value& cmd, const std::string& origin) {
    const std::string c = cmd.get("cmd", "");
    if (c.empty()) return fail("missing \"cmd\"");

    if (c == "state") return stateJson(cmd.getBool("full", false));
    if (c == "list") {
        std::lock_guard<std::mutex> lk(mu_);
        json::Value v = catalog_.listJson();
        v["ok"] = true;
        v["catalog_revision"] = static_cast<double>(catalogRevision_);
        return v;
    }
    if (c == "preset_types" || c == "preset_form" || c == "preset_save") return presetCommand(cmd);
    if (c == "server" || c == "info") {
        json::Value v = serverInfo();
        v["ok"] = true;
        v["config"] = cfg_.toJson();
        return v;
    }
    if (c == "history") {
        std::uint64_t next = 0;
        auto h = historySince(static_cast<std::uint64_t>(cmd.get("since", 0.0)), &next,
                              static_cast<std::size_t>(cmd.get("max", 20000.0)));
        json::Value v = ok();
        json::Value arr = json::Value::array();
        for (const auto& s : h) arr.push(sampleJson(s));
        v["columns"] = "t,t_roast,bt,et,ror_bt,ror_et,bean_true,heat,fan,drum";
        v["samples"] = arr;
        v["next"] = static_cast<double>(next);
        return v;
    }
    if (c == "events") {
        json::Value v = ok();
        json::Value arr = json::Value::array();
        for (const auto& e : eventsSince(static_cast<std::uint64_t>(cmd.get("since", 0.0)))) arr.push(eventJson(e));
        v["events"] = arr;
        return v;
    }
    if (c == "visual") {
        VisualState vs = visual();
        json::Value v = ok();
        v["frame"] = static_cast<double>(vs.frame);
        v["kind"] = roasterKindName(vs.kind);
        if (vs.kind == RoasterKind::FluidBed) v["air_pattern"] = fluidBedAirPatternInfo(vs.air_pattern).id;
        v["bean_location"] = static_cast<int>(vs.bean_location);
        v["agitation"] = r1(vs.agitation, 3);
        v["mix_quality"] = r1(vs.mix_quality, 3);
        v["angle_rad"] = r1(vs.angle_rad, 3);
        v["fan"] = r1(vs.fan, 3);
        v["heat"] = r1(vs.heat, 3);
        v["element_glow"] = r1(vs.element_glow, 3);
        v["fill"] = r1(vs.fill, 3);
        v["smoke"] = r1(vs.smoke, 3);
        v["chaff_airborne"] = r1(vs.chaff_airborne, 3);
        v["chamber_diameter"] = r1(vs.geometry.chamber_diameter, 3);
        v["chamber_height"] = r1(vs.geometry.chamber_height, 3);
        v["pops_fc"] = static_cast<double>(vs.pops_fc);
        v["pops_sc"] = static_cast<double>(vs.pops_sc);
        // Beans as flat arrays: [r,g,b,size_mm,oil,temp,count,exposure,cracked,scorch] per parcel
        json::Value beans = json::Value::array();
        std::size_t max = static_cast<std::size_t>(cmd.get("max", 1000.0));
        for (std::size_t i = 0; i < vs.beans.size() && i < max; ++i) {
            const VisualBean& b = vs.beans[i];
            json::Value a = json::Value::array();
            a.push(r1(b.rgb[0], 3)); a.push(r1(b.rgb[1], 3)); a.push(r1(b.rgb[2], 3));
            a.push(r1(b.size_mm, 2)); a.push(r1(b.oil, 2)); a.push(r1(b.temp_C, 1));
            a.push(r1(b.count, 1)); a.push(r1(b.exposure, 2)); a.push(r1(b.cracked, 2)); a.push(r1(b.scorch, 2));
            beans.push(a);
        }
        v["beans"] = beans;
        return v;
    }
    if (c == "report") {
        std::lock_guard<std::mutex> lk(mu_);
        json::Value v = sim_.report().toJson();
        v["ok"] = true;
        return v;
    }
    // Settings changed below are saved to roastsim.json (after the sim lock is
    // released) so the next start picks up where this one left off.
    if (c == "load") {
        std::string rid = cmd.get("roaster", "");
        std::string bid = cmd.get("bean", "");
        json::Value saved = json::Value::object();
        {
            std::lock_guard<std::mutex> lk(mu_);
            const RoasterDef* r = rid.empty() ? catalog_.roaster(sim_.roaster().id) : catalog_.roaster(rid);
            const BeanSpec* b = bid.empty() ? catalog_.bean(sim_.bean().id) : catalog_.bean(bid);
            if (!r) return fail("unknown roaster '" + rid + "'");
            if (!b) return fail("unknown bean '" + bid + "'");
            sim_.load(*r, *b);
            ++roasterRevision_;
            batchG_ = r->batch.default_g;
            saved["roaster"] = r->id;
            saved["bean"] = b->id;
            saved["batch_g"] = batchG_;
            saved["env"] = envJson(sim_.environment());  // loading sets the roaster's mains voltage
        }
        saveSettings(saved);
        return ok();
    }
    if (c == "bean") {
        json::Value saved = json::Value::object();
        {
            std::lock_guard<std::mutex> lk(mu_);
            const BeanSpec* b = catalog_.bean(cmd.get("id", cmd.get("bean", "").c_str()));
            if (!b) return fail("unknown bean");
            sim_.setBean(*b);
            saved["bean"] = b->id;
        }
        saveSettings(saved);
        return ok();
    }
    if (c == "set") {
        std::lock_guard<std::mutex> lk(mu_);
        auto setOne = [&](const std::string& name, double val) -> bool {
            Control ctl;
            if (!controlFromName(name, ctl) || !sim_.roaster().hasControl(ctl)) return false;
            sim_.setControl(ctl, val);
            return true;
        };
        if (cmd.has("control")) {
            if (!setOne(cmd.get("control", ""), cmd.get("value", 0.0)))
                return fail("this roaster has no control '" + cmd.get("control", "") + "'");
        }
        const json::Value& cs = cmd["controls"];
        for (const auto& k : cs.keys())
            if (!setOne(k, cs[k].num())) return fail("this roaster has no control '" + k + "'");
        return ok();
    }
    if (c == "batch") {
        double g = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            g = cmd.get("batch_g", cmd.get("value", batchG_));
            if (!(g > 0)) return fail("batch must be > 0");
            batchG_ = g;
        }
        json::Value saved = json::Value::object();
        saved["batch_g"] = g;
        saveSettings(saved);
        return ok();
    }
    if (c == "charge") {
        double g = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            g = cmd.get("batch_g", batchG_);
            std::string err;
            if (!sim_.charge(g, origin, &err)) return fail(err);
            if (g == batchG_) return ok();
            batchG_ = g;
        }
        json::Value saved = json::Value::object();
        saved["batch_g"] = g;
        saveSettings(saved);
        return ok();
    }
    if (c == "drop") {
        std::lock_guard<std::mutex> lk(mu_);
        std::string err;
        if (!sim_.drop(origin, &err)) return fail(err);
        return ok();
    }
    if (c == "cool_end") {
        std::lock_guard<std::mutex> lk(mu_);
        return sim_.coolEnd(origin) ? ok() : fail("not cooling");
    }
    if (c == "mark") {
        std::lock_guard<std::mutex> lk(mu_);
        sim_.markEvent(cmd.get("event", ""), origin);
        return ok();
    }
    if (c == "empty") {
        std::lock_guard<std::mutex> lk(mu_);
        sim_.emptyMachine();
        return ok();
    }
    if (c == "reset") {
        std::lock_guard<std::mutex> lk(mu_);
        sim_.coldReset();
        return ok();
    }
    if (c == "preheat") {
        std::lock_guard<std::mutex> lk(mu_);
        double t = cmd.get("target_C", cmd.get("target", 0.0));
        if (cmd.has("target") && !cmd.has("target_C")) t = fromLoggerUnits(t);
        if (!(t > 40 && t < 450)) return fail("preheat target out of range");
        if (!sim_.powered()) return fail("the machine is powered off - switch it on first");
        bool reached = false;
        double used = sim_.preheat(t, 2400, &reached);
        if (used < 0) return fail("cannot preheat now (beans in the roaster, or no heat control)");
        json::Value v = ok();
        v["simulated_s"] = r1(used, 0);
        v["reached"] = reached;
        return v;
    }
    if (c == "repair") {
        std::lock_guard<std::mutex> lk(mu_);
        sim_.repair();
        return ok();
    }
    if (c == "env") {
        json::Value v = ok();
        {
            std::lock_guard<std::mutex> lk(mu_);
            Environment e = sim_.environment();
            applyEnvironmentJson(cmd, e);
            sim_.setEnvironment(e);
            v["env"] = envJson(e);
        }
        json::Value saved = json::Value::object();
        saved["env"] = v["env"];
        saveSettings(saved);
        return v;
    }
    if (c == "time_scale") {
        double s = cmd.get("value", 1.0);
        if (!(s > 0 && s <= 120)) return fail("time scale must be in (0, 120]");
        {
            std::lock_guard<std::mutex> lk(mu_);
            timeScale_ = s;
        }
        json::Value saved = json::Value::object();
        saved["time_scale"] = s;
        saveSettings(saved);
        return ok();
    }
    if (c == "ui_settings") {
        // {"cmd":"ui_settings","shell":"win32","settings":{...}}: a front end's own
        // settings (sound, window placement...), kept under "ui"."<shell>" in
        // roastsim.json and merged key by key. Without "settings" it just reads them.
        const std::string shell = cmd.get("shell", "");
        if (shell.empty()) return fail("missing \"shell\"");
        const json::Value& add = cmd["settings"];
        json::Value v = ok();
        if (add.isObject()) {
            json::Value ui = json::Value::object(), saved = json::Value::object();
            ui[shell] = add;
            saved["ui"] = ui;
            v["saved"] = saveSettings(saved);
        }
        std::string text, perr;
        json::Value mine = json::Value::object();
        if (!cfg_.config_path.empty() && readTextFile(cfg_.config_path, text) && !text.empty()) {
            const json::Value file = json::Value::parse(text, &perr);
            if (perr.empty() && file["ui"][shell].isObject()) mine = file["ui"][shell];
        }
        v["settings"] = mine;
        return v;
    }
    if (c == "pause") {
        std::lock_guard<std::mutex> lk(mu_);
        paused_ = cmd.getBool("value", !paused_);
        return ok();
    }
    if (c == "power") {
        // Master switch, like "pause": toggles when "value" is absent.
        std::lock_guard<std::mutex> lk(mu_);
        bool on = cmd.getBool("value", !sim_.powered());
        sim_.setPower(on, origin);
        json::Value v = ok();
        v["power"] = on;
        return v;
    }
    if (c == "step") {
        std::lock_guard<std::mutex> lk(mu_);
        sim_.step(clampd(cmd.get("seconds", 1.0), 0.0, 3600.0));
        return ok();
    }
    if (c == "fault") {
        ProbeFault f;
        if (!probeFaultFromName(cmd.get("type", "none"), f)) return fail("unknown fault type");
        std::lock_guard<std::mutex> lk(mu_);
        if (!sim_.setProbeFault(cmd.get("probe", "BT"), f, cmd.get("param", 0.0))) return fail("unknown probe");
        return ok();
    }
    if (c == "units") {
        // {"cmd":"units","temperature":"F","weight":"lb","altitude":"ft"}: any subset.
        // ("value" is accepted as the temperature for older clients.) Saved to roastsim.json.
        std::string t = cmd.get("temperature", cmd.get("value", "").c_str());
        std::string err;
        bool saved = false;
        if (!changeUnits(t.empty() ? 0 : t[0], cmd.get("weight", ""), cmd.get("altitude", ""),
                         cmd.getBool("save", true), &err, &saved))
            return fail(err);
        json::Value v = ok();
        v["units"] = unitsJson();
        v["saved"] = saved;
        return v;
    }
    if (c == "parcels") {
        std::lock_guard<std::mutex> lk(mu_);
        sim_.setParcelCount(static_cast<int>(cmd.get("value", 200.0)));
        return ok();
    }
    return fail("unknown command '" + c + "'");
}

}  // namespace roastsim
