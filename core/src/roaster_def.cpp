#include "roastsim/roaster_def.hpp"

#include <cmath>

namespace roastsim {

const ControlDef* RoasterDef::control(Control c) const {
    for (const auto& cd : controls)
        if (cd.id == c) return &cd;
    return nullptr;
}

int RoasterDef::probeIndex(const std::string& pid) const {
    for (std::size_t i = 0; i < probes.size(); ++i)
        if (probes[i].id == pid) return static_cast<int>(i);
    return -1;
}

double RoasterDef::nominalLineVoltage() const {
    double volts = 0;
    switch (kind) {
        case RoasterKind::FluidBed: volts = fluid.nominal_V; break;
        case RoasterKind::Drum: volts = drum.nominal_V; break;
        case RoasterKind::StirPan: volts = pan.nominal_V; break;
    }
    return volts > 0 && std::isfinite(volts) ? volts : Environment{}.line_voltage;
}

namespace {

void readFluid(const json::Value& v, FluidBedParams& p) {
    if (!v.isObject()) return;
    p.wiring = v.get("wiring", p.wiring.c_str());
#define RS_F(n) p.n = v.get(#n, p.n)
    RS_F(heater_W); RS_F(nominal_V); RS_F(element_C); RS_F(element_eff); RS_F(element_loss_UA);
    RS_F(fan_max_flow); RS_F(fan_tau); RS_F(fan_min_pct); RS_F(stock_stall_frac); RS_F(wall_C);
    RS_F(wall_UA_in); RS_F(wall_UA_out); RS_F(h_ref); RS_F(h_flow_exp); RS_F(fluidize_flow_per_kg);
    RS_F(eject_agitation); RS_F(unevenness); RS_F(bt_probe_full_kg); RS_F(thermostat_open_C); RS_F(thermostat_close_C);
    RS_F(fuse_C); RS_F(fuse_hold_s); RS_F(pwm_period_s); RS_F(chamber_diameter_m); RS_F(chamber_height_m);
#undef RS_F
    p.thermostat = v.getBool("thermostat", p.thermostat);
}

void readDrum(const json::Value& v, DrumParams& p) {
    if (!v.isObject()) return;
    p.heat_source = v.get("heat_source", p.heat_source.c_str());
#define RS_F(n) p.n = v.get(#n, p.n)
    RS_F(heat_max_W); RS_F(heat_min_frac); RS_F(heat_tau); RS_F(efficiency); RS_F(frac_drum);
    RS_F(frac_air); RS_F(frac_beans_rad); RS_F(nominal_V); RS_F(drum_mass_kg); RS_F(drum_cp);
    RS_F(drum_diameter_m); RS_F(drum_length_m); RS_F(outer_frac); RS_F(wall_G); RS_F(loss_UA);
    RS_F(airflow_max); RS_F(airflow_min_frac); RS_F(fan_tau); RS_F(h_air_ref); RS_F(h_flow_exp);
    RS_F(wall_air_UA); RS_F(h_contact); RS_F(emissivity); RS_F(rpm_max); RS_F(unevenness);
    RS_F(bt_probe_full_kg);
#undef RS_F
    p.double_wall = v.getBool("double_wall", p.double_wall);
}

void readPan(const json::Value& v, StirPanParams& p) {
    if (!v.isObject()) return;
#define RS_F(n) p.n = v.get(#n, p.n)
    RS_F(heater_W); RS_F(nominal_V); RS_F(pan_C); RS_F(pan_loss_UA); RS_F(pan_diameter_m);
    RS_F(thermostat_open_C); RS_F(thermostat_close_C); RS_F(h_contact); RS_F(h_dome); RS_F(pan_air_UA);
    RS_F(dome_vent_UA); RS_F(dome_loss_UA); RS_F(fan_max_flow); RS_F(stir_rpm_max); RS_F(unevenness);
    RS_F(pwm_period_s); RS_F(bt_probe_full_g); RS_F(dome_height_m);
#undef RS_F
    p.thermostat = v.getBool("thermostat", p.thermostat);
}

}  // namespace

bool RoasterDef::fromJson(const json::Value& v, RoasterDef& out, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    if (!v.isObject()) return fail("roaster definition must be a JSON object");
    RoasterDef d;
    d.raw = v;
    d.id = v.get("id", "");
    if (d.id.empty()) return fail("roaster definition needs an \"id\"");
    d.name = v.get("name", d.id.c_str());
    d.description = v.get("description", "");
    if (!roasterKindFromName(v.get("kind", ""), d.kind))
        return fail("roaster '" + d.id + "': unknown kind (fluid_bed | drum | stir_pan)");

    const json::Value& b = v["batch"];
    d.batch.default_g = b.get("default_g", d.batch.default_g);
    d.batch.min_g = b.get("min_g", d.batch.min_g);
    d.batch.max_g = b.get("max_g", d.batch.max_g);
    d.charge_bean_temp_C = v.get("charge_bean_temp_C", d.charge_bean_temp_C);

    for (const auto& c : v["controls"].items()) {
        ControlDef cd;
        if (!controlFromName(c.get("id", ""), cd.id))
            return fail("roaster '" + d.id + "': unknown control id '" + c.get("id", "") + "'");
        cd.label = c.get("label", controlName(cd.id));
        cd.unit = c.get("unit", "%");
        cd.def = c.get("default", 0.0);
        cd.min = c.get("min", 0.0);
        cd.max = c.get("max", 100.0);
        cd.step = c.get("step", 1.0);
        cd.toggle = c.getBool("toggle", false);
        cd.slew_per_s = c.get("slew_per_s", 0.0);
        d.controls.push_back(cd);
    }
    // Every machine drops into a cooling tray; expose its switch if not declared.
    if (!d.control(Control::Cooling)) {
        ControlDef cd;
        cd.id = Control::Cooling;
        cd.label = "Cooling tray";
        cd.def = 100;
        cd.toggle = true;
        d.controls.push_back(cd);
    }

    for (const auto& p : v["probes"].items()) {
        ProbeDef pd;
        pd.id = p.get("id", "");
        if (pd.id.empty()) return fail("roaster '" + d.id + "': probe without id");
        pd.label = p.get("label", pd.id.c_str());
        if (!probeLocationFromName(p.get("location", ""), pd.loc))
            return fail("roaster '" + d.id + "': probe '" + pd.id + "' has unknown location");
        pd.tau_air_s = p.get("tau_air_s", pd.tau_air_s);
        pd.tau_bean_s = p.get("tau_bean_s", pd.tau_bean_s);
        pd.noise_sd = p.get("noise_sd", pd.noise_sd);
        pd.resolution = p.get("resolution", pd.resolution);
        pd.offset = p.get("offset", pd.offset);
        d.probes.push_back(pd);
    }
    if (d.probeIndex("BT") < 0) return fail("roaster '" + d.id + "' must define a probe with id \"BT\"");
    if (d.probeIndex("ET") < 0) return fail("roaster '" + d.id + "' must define a probe with id \"ET\"");

    readFluid(v["fluid_bed"], d.fluid);
    readDrum(v["drum"], d.drum);
    readPan(v["stir_pan"], d.pan);
    const json::Value& c = v["cooling"];
    d.cool.h_on = c.get("h_on", d.cool.h_on);
    d.cool.h_off = c.get("h_off", d.cool.h_off);

    out = std::move(d);
    return true;
}

json::Value RoasterDef::describe() const {
    json::Value v = json::Value::object();
    v["id"] = id;
    v["name"] = name;
    v["description"] = description;
    v["kind"] = roasterKindName(kind);
    json::Value b = json::Value::object();
    b["default_g"] = batch.default_g;
    b["min_g"] = batch.min_g;
    b["max_g"] = batch.max_g;
    v["batch"] = b;
    json::Value cs = json::Value::array();
    for (const auto& c : controls) {
        json::Value o = json::Value::object();
        o["id"] = controlName(c.id);
        o["label"] = c.label;
        o["unit"] = c.unit;
        o["default"] = c.def;
        o["min"] = c.min;
        o["max"] = c.max;
        o["step"] = c.step;
        o["toggle"] = c.toggle;
        cs.push(o);
    }
    v["controls"] = cs;
    json::Value ps = json::Value::array();
    for (const auto& p : probes) {
        json::Value o = json::Value::object();
        o["id"] = p.id;
        o["label"] = p.label;
        o["location"] = probeLocationName(p.loc);
        ps.push(o);
    }
    v["probes"] = ps;
    return v;
}

}  // namespace roastsim
