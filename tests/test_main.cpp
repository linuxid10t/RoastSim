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
