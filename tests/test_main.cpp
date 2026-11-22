// Core tests: JSON, determinism, and physics plausibility for every preset.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "roastsim/catalog.hpp"
#include "roastsim/scenario.hpp"
#include "test_util.hpp"

using namespace roastsim;

namespace {

const Catalog& catalog() {
    static Catalog c;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        c.loadDir(rstest::dataDir());
        for (const auto& e : c.errors()) std::printf("    catalog: %s\n", e.c_str());
    }
    return c;
}

json::Value script(const char* text) {
    std::string err;
    json::Value v = json::Value::parse(text, &err);
    if (!err.empty()) std::printf("    script JSON error: %s\n", err.c_str());
    return v;
}

ScenarioResult run(const char* text) {
    ScenarioResult r;
    if (!runScenario(script(text), catalog(), r)) std::printf("    scenario error: %s\n", r.error.c_str());
    return r;
}

// Plausibility envelope every "sensible" roast must land in.
void checkSensible(const ScenarioResult& r, double fcMin, double fcMax) {
    CHECK(r.ok);
    CHECK(r.report.valid);
    CHECK_RANGE(r.report.fcs_s, fcMin, fcMax);
    CHECK_RANGE(r.report.weight_loss_pct, 9.0, 18.0);
    CHECK_RANGE(r.report.moisture_pct, 0.8, 4.5);
    CHECK_RANGE(r.report.agtron_ground, 40.0, 95.0);
    CHECK_RANGE(r.report.tp_s, 3.0, 120.0);
    CHECK(r.report.dry_s > r.report.tp_s);
    CHECK(r.report.fcs_s > r.report.dry_s);
    CHECK(r.report.drop_s > r.report.fcs_s);
}

void startPilotRoast(Simulation& sim, double batch, double airflow, double inlet) {
    const auto& roaster = *catalog().roaster("spouted_bed_pilot");
    const auto& fluid = roaster.fluid;
    Environment env;
    env.ambient_C = 20;
    env.source_ohms = 0;
    sim.setEnvironment(env);
    sim.setSeed(7);
    sim.load(roaster, *catalog().bean("kenya_pilot_blend"));
    sim.setPower(true, "test");
    sim.setControl(Control::Fan, 100 * airflow / fluid.fan_max_flow);
    // Electrical power required for the measured inlet temperature at steady state.
    const double heat = (inlet - env.ambient_C) * (airflow * kAirCp + fluid.element_loss_UA / fluid.element_eff);
    sim.setControl(Control::Heat, 100 * heat / fluid.heater_W);
    sim.step(600);
    CHECK(sim.charge(batch, "test"));
}

}  // namespace

TEST(json_roundtrip) {
    std::string err;
    json::Value v = json::Value::parse(R"({"a": [1, 2.5, -3e2, true, null, "x\"é"], "b": {"c": "d"}})", &err);
    CHECK(err.empty());
    CHECK(v["a"].size() == 6);
    CHECK(v["a"][1].num() == 2.5);
    CHECK(v["a"][2].num() == -300);
    CHECK(v["a"][5].str() == "x\"\xC3\xA9");
    json::Value w = json::Value::parse(v.dump(), &err);
    CHECK(err.empty());
    CHECK(w.dump() == v.dump());
    json::Value bad = json::Value::parse("{\"a\": }", &err);
    CHECK(!err.empty());
    CHECK(json::formatNumber(0.1 + 0.2, 3) == "0.3");
}

TEST(catalog_loads_all_presets) {
    CHECK(catalog().errors().empty());
    CHECK(catalog().roasters().size() >= 8);
    CHECK(catalog().beans().size() >= 6);
    CHECK(catalog().roaster("popper_stock") != nullptr);
    CHECK(catalog().roaster("stir_pan") != nullptr);
}

TEST(electric_roaster_families_use_rated_voltage) {
    Simulation sim;
    const auto& bean = *catalog().bean("colombia_washed");
    RoasterDef drum = *catalog().roaster("drum_home_1kg");
    drum.drum.nominal_V = 240;
    sim.load(drum, bean);
    CHECK(sim.environment().line_voltage == 240);
    RoasterDef pan = *catalog().roaster("stir_pan");
    pan.pan.nominal_V = 220;
    sim.load(pan, bean);
    CHECK(sim.environment().line_voltage == 220);
    sim.load(*catalog().roaster("drum_shop_5kg_gas"), bean);
    CHECK(sim.environment().line_voltage == 120);
}

TEST(scenario_uses_rated_voltage_unless_overridden) {
    auto checkVoltage = [](const char* recipe, double volts) {
        ScenarioResult result;
        bool sampled = false;
        CHECK(runScenario(script(recipe), catalog(), result, [&](const Simulation& sim) {
            sampled = true;
            CHECK(sim.environment().line_voltage == volts);
            CHECK(std::abs(sim.snapshot().tele.voltage_V - volts) < 1e-9);
        }));
        CHECK(sampled);
    };
    checkVoltage(R"({"roaster":"spouted_bed_pilot","max_s":0.05,"env":{"source_ohms":0}})", 230);
    checkVoltage(R"({"roaster":"spouted_bed_pilot","max_s":0.05,"env":{"source_ohms":0,"line_voltage":218}})", 218);
}

TEST(deterministic_with_seed) {
    const char* s = R"({"roaster":"popper_modded","bean":"colombia_washed","seed":42,"max_s":600,
        "actions":[{"at":0,"set":{"heat":100,"fan":80}},{"at":10,"charge":true},{"roast_at":300,"drop":true}]})";
    ScenarioResult a = run(s), b = run(s);
    CHECK(a.history.size() == b.history.size());
    bool same = a.history.size() == b.history.size();
    for (std::size_t i = 0; same && i < a.history.size(); ++i)
        same = a.history[i].bt == b.history[i].bt && a.history[i].et == b.history[i].et;
    CHECK(same);
}

TEST(popper_modded_roast) {
    checkSensible(run(R"({"roaster":"popper_modded","bean":"colombia_washed","seed":1,"max_s":1500,
        "actions":[{"at":0,"set":{"heat":100,"fan":85}},{"at":30,"charge":true},{"bt_ge":170,"set":{"fan":70}},
                   {"event":"FCs","delay":75,"drop":true}]})"), 150, 360);
}

TEST(popper_stock_roast_and_thermostat) {
    ScenarioResult r = run(R"({"roaster":"popper_stock","bean":"colombia_washed","seed":3,"max_s":1500,
        "actions":[{"at":0,"set":{"voltage":100}},{"at":20,"charge":true},{"event":"FCs","delay":60,"drop":true}]})");
    checkSensible(r, 150, 420);
    // At 110 % line voltage with no beans, the bimetal cut-out must cycle.
    ScenarioResult hot = run(R"({"roaster":"popper_stock","seed":3,"max_s":200,"env":{"line_voltage":132},
        "actions":[{"at":0,"set":{"voltage":110}}]})");
    bool tripped = false;
    for (const auto& e : hot.events) tripped |= e.type == "ALARM" && e.detail == "thermostat_open";
    CHECK(tripped);
}

TEST(air_roaster_roast) {
    checkSensible(run(R"({"roaster":"air_roaster","bean":"colombia_washed","seed":5,"max_s":2400,
        "actions":[{"at":0,"set":{"heat":100,"fan":90}},{"at":20,"charge":true},{"roast_at":60,"set":{"fan":75}},
                   {"bt_ge":180,"set":{"fan":65}},{"event":"FCs","set":{"heat":80}},{"event":"FCs","delay":90,"drop":true}]})"),
                  200, 480);
}

TEST(air_roaster_low_fan_stalls_bed) {
    ScenarioResult r = run(R"({"roaster":"air_roaster","seed":5,"batch_g":225,"max_s":200,
        "actions":[{"at":0,"set":{"heat":60,"fan":25}},{"at":5,"charge":true}]})");
    bool stalled = false;
    for (const auto& e : r.events) stalled |= e.type == "ALARM" && e.detail == "bed_stalled";
    CHECK(stalled);
}

TEST(fluid_bed_air_patterns_change_circulation) {
    const auto& base = *catalog().roaster("air_roaster");
    CHECK(base.fluid.air_pattern == FluidBedAirPattern::Upflow);
    auto runPattern = [&](FluidBedAirPattern pattern, double fan, double batch) {
        auto def = base;
        def.fluid.air_pattern = pattern;
        Simulation sim;
        sim.setSeed(5);
        sim.load(def, *catalog().bean("colombia_washed"));
        sim.setPower(true, "test");
        sim.setControl(Control::Fan, fan);
        sim.setControl(Control::Heat, 80);
        sim.step(15);
        CHECK(sim.charge(batch, "test"));
        sim.step(30);
        CHECK(sim.visual().air_pattern == pattern);
        return sim.snapshot();
    };

    const auto upflow = runPattern(FluidBedAirPattern::Upflow, 52, 150);
    const auto spout = runPattern(FluidBedAirPattern::CentralSpout, 52, 150);
    const auto swirl = runPattern(FluidBedAirPattern::TangentialSwirl, 52, 150);
    const auto side = runPattern(FluidBedAirPattern::SideJet, 52, 150);
    CHECK(spout.tele.mix_quality > side.tele.mix_quality);
    CHECK(side.tele.mix_quality > upflow.tele.mix_quality);
    CHECK(upflow.tele.mix_quality > swirl.tele.mix_quality);
    CHECK(std::abs(spout.tele.airflow_kg_s - swirl.tele.airflow_kg_s) < 1e-12);
    CHECK(std::abs(spout.beans.T_mean - swirl.beans.T_mean) > 0.1);

    const auto lightSpout = runPattern(FluidBedAirPattern::CentralSpout, 100, 80);
    const auto lightSwirl = runPattern(FluidBedAirPattern::TangentialSwirl, 100, 80);
    CHECK((lightSpout.tele.alarms & AlarmBeansEjecting) != 0);
    CHECK((lightSwirl.tele.alarms & AlarmBeansEjecting) == 0);

    auto bad = base.raw;
    bad["fluid_bed"]["air_pattern"] = "unknown";
    RoasterDef rejected;
    CHECK(!RoasterDef::fromJson(bad, rejected, nullptr));
}

TEST(fluid_bed_wall_heat_balances_during_fan_coastdown) {
    RoasterDef def = *catalog().roaster("air_roaster");
    def.fluid.wall_UA_in = 90;
    def.fluid.wall_UA_out = 0;
    auto machine = createMachine(def);
    Environment env;
    MachineInputs in;
    in.env = &env;
    machine->reset(env);
    in.u[static_cast<int>(Control::Heat)] = 1;
    constexpr double dt = Simulation::kStep;
    for (int i = 0; i < 200; ++i) machine->step(dt, in);
    CHECK(machine->telemetry().element_C > env.ambient_C + 100);
    CHECK(machine->telemetry().wall_C == env.ambient_C);

    auto checkExchange = [&] {
        const double previousWall = machine->telemetry().wall_C;
        machine->step(dt, in);
        const auto& state = machine->telemetry();
        const double wallEnergy = def.fluid.wall_C * (state.wall_C - previousWall);
        const double airEnergy = state.airflow_kg_s * kAirCp * (state.inlet_C - state.exhaust_C) * dt;
        CHECK(std::abs(wallEnergy - airEnergy) < 1e-8);
        CHECK_RANGE(state.exhaust_C, std::min(previousWall, state.inlet_C) - 1e-9,
                    std::max(previousWall, state.inlet_C) + 1e-9);
    };
    in.u[static_cast<int>(Control::Fan)] = 1;
    for (int i = 0; i < 300; ++i) checkExchange();
    in.u[static_cast<int>(Control::Heat)] = 0;
    in.u[static_cast<int>(Control::Fan)] = 0;
    for (int i = 0; i < 300; ++i) checkExchange();
}

TEST(bean_drying_conserves_mass_and_responds_to_humidity) {
    auto evaporated = [](double humidity) {
        Environment env;
        env.ambient_C = 35;
        env.humidity_rh = humidity;
        Rng rng;
        BeanBatch beans;
        beans.init(*catalog().bean("colombia_washed"), 0.1, 3, 35, rng);
        const double initialMass = beans.totalMass();
        double evaporated = 0, lostMass = 0;
        for (int i = 0; i < 2400; ++i) {
            for (auto& p : beans.parcels()) {
                p.T_s = 165;
                p.T_c = 145;
            }
            const auto step = beans.integrate(Simulation::kStep, rng, env);
            evaporated += step.water_evap_kg;
            lostMass += step.water_evap_kg + step.organic_loss_kg + step.chaff_released_kg;
        }
        const double surface = beans.kinetics().surface_frac;
        for (const auto& p : beans.parcels()) {
            CHECK(p.water_s > 0);
            CHECK(p.water_c / (1.0 - surface) > p.water_s / surface);
        }
        CHECK(std::abs(initialMass - beans.totalMass() - lostMass) < 1e-12);
        return evaporated;
    };
    const double dryAir = evaporated(10);
    const double humidAir = evaporated(90);
    CHECK(dryAir > humidAir);
    CHECK(humidAir > 0);
}

TEST(spouted_bed_reproduces_measured_roasts) {
    std::string raw, parseError;
    CHECK(readTextFile(rstest::dataDir() + "/calibration/spouted_bed_measurements.json", raw));
    const auto roasts = json::Value::parse(raw, &parseError);
    CHECK(parseError.empty());
    CHECK(roasts.size() == 3);
    for (const auto& roast : roasts.items()) {
        const double batch = roast.get("batch_g", 0.0), flow = roast.get("airflow_kg_s", 0.0);
        const double inlet = roast.get("inlet_setpoint_C", 0.0);
        Simulation sim;
        startPilotRoast(sim, batch, flow, inlet);
        double t = 0, btError = 0, inletError = 0, massError = 0, moistureError = 0;
        for (const auto& sample : roast["bt_C"].items()) {
            sim.step(sample.at(0).num() - t);
            t = sample.at(0).num();
            const auto s = sim.snapshot();
            CHECK(std::abs(s.tele.airflow_kg_s - flow) < 1e-7);
            btError += std::pow(s.bt - sample[1].num(), 2);
        }
        const auto& airSamples = roast["inlet_C"].items();
        // The measured points are the inlet plateau, after the charge transient.
        const double plateau = sim.snapshot().et;
        for (const auto& sample : airSamples) inletError += std::pow(plateau - sample[1].num(), 2);
        for (const auto& sample : roast["cooled_bean_samples"].items()) {
            Simulation cooled;
            startPilotRoast(cooled, batch, flow, inlet);
            cooled.step(sample.at(0).num());
            CHECK(cooled.drop("test"));
            cooled.step(60);
            const auto s = cooled.snapshot();
            massError += std::pow(s.beans.weight_loss_pct - sample[1].num(), 2);
            moistureError += std::pow(s.beans.moisture_pct - sample[2].num(), 2);
        }
        const auto rmse = [&](double error, const char* key) { return std::sqrt(error / roast[key].size()); };
        const bool holdout = roast.get("split", "") == "holdout";
        CHECK_RANGE(rmse(btError, "bt_C"), 0, holdout ? 15 : 10);
        CHECK_RANGE(rmse(massError, "cooled_bean_samples"), 0, 2);
        CHECK_RANGE(rmse(moistureError, "cooled_bean_samples"), 0, 1);
        if (!airSamples.empty()) CHECK_RANGE(rmse(inletError, "inlet_C"), 0, 3);
    }
}

TEST(spouted_bed_fan_step_changes_inlet_and_bean_heating) {
    Simulation steady, stepped;
    for (Simulation* sim : {&steady, &stepped}) {
        startPilotRoast(*sim, 500, 0.0228, 250);
        sim->step(120);
    }
    stepped.setControl(Control::Fan, 100);
    steady.step(45);
    stepped.step(45);
    const auto before = steady.snapshot(), after = stepped.snapshot();
    CHECK(before.tele.airflow_kg_s < after.tele.airflow_kg_s);
    CHECK(before.tele.inlet_C > after.tele.inlet_C + 20);
    CHECK(before.beans.T_mean > after.beans.T_mean);
}

TEST(drum_home_roast) {
    checkSensible(run(R"({"roaster":"drum_home_1kg","bean":"colombia_washed","batch_g":500,"seed":2,"max_s":2400,
        "preheat_C":190,"actions":[{"at":0,"set":{"fan":30,"drum":60}},{"at":0,"set":{"heat":85}},{"at":1,"charge":true},
        {"bt_ge":160,"set":{"heat":75,"fan":40}},{"event":"FCs","set":{"heat":55,"fan":55}},
        {"event":"FCs","delay":100,"drop":true}]})"),
                  300, 720);
}

TEST(drum_shop_roast) {
    checkSensible(run(R"({"roaster":"drum_shop_5kg_gas","bean":"colombia_washed","batch_g":5000,"seed":3,"max_s":3000,
        "preheat_C":210,"actions":[{"at":0,"set":{"fan":40,"drum":75}},{"at":0,"set":{"heat":70}},{"at":1,"charge":true},
        {"bt_ge":165,"set":{"heat":55,"fan":55}},{"event":"FCs","set":{"heat":35,"fan":70}},
        {"event":"FCs","delay":110,"drop":true}]})"),
                  420, 840);
}

TEST(drum_ir_roast) {
    checkSensible(run(R"({"roaster":"drum_tabletop_ir","bean":"ethiopia_natural","seed":6,"max_s":2400,
        "actions":[{"at":0,"set":{"heat":100,"fan":30}},{"at":90,"charge":true},{"event":"FCs","set":{"heat":70,"fan":60}},
                   {"event":"FCs","delay":90,"drop":true}]})"),
                  200, 600);
}

TEST(stir_pan_roast_and_scorch_without_stirring) {
    checkSensible(run(R"({"roaster":"stir_pan","bean":"colombia_washed","seed":4,"max_s":2400,
        "actions":[{"at":0,"set":{"heat":100,"stir":70,"fan":30}},{"at":60,"charge":true},
                   {"event":"FCs","set":{"heat":75,"fan":60}},{"event":"FCs","delay":90,"drop":true}]})"),
                  180, 540);
    ScenarioResult still = run(R"({"roaster":"stir_pan","bean":"colombia_washed","seed":4,"max_s":2400,
        "actions":[{"at":0,"set":{"heat":100,"stir":0,"fan":30}},{"at":120,"charge":true},{"roast_at":420,"drop":true}]})");
    CHECK(still.report.valid);
    CHECK(still.report.scorch_pct > 5.0);
    CHECK(still.report.agtron_sd > 4.0);
}

TEST(scorch_needs_a_hot_spot_not_just_a_dark_roast) {
    // Fluidized beans roasted very dark (surface past 240 C) are dark, not burnt.
    ScenarioResult dark = run(R"({"roaster":"air_roaster","bean":"colombia_washed","seed":3,"max_s":2400,
        "actions":[{"at":0,"set":{"heat":100,"fan":75}},{"at":1,"charge":true},{"roast_at":530,"drop":true}]})");
    CHECK(dark.report.valid);
    CHECK(dark.report.agtron_ground < 32);
    CHECK(dark.report.scorch_pct == 0);
    // Starve the bed of air and the beans rest in the hot jet off the heater.
    ScenarioResult stalled = run(R"({"roaster":"air_roaster","bean":"colombia_washed","seed":3,"max_s":2400,
        "actions":[{"at":0,"set":{"heat":100,"fan":40}},{"at":1,"charge":true},{"roast_at":400,"drop":true}]})");
    CHECK(stalled.report.valid);
    CHECK(stalled.report.scorch_pct > 20);
}

TEST(darker_drop_means_darker_roast) {
    auto at = [](int delay) {
        std::string s = R"({"roaster":"popper_modded","bean":"colombia_washed","seed":9,"max_s":1500,
            "actions":[{"at":0,"set":{"heat":100,"fan":80}},{"at":20,"charge":true},{"event":"FCs","delay":)" +
                        std::to_string(delay) + R"(,"drop":true}]})";
        ScenarioResult r;
        runScenario(script(s.c_str()), catalog(), r);
        return r.report;
    };
    RoastReport light = at(30), dark = at(150);
    CHECK(dark.agtron_ground < light.agtron_ground - 8);
    CHECK(dark.weight_loss_pct > light.weight_loss_pct);
    CHECK(dark.moisture_pct < light.moisture_pct);
}

TEST(denser_bean_takes_longer_with_bigger_core_lag) {
    // Same coffee, only density changed: dense beans must not heat faster.
    Catalog cat;
    cat.loadDir(rstest::dataDir());
    std::string err;
    const char* base = R"({"id":"%s","name":"x","moisture_pct":11,"bean_mass_mg":155,"fc_temp":196,"density":%d})";
    char buf[256];
    std::snprintf(buf, sizeof buf, base, "soft", 650);
    CHECK(cat.addBeanJson(buf, &err));
    std::snprintf(buf, sizeof buf, base, "hard", 760);
    CHECK(cat.addBeanJson(buf, &err));
    auto roast = [&](const char* bean) {
        std::string s = std::string(R"({"roaster":"popper_modded","seed":4,"max_s":1500,"bean":")") + bean +
                        R"(","actions":[{"at":0,"set":{"heat":100,"fan":85}},{"at":20,"charge":true},
                        {"event":"FCs","delay":60,"drop":true}]})";
        ScenarioResult r;
        runScenario(script(s.c_str()), cat, r);
        return r.report;
    };
    RoastReport soft = roast("soft"), hard = roast("hard");
    CHECK(hard.fcs_s > soft.fcs_s + 5);
    CHECK(hard.fcs_bt > soft.fcs_bt);
    CHECK(hard.agtron_ground - hard.agtron_whole > soft.agtron_ground - soft.agtron_whole);
}

TEST(crack_sounds_depend_on_the_bean) {
    // Average the sound attributes of the first 200 cracks for a few coffees.
    struct Avg { double loud = 0, size = 0; int n = 0; };
    auto cracks = [](const char* bean) {
        Simulation sim;
        sim.setSeed(3);
        sim.load(*catalog().roaster("popper_modded"), *catalog().bean(bean));
        sim.setPower(true, "test");
        sim.setControl(Control::Heat, 100);
        sim.setControl(Control::Fan, 85);
        sim.charge(100, "test");
        Avg a;
        for (int i = 0; i < 20 * 900 && a.n < 200; ++i) {
            sim.step(Simulation::kStep);
            if (sim.crackSeq() >= 200) {
                for (const auto& c : sim.cracksSince(0, 200)) {
                    CHECK(!c.second);
                    a.loud += c.loudness;
                    a.size += c.size_mm;
                    ++a.n;
                }
            }
        }
        if (a.n) { a.loud /= a.n; a.size /= a.n; }
        return a;
    };
    Avg kenya = cracks("kenya_washed"), colombia = cracks("colombia_washed"), ethiopia = cracks("ethiopia_natural"),
        decaf = cracks("decaf_water_process");
    CHECK(kenya.n == 200 && decaf.n == 200);
    CHECK(kenya.loud > colombia.loud);       // dense, wet, big beans pop loudest
    CHECK(decaf.loud < 0.6 * colombia.loud); // decaf's famously quiet first crack
    CHECK(kenya.size > ethiopia.size);       // bigger beans -> deeper pops
}

TEST(power_switch_gates_the_machine) {
    Simulation sim;
    sim.load(*catalog().roaster("popper_modded"), *catalog().bean("colombia_washed"));
    CHECK(!sim.powered());                    // machines boot switched off
    CHECK(sim.preheat(190) < 0);              // preheating needs power
    sim.setControl(Control::Heat, 100);
    sim.setControl(Control::Fan, 85);
    CHECK(sim.charge(100, "test"));           // charging is still physically possible
    for (int i = 0; i < 400; ++i) sim.step(Simulation::kStep);  // 20 s, machine off
    Snapshot off = sim.snapshot();
    CHECK(off.actuators[static_cast<int>(Control::Heat)] == 0);
    CHECK(off.actuators[static_cast<int>(Control::Fan)] == 0);
    CHECK(off.beans.T_mean < 30);             // beans stay cold
    CHECK((off.alarms & AlarmBedStalled) == 0);  // off is not "bed stalled"
    sim.setPower(true, "test");
    CHECK(sim.powered());
    for (int i = 0; i < 1200; ++i) sim.step(Simulation::kStep);  // 60 s switched on
    Snapshot on = sim.snapshot();
    CHECK(on.actuators[static_cast<int>(Control::Fan)] > 50);  // blower spun up
    CHECK(on.tele.heater_W > 100);
    CHECK(on.beans.T_mean > 30);
    // The dial settings survived the off period; a cold reset switches off and
    // puts the dials back to the preset's defaults, and so does a reload.
    CHECK(on.controls[static_cast<int>(Control::Heat)] == 100);
    sim.coldReset();
    CHECK(!sim.powered());
    for (const auto& cd : sim.roaster().controls) CHECK(sim.control(cd.id) == cd.snapToLevel(cd.def));
    CHECK(sim.snapshot().actuators[static_cast<int>(Control::Fan)] == 0);
    sim.setPower(true, "test");
    sim.load(*catalog().roaster("popper_modded"), *catalog().bean("colombia_washed"));
    CHECK(!sim.powered());
}

TEST(preheat_is_a_short_warmup_on_air_roasters) {
    auto preheat = [](const char* roaster, double target, double* et) {
        Simulation sim;
        sim.load(*catalog().roaster(roaster), *catalog().bean("colombia_washed"));
        sim.setPower(true, "test");
        double used = sim.preheat(target);
        *et = sim.snapshot().et;
        return used;
    };
    double airEt = 0, popperEt = 0, drumEt = 0;
    const double air = preheat("air_roaster", 180, &airEt);
    const double popper = preheat("popper_modded", 180, &popperEt);
    const double drum = preheat("drum_home_1kg", 190, &drumEt);
    std::printf("    preheat: air %.0f s (ET %.1f), popper %.0f s (ET %.1f), drum %.0f s (ET %.1f)\n", air, airEt,
                popper, popperEt, drum, drumEt);
    CHECK_RANGE(air, 30, 240);     // the air is hot within a few minutes
    CHECK_RANGE(popper, 30, 240);
    CHECK(drum > 2 * air);         // a drum's metal takes much longer to settle
    CHECK_RANGE(airEt, 176, 184);
    CHECK_RANGE(popperEt, 176, 184);
    CHECK_RANGE(drumEt, 186, 194);
    // A target the air can't reach: stop once it levels off at full heat.
    Simulation sim;
    sim.load(*catalog().roaster("air_roaster"), *catalog().bean("colombia_washed"));
    sim.setPower(true, "test");
    bool reached = true;
    CHECK_RANGE(sim.preheat(300, 2400, &reached), 30, 400);
    CHECK(!reached);
    CHECK(sim.events().back().detail.rfind("preheat stopped at ET", 0) == 0);
}

TEST(report_tracks_the_charged_batch) {
    Simulation sim;
    const auto& colombia = *catalog().bean("colombia_washed");
    const auto& kenya = *catalog().bean("kenya_washed");
    sim.load(*catalog().roaster("popper_modded"), colombia);
    CHECK(sim.charge(100, "test"));
    sim.setBean(kenya);
    CHECK(sim.snapshot().bean_id == kenya.id);
    CHECK(sim.report().bean == colombia.name);
    sim.markEvent("DRY", "test");
    CHECK(sim.drop("test"));
    CHECK(sim.charge(100, "test"));  // both charges share a timestamp when paused
    CHECK(sim.report().bean == kenya.name);
    CHECK(sim.report().dry_s < 0);
    CHECK(sim.report().drop_s < 0);
}

TEST(milestone_estimates_follow_roast_changes) {
    Simulation sim;
    sim.load(*catalog().roaster("popper_modded"), *catalog().bean("colombia_washed"));
    CHECK(sim.charge(100, "test"));
    sim.step(30);
    CHECK(sim.snapshot().eta_dry_s == -2);
    CHECK(sim.snapshot().eta_fc_s == -1);
    sim.markEvent("DRY", "test");
    CHECK(sim.snapshot().eta_dry_s == -1);
    sim.step(1);
    CHECK(sim.snapshot().eta_fc_s == -2);
    CHECK(sim.drop("test"));
    CHECK(sim.snapshot().eta_fc_s == -1);
    CHECK(sim.charge(100, "test"));
    CHECK(sim.snapshot().eta_dry_s == -1);
    CHECK(sim.snapshot().eta_fc_s == -1);
}

TEST(probe_open_fault_reads_minus_one) {
    ScenarioResult r = run(R"({"roaster":"popper_modded","seed":1,"max_s":60,
        "actions":[{"at":0,"set":{"heat":50,"fan":80}},{"at":10,"fault":{"probe":"BT","type":"open"}}]})");
    CHECK(!r.history.empty() && r.history.back().bt == -1.0f);
}

TEST(altitude_reduces_airflow_heat) {
    auto fcTime = [](double alt) {
        std::string s = R"({"roaster":"popper_modded","bean":"colombia_washed","seed":2,"max_s":1500,"env":{"altitude_m":)" +
                        std::to_string(alt) + R"(},"actions":[{"at":0,"set":{"heat":100,"fan":85}},{"at":20,"charge":true},
                        {"event":"FCs","delay":10,"drop":true}]})";
        ScenarioResult r;
        runScenario(script(s.c_str()), catalog(), r);
        return r.report.fcs_s;
    };
    // Thinner air carries the same heater power at a higher temperature: earlier crack.
    double sea = fcTime(0), high = fcTime(2500);
    CHECK(sea > 0 && high > 0);
    CHECK(high < sea);
}

int main(int argc, char** argv) {
    if (argc > 1) rstest::dataDir() = argv[1];
    const char* filter = argc > 2 ? argv[2] : nullptr;
    int run = 0;
    for (const auto& c : rstest::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = rstest::failures();
        std::printf("%-45s", c.name);
        std::fflush(stdout);
        c.fn();
        std::printf("%s\n", rstest::failures() == before ? "ok" : "FAILED");
        ++run;
    }
    std::printf("\n%d tests, %d failed checks\n", run, rstest::failures());
    return rstest::failures() == 0 ? 0 : 1;
}
